#pragma once
#include "Model.h"
#include "llvm/ADT/ArrayRef.h"
#include <functional>
#include <memory>

namespace clients
{
bool nativeMayAlias(SVF::BVDataPTAImpl& pta, Id a, Id b);

// The read-only answer view is valid for the duration of the callback.
using ConsumeTile =
    std::function<void(uint64_t base, llvm::ArrayRef<unsigned char> answers)>;
class Query
{
    struct State;
    std::unique_ptr<State> state;

public:
    Query(const Index&, SVF::BVDataPTAImpl* pta, const std::string& backend,
          Id device, Id tile, bool verify, Times&);
    ~Query();
    void serve(const std::vector<Id>& pointers, const ConsumeTile& consume);
};
} // namespace clients
