#pragma once
#include "Model.h"
#include <functional>
#include <memory>

namespace clients
{
using Match = std::function<void(Id, Id)>;
class Query
{
    struct State;
    std::unique_ptr<State> state;

public:
    Query(const Index&, const std::string& backend, Id device, Id tile,
          bool verify, Times&);
    ~Query();
    void serve(const std::vector<Id>& pointers, const Match& match);
};
} // namespace clients
