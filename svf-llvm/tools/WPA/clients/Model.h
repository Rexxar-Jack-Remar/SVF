#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace SVF
{
class BVDataPTAImpl;
}

namespace clients
{
using Id = uint32_t;
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

constexpr Id Empty = UINT32_MAX;
constexpr Id Unknown = UINT32_MAX - 1;
struct Range
{
    uint64_t offset, size;
};
struct Index
{
    std::vector<Id> objects;
    std::vector<Range> groups;
    std::map<Id, Id> pointerGroup;
    bool alias(Id a, Id b) const;
};
struct Site
{
    Id block, order;
    std::string label;
};
struct Port
{
    Id site, pointer, value;
};
struct Endpoint
{
    Id site, value;
};

// A flow vertex denotes an SSA value or an operation-specific transfer,
// never the abstract object addressed by the SSA value.
struct Model
{
    std::vector<std::vector<Id>> cfg;
    std::vector<Site> sites;
    std::vector<std::vector<Id>> flow;
    std::vector<Id> flowSite;
    std::vector<Port> reads, writes, frees;
    std::vector<Endpoint> sources, sinks;
    std::map<std::string, uint64_t> unknownCalls;
    Index index;
};
struct Times
{
    double graph = 0, index = 0, query = 0, consume = 0, solve = 0, report = 0;
    std::string baselineIndex = "none";
    uint64_t deviceBytes = 0;
    double gpuKernel = 0, gpuCopy = 0;
    uint64_t candidates = 0, pairs = 0, hits = 0, tiles = 0, reports = 0;
};
Model extract(SVF::BVDataPTAImpl&, Times&);
void save(const Model&, const std::string&);
Model load(const std::string&);
void fail(const std::string&);
void analyze(const Model&, SVF::BVDataPTAImpl* pta, bool uaf,
             const std::string& backend, Id device, Id tile, bool verify,
             std::ostream&, Times&);
} // namespace clients
