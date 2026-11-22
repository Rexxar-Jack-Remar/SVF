#pragma once
#include "Model.h"
namespace clients
{
class GpuIndex
{
    struct State;
    State* state;

public:
    GpuIndex(const Index&, const std::vector<Id>& groups, Id device, Id tile,
             Times&);
    ~GpuIndex();
    GpuIndex(const GpuIndex&) = delete;
    GpuIndex& operator=(const GpuIndex&) = delete;
    void candidates(const std::vector<Id>& rows);
    void query(uint64_t base, Id count, unsigned char* answers);
};
} // namespace clients
