// MIT License
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "common_benchmark_header.hpp"

// HIP API
#include "hipcub/warp/warp_store.hpp"


#ifndef DEFAULT_N
const size_t DEFAULT_N = 1024 * 1024 * 32;
#endif

template<
    class T,
    unsigned BlockSize,
    unsigned ItemsPerThread,
    unsigned LogicalWarpSize,
    ::hipcub::WarpStoreAlgorithm Algorithm,
    unsigned Trials
>
__global__
__launch_bounds__(BlockSize)
void warp_store_kernel(T* d_input, T* d_output)
{
    int thread_data[ItemsPerThread];
    for (unsigned i = 0; i < ItemsPerThread; ++i)
    {
        thread_data[i] = d_input[hipThreadIdx_x * ItemsPerThread + i];
    }

    using WarpStoreT = ::hipcub::WarpStore<
        T,
        ItemsPerThread,
        Algorithm,
        LogicalWarpSize
    >;
    constexpr unsigned warps_in_block = BlockSize / LogicalWarpSize;
    constexpr int tile_size = ItemsPerThread * LogicalWarpSize;
    __shared__ typename WarpStoreT::TempStorage temp_storage[warps_in_block];
    unsigned warp_id = hipThreadIdx_x / LogicalWarpSize;

    #pragma nounroll
    for (unsigned trial = 0; trial < Trials; ++trial)
    {
        WarpStoreT(temp_storage[warp_id]).Store(d_output + warp_id * tile_size, thread_data);
    }

}

template<
    class T,
    unsigned BlockSize,
    unsigned ItemsPerThread,
    unsigned LogicalWarpSize,
    ::hipcub::WarpStoreAlgorithm Algorithm,
    unsigned Trials = 100
>
void run_benchmark(benchmark::State& state, hipStream_t stream, size_t N)
{
    constexpr unsigned items_per_block = BlockSize * ItemsPerThread;
    const unsigned size = items_per_block * ((N + items_per_block - 1) / items_per_block);

    std::vector<T> input = benchmark_utils::get_random_data<T>(size, T(0), T(10));
    T * d_input;
    T * d_output;
    HIP_CHECK(hipMalloc(&d_input, size * sizeof(T)));
    HIP_CHECK(hipMalloc(&d_output, size * sizeof(T)));
    HIP_CHECK(
        hipMemcpy(
            d_input, input.data(),
            size * sizeof(T),
            hipMemcpyHostToDevice
        )
    );

    for (auto _ : state)
    {
        auto start = std::chrono::high_resolution_clock::now();
        
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(warp_store_kernel<
                T,
                BlockSize,
                ItemsPerThread,
                LogicalWarpSize,
                Algorithm,
                Trials
            >),
            dim3(size / items_per_block), dim3(BlockSize), 0, stream, d_input, d_output
        );
        HIP_CHECK(hipPeekAtLastError())
        HIP_CHECK(hipDeviceSynchronize());
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
        state.SetIterationTime(elapsed_seconds.count());
    }
    state.SetBytesProcessed(state.iterations() * Trials * size * sizeof(T));
    state.SetItemsProcessed(state.iterations() * Trials * size);

    HIP_CHECK(hipFree(d_input));
    HIP_CHECK(hipFree(d_output));
}

#define CREATE_BENCHMARK(T, BS, IT, WS, ALG) \
benchmark::RegisterBenchmark( \
    "warp_store<" #T ", " #BS ", " #IT ", " #WS ", " #ALG ">.", \
    &run_benchmark<T, BS, IT, WS, ALG>, \
    stream, size \
)

int main(int argc, char *argv[])
{
    cli::Parser parser(argc, argv);
    parser.set_optional<size_t>("size", "size", DEFAULT_N, "number of values");
    parser.set_optional<int>("trials", "trials", -1, "number of iterations");
    parser.run_and_exit_if_error();

    // Parse argv
    benchmark::Initialize(&argc, argv);
    const size_t size = parser.get<size_t>("size");
    const int trials = parser.get<int>("trials");

    // HIP
    hipStream_t stream = 0; // default
    hipDeviceProp_t devProp;
    int device_id = 0;
    HIP_CHECK(hipGetDevice(&device_id));
    HIP_CHECK(hipGetDeviceProperties(&devProp, device_id));
    std::cout << "[HIP] Device name: " << devProp.name << std::endl;

    // Add benchmarks
    std::vector<benchmark::internal::Benchmark*> benchmarks{
        CREATE_BENCHMARK(int, 128, 4, 32, ::hipcub::WARP_STORE_DIRECT),
        CREATE_BENCHMARK(int, 128, 4, 32, ::hipcub::WARP_STORE_STRIPED),
        CREATE_BENCHMARK(int, 128, 4, 32, ::hipcub::WARP_STORE_VECTORIZE),
        CREATE_BENCHMARK(int, 128, 4, 32, ::hipcub::WARP_STORE_TRANSPOSE),
        CREATE_BENCHMARK(int, 256, 4, 32, ::hipcub::WARP_STORE_DIRECT),
        CREATE_BENCHMARK(int, 256, 4, 32, ::hipcub::WARP_STORE_STRIPED),
        CREATE_BENCHMARK(int, 256, 4, 32, ::hipcub::WARP_STORE_VECTORIZE),
        CREATE_BENCHMARK(int, 256, 4, 32, ::hipcub::WARP_STORE_TRANSPOSE)
    };

    // Use manual timing
    for (auto& b : benchmarks)
    {
        b->UseManualTime();
        b->Unit(benchmark::kMillisecond);
    }

    // Force number of iterations
    if (trials > 0)
    {
        for (auto& b : benchmarks)
        {
            b->Iterations(trials);
        }
    }

    // Run benchmarks
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
