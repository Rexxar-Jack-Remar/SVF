#include "Cuda.h"
#include <algorithm>
#include <cuda_runtime.h>

namespace clients
{
namespace
{
void check(cudaError_t error)
{
    if (error != cudaSuccess)
        fail(std::string("CUDA: ") + cudaGetErrorString(error));
}
template <class T> struct Buffer
{
    T* data = nullptr;
    size_t capacity = 0;
    ~Buffer()
    {
        cudaFree(data);
    }
    void reserve(size_t n)
    {
        if (n <= capacity)
            return;
        check(cudaFree(data));
        check(cudaMalloc(&data, n * sizeof(T)));
        capacity = n;
    }
    void upload(const std::vector<T>& values)
    {
        reserve(values.size());
        if (!values.empty())
            check(cudaMemcpy(data, values.data(), values.size() * sizeof(T),
                             cudaMemcpyHostToDevice));
    }
};
__device__ uint64_t rowStart(uint64_t row, uint64_t n)
{
    return row * (2 * n - row - 1) / 2;
}
__device__ bool intersect(Id a, Id b, const Range* ranges, const Id* objects)
{
    if (a == Unknown || b == Unknown)
        return true;
    if (a == Empty || b == Empty)
        return false;
    if (a == b)
        return true;
    Range x = ranges[a], y = ranges[b];
    auto i = x.offset, j = y.offset;
    auto endX = i + x.size, endY = j + y.size;
    if (objects[endX - 1] < objects[j] || objects[endY - 1] < objects[i])
        return false;
    while (i < endX && j < endY)
    {
        if (objects[i] == objects[j])
            return true;
        if (objects[i] < objects[j])
            ++i;
        else
            ++j;
    }
    return false;
}
__global__ void answerPairs(uint64_t n, uint64_t base, Id count, const Id* rows,
                            const Id* groups, const Range* ranges,
                            const Id* objects, unsigned char* answers)
{
    for (uint64_t t = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
         t < count; t += uint64_t(blockDim.x) * gridDim.x)
    {
        uint64_t k = base + t, low = 0, high = n - 1;
        while (low + 1 < high)
        {
            uint64_t mid = (low + high) / 2;
            if (rowStart(mid, n) <= k)
                low = mid;
            else
                high = mid;
        }
        uint64_t j = low + 1 + k - rowStart(low, n);
        answers[t] =
            intersect(groups[rows[low]], groups[rows[j]], ranges, objects);
    }
}
} // namespace
struct GpuIndex::State
{
    Buffer<Range> ranges;
    Buffer<Id> objects, groups, rows;
    Buffer<unsigned char> answers;
    size_t n = 0;
    Times* times;
    cudaEvent_t before, after;
};
GpuIndex::GpuIndex(const Index& index, const std::vector<Id>& groups, Id device,
                   Id tile, Times& times)
    : state(new State)
{
    check(cudaSetDevice(device));
    state->times = &times;
    check(cudaEventCreate(&state->before));
    check(cudaEventCreate(&state->after));
    auto start = Clock::now();
    state->ranges.upload(index.groups);
    state->objects.upload(index.objects);
    state->groups.upload(groups);
    state->times->gpuCopy += seconds(start);
    state->answers.reserve(tile);
    state->times->deviceBytes =
        state->ranges.capacity * sizeof(Range) +
        (state->objects.capacity + state->groups.capacity) * sizeof(Id) +
        state->answers.capacity;
}
GpuIndex::~GpuIndex()
{
    cudaEventDestroy(state->before);
    cudaEventDestroy(state->after);
    delete state;
}
void GpuIndex::candidates(const std::vector<Id>& rows)
{
    auto start = Clock::now();
    state->rows.upload(rows);
    state->times->gpuCopy += seconds(start);
    state->n = rows.size();
    state->times->deviceBytes =
        state->ranges.capacity * sizeof(Range) +
        (state->objects.capacity + state->groups.capacity +
         state->rows.capacity) *
            sizeof(Id) +
        state->answers.capacity;
}
void GpuIndex::query(uint64_t base, Id count, unsigned char* answers)
{
    unsigned blocks = std::min<uint64_t>(65535, (uint64_t(count) + 255) / 256);
    check(cudaEventRecord(state->before));
    answerPairs<<<blocks, 256>>>(state->n, base, count, state->rows.data,
                                 state->groups.data, state->ranges.data,
                                 state->objects.data, state->answers.data);
    check(cudaGetLastError());
    check(cudaEventRecord(state->after));
    check(cudaEventSynchronize(state->after));
    float ms = 0;
    check(cudaEventElapsedTime(&ms, state->before, state->after));
    state->times->gpuKernel += ms / 1000.0;
    auto start = Clock::now();
    check(cudaMemcpy(answers, state->answers.data, count,
                     cudaMemcpyDeviceToHost));
    state->times->gpuCopy += seconds(start);
}
} // namespace clients
