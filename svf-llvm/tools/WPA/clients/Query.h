#pragma once
#include "Model.h"
#include "llvm/ADT/ArrayRef.h"
#include <functional>
#include <memory>

namespace clients
{
// The read-only answer view is valid for the duration of the callback.
using ConsumeTile =
    std::function<void(uint64_t base, llvm::ArrayRef<unsigned char> answers)>;
class Query
{
    struct State;
    std::unique_ptr<State> state;

public:
    Query(const Index&, const std::string& backend, Id device, Id tile,
          bool verify, Times&);
    ~Query();
    void serve(const std::vector<Id>& pointers, const ConsumeTile& consume);
};
} // namespace clients
