#include "Cuda.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>

namespace clients
{
void fail(const std::string& message)
{
    std::cerr << message << '\n';
    std::exit(1);
}
} // namespace clients

// Probe chosen intervals of a 5-billion-pair triangle without materializing it.
int main()
{
    using namespace clients;
    Index index;
    index.objects = {3, 7, 7, 11, 42};
    index.groups = {{0, 2}, {2, 2}, {4, 1}};
    std::vector<Id> groups{0, 1, 2, Empty, Unknown};
    Times times;
    GpuIndex gpu(index, groups, 0, 9, times);
    for (uint64_t n : {uint64_t(100003), uint64_t(13)})
    {
        std::vector<Id> rows(n);
        for (uint64_t i = 0; i < n; ++i)
            rows[i] = (3 * i + 1) % groups.size();
        gpu.candidates(rows);
        for (uint64_t row : {uint64_t(0), n / 2, n - 3})
        {
            uint64_t start = row * (2 * n - row - 1) / 2;
            for (uint64_t base : {start, start + n - row - 2})
            {
                unsigned char answers[9];
                Id count = std::min<uint64_t>(9, n * (n - 1) / 2 - base);
                gpu.query(base, count, answers);
                uint64_t i = row, j = i + 1 + (base - start);
                for (Id k = 0; k < count; ++k)
                {
                    Id a = groups[rows[i]], b = groups[rows[j]];
                    bool expected = a == Unknown || b == Unknown ||
                                    (a != Empty && b != Empty &&
                                     (a == b || (a < 2 && b < 2)));
                    if (answers[k] != expected)
                        fail("wrong answer at logical pair " +
                             std::to_string(base + k));
                    if (++j == n)
                        j = ++i + 1;
                }
            }
        }
    }
}
