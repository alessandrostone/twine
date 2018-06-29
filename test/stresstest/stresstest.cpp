#include <random>
#include <array>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>

#include <getopt.h>
#include <sys/mman.h>


#ifdef TWINE_BUILD_WITH_XENOMAI
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cobalt/pthread.h>
#include <xenomai/init.h>
#pragma GCC diagnostic pop
#endif

#include "twine.h"
#include "twine_internal.h"

/*
 * Tool for stress testing WorkerPool implementations with
 * variable number of cores and worker threads.
 */

constexpr int DEFAULT_CORES = 4;
constexpr int DEFAULT_WORKERS = 10;
constexpr int MAX_LOAD = 300;
constexpr int MIN_LOAD = 20;
constexpr int DEFAULT_ITERATIONS = 10000;

/* iir parameters: */
constexpr float CUTOFF = 0.2f;
constexpr float Q = 0.5f;
constexpr float w0 = 2.0f * M_PI * CUTOFF;
constexpr float w0_cos = std::cos(w0);
constexpr float w0_sin = std::sin(w0);
constexpr float alpha = w0_sin / Q;
constexpr float norm = 1.0f / (1.0f + alpha);

constexpr float co_a1 = -2.0f * w0_cos * norm;
constexpr float co_a2 = (1 - alpha) * norm;
constexpr float co_b0 = (1.0f - w0_cos) / 2.0f * norm;;
constexpr float co_b1 = (1 - w0_cos) * norm;
constexpr float co_b2 = co_b0;

typedef std::array<float, 128> AudioBuffer;
typedef std::array<float, 2> FilterRegister;

/* Biquad implementation to keep the cpu busy */
template <size_t length>
void process_filter(std::array<float, length>& buffer, FilterRegister& mem)
{
    for (unsigned int i = 0; i < length; ++i)
    {
        float w = buffer[i] - co_a1 * mem[0] - co_a2 * mem[1];
        buffer[i] = co_b1 * mem[0] + co_b2 * mem[1] + co_b0 * w;
        mem[1] = mem[0];
        mem[0] = w;
    }
}

struct ProcessData
{
    AudioBuffer buffer;
    FilterRegister mem;
};

void worker_function(void* data)
{
    auto process_data = reinterpret_cast<ProcessData*>(data);
    int iters = std::rand() % (MAX_LOAD - MIN_LOAD) + MIN_LOAD; // quick and dirty rand function is enough here
    for (int i = 0; i < iters; ++i)
    {
        process_filter(process_data->buffer, process_data->mem);
    }
}

std::string to_error_string(twine::WorkerPoolStatus status)
{
    switch (status)
    {
        case twine::WorkerPoolStatus::PERMISSION_DENIED:
            return "Permission denied";

        case twine::WorkerPoolStatus::LIMIT_EXCEEDED:
            return "Thread count limit exceeded";

        default:
            return "Error";
    }
}

#ifdef TWINE_BUILD_WITH_XENOMAI
void xenomai_thread_init()
{
    // For some obscure reasons, xenomai_init() crashes
    // if argv is allocated here on the stack, so we malloc it
    // beforehand.
    int argc = 1;
    char** argv = (char**) malloc(2 * sizeof(char*));
    argv[0] = (char*) malloc(32 * sizeof(char));
    argv[1] = nullptr;
    strcpy(argv[0], "stress_test");
    optind = 1;
    xenomai_init(&argc, (char* const**) &argv);
    free(argv[0]);
    free(argv);
    mlockall(MCL_CURRENT|MCL_FUTURE);
    twine::init_xenomai();
}
#endif
#ifndef TWINE_BUILD_WITH_XENOMAI
void xenomai_thread_init()
{ }
#endif


std::tuple<int, int, int, bool> parse_opts(int argc, char** argv)
{
    int workers = DEFAULT_WORKERS;
    int cores = DEFAULT_CORES;
    int iters = DEFAULT_ITERATIONS;
    bool xenomai = false;
    char c;

    while ((c = getopt(argc, argv, "w:c:i:x")) != -1)
    {
        switch (c)
        {
            case 'w':
                workers = atoi(optarg);
                break;
            case 'c':
                cores = atoi(optarg);
                break;
            case 'i':
                iters = atoi(optarg);
                break;
            case 'x':
                if (!xenomai)
                {
                    xenomai_thread_init();
                    xenomai = true;
                }
                break;
            case '?':
                std::cout << "Options are: -w[n of worker threads], -c[n of cores], -i[n of iterations], -x - use xenomai threads" << std::endl;
                [[fallthrough]]
            default:
                abort();
        }
    }
    return std::make_tuple(workers, cores, iters, xenomai);
}

void* run_stress_test(void* data)
{
    auto [pool, test_data, iters, xenomai] = *(reinterpret_cast<std::tuple<twine::WorkerPool*,
                                               std::vector<ProcessData>*, int, bool>*>(data));
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-1, 1);
    for (int i = 0; i < iters; ++i)
    {
        pool->wait_for_workers_idle();
        // Fill buffers with random numbers
        for (auto& p : *test_data) {for (auto& b : p.buffer) {b = dist(gen);}}

        // Run all workers
        pool->wakeup_workers();
        if ((i + 1) % 10 == 0)
        {
            if (xenomai)
            {
#ifdef TWINE_BUILD_WITH_XENOMAI
                __cobalt_printf("\rIterations: %i", i);
#endif
            }
            else
            {
                std::cout << "\rIterations: " << i;
                std::cout.flush();
            }
        }
    }
    return nullptr;
}

void run_stress_test_in_xenomai_thread(void* data)
{
#ifdef TWINE_BUILD_WITH_XENOMAI
        /* Threadpool must be controlled from another xenomai thread */
        struct sched_param rt_params = { .sched_priority = 70 };
        pthread_attr_t task_attributes;
        __cobalt_pthread_attr_init(&task_attributes);

        pthread_attr_setdetachstate(&task_attributes, PTHREAD_CREATE_JOINABLE);
        pthread_attr_setinheritsched(&task_attributes, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&task_attributes, SCHED_FIFO);
        pthread_attr_setschedparam(&task_attributes, &rt_params);
        pthread_t thread;

        auto res = __cobalt_pthread_create(&thread, &task_attributes, &run_stress_test, data);
        if (res != 0)
        {
            std::cout << "Failed to start xenomai thread: " << strerror(res) <<std::endl;
        }
        /* Wait for the xenomai thread to finish */
        __cobalt_pthread_join(thread, nullptr);
#endif
}

int main(int argc, char **argv)
{
    auto [workers, cores, iters, xenomai] = parse_opts(argc, argv);

    std::vector<ProcessData> data;
    data.reserve(DEFAULT_ITERATIONS);
    auto worker_pool = twine::WorkerPool::CreateWorkerPool(cores);
    for (int i = 0; i < workers; ++i)
    {
        ProcessData d;
        d.mem = {0,0};
        data.push_back(d);
        auto res = worker_pool->add_worker(worker_function, &data[i]);
        if (res != twine::WorkerPoolStatus::OK)
        {
            std::cout << "Failed to start workers: " << to_error_string(res) << std::endl;
            return -1;
        }
    }
    auto test_data = std::make_tuple(worker_pool.get(), &data, iters, xenomai);
    if (xenomai)
    {
        run_stress_test_in_xenomai_thread(&test_data);

    } else
    {
        run_stress_test(&test_data);
    }

    return 0;
}