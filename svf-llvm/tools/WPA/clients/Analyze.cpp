#include "Model.h"
#include "Query.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <deque>
#include <numeric>
#include <ostream>
#include <set>
#include <unordered_map>

namespace clients
{
namespace
{
using Adjacency = std::vector<std::vector<Id>>;

class Control
{
    const Model& m;
    Adjacency reverse, dag;
    std::vector<Id> scc;
    std::vector<bool> cyclic;
    // One traversed condensation-DAG row at a time bounds auxiliary memory.
    Id cached = Unknown;
    std::vector<Id> reached;

public:
    std::vector<Id> region;
    explicit Control(const Model& model)
        : m(model), reverse(m.cfg.size()), scc(m.cfg.size(), Unknown),
          region(m.cfg.size(), Unknown)
    {
        for (Id i = 0; i < m.cfg.size(); ++i)
            for (Id j : m.cfg[i])
                reverse[j].push_back(i);
        std::vector<Id> finish;
        std::vector<bool> seen(m.cfg.size());
        for (Id i = 0; i < m.cfg.size(); ++i)
        {
            if (seen[i])
                continue;
            std::vector<std::pair<Id, size_t>> stack{{i, 0}};
            seen[i] = true;
            while (!stack.empty())
            {
                auto& [v, k] = stack.back();
                if (k == m.cfg[v].size())
                {
                    finish.push_back(v);
                    stack.pop_back();
                    continue;
                }
                Id w = m.cfg[v][k++];
                if (!seen[w])
                {
                    seen[w] = true;
                    stack.emplace_back(w, 0);
                }
            }
        }
        for (auto it = finish.rbegin(); it != finish.rend(); ++it)
        {
            if (scc[*it] != Unknown)
                continue;
            Id component = dag.size();
            dag.emplace_back();
            std::vector<Id> work{*it};
            scc[*it] = component;
            for (size_t k = 0; k < work.size(); ++k)
                for (Id v : reverse[work[k]])
                    if (scc[v] == Unknown)
                    {
                        scc[v] = component;
                        work.push_back(v);
                    }
            cyclic.push_back(work.size() > 1);
        }
        reached.resize(dag.size(), Unknown);
        for (Id v = 0; v < m.cfg.size(); ++v)
            for (Id w : m.cfg[v])
            {
                if (v == w)
                    cyclic[scc[v]] = true;
                if (scc[v] != scc[w])
                    dag[scc[v]].push_back(scc[w]);
            }
        for (Id i = 0; i < region.size(); ++i)
        {
            if (region[i] != Unknown)
                continue;
            region[i] = i;
            std::vector<Id> work{i};
            for (size_t k = 0; k < work.size(); ++k)
            {
                for (auto* edges :
                     {&m.cfg[work[k]],
                      static_cast<const std::vector<Id>*>(&reverse[work[k]])})
                    for (Id w : *edges)
                        if (region[w] == Unknown)
                        {
                            region[w] = i;
                            work.push_back(w);
                        }
            }
        }
    }
    bool after(Id from, Id to)
    {
        auto& a = m.sites[from];
        auto& b = m.sites[to];
        Id x = scc[a.block], y = scc[b.block];
        if (x == y)
            return cyclic[x] || a.order < b.order;
        if (x > y || region[a.block] != region[b.block])
            return false;
        if (cached != x)
        {
            // Reset between origins: a previously visited row may be queried
            // again.
            std::fill(reached.begin(), reached.end(), Unknown);
            std::vector<Id> work{x};
            reached[x] = x;
            for (size_t k = 0; k < work.size(); ++k)
                for (Id child : dag[work[k]])
                    if (reached[child] != x)
                    {
                        reached[child] = x;
                        work.push_back(child);
                    }
            cached = x;
        }
        return reached[y] == x;
    }
};

using Relations = std::unordered_map<Id, llvm::SparseBitVector<>>;
Relations relations(const Model& m, const Control& cfg, bool uaf, Query& query)
{
    enum
    {
        Left = 1,
        Right = 2
    };
    std::map<Id, std::map<Id, unsigned>> regions;
    auto include = [&](const std::vector<Port>& ports, unsigned role) {
        for (auto p : ports)
            regions[cfg.region[m.sites[p.site].block]][p.pointer] |= role;
    };
    include(uaf ? m.frees : m.writes, Left);
    include(m.reads, Right);
    if (uaf)
        include(m.writes, Right);
    std::set<Id> active;
    for (auto e : m.sources)
        active.insert(cfg.region[m.sites[e.site].block]);
    Relations result;
    for (const auto& entry : regions)
    {
        Id region = entry.first;
        const auto& roles = entry.second;
        if (!uaf && !active.count(region))
            continue;
        // Cheap role-aware pruning: an address disjoint from the union on
        // the opposite side can never contribute a free/use or write/read edge.
        std::set<Id> leftObjects, rightObjects;
        bool leftUnknown = false, rightUnknown = false;
        auto collect = [&](unsigned role, std::set<Id>& objects,
                           bool& unknown) {
            std::set<Id> groups;
            for (auto [p, mask] : roles)
                if (mask & role)
                    groups.insert(m.index.pointerGroup.at(p));
            unknown = groups.count(Unknown);
            if (unknown)
                return;
            for (Id g : groups)
                if (g != Empty)
                {
                    auto r = m.index.groups[g];
                    auto first = m.index.objects.begin() + r.offset;
                    objects.insert(first, first + r.size);
                }
        };
        collect(Left, leftObjects, leftUnknown);
        collect(Right, rightObjects, rightUnknown);
        auto overlaps = [&](Id g, const std::set<Id>& objects, bool unknown) {
            if (unknown || g == Unknown)
                return true;
            if (g == Empty)
                return false;
            auto r = m.index.groups[g];
            for (uint64_t i = r.offset; i < r.offset + r.size; ++i)
                if (objects.count(m.index.objects[i]))
                    return true;
            return false;
        };
        bool left = false, right = false;
        std::vector<Id> pointers;
        std::unordered_map<Id, unsigned> mask;
        for (auto [p, role] : roles)
        {
            Id g = m.index.pointerGroup.at(p);
            if ((role & Left) && !overlaps(g, rightObjects, rightUnknown))
                role &= ~Left;
            if ((role & Right) && !overlaps(g, leftObjects, leftUnknown))
                role &= ~Right;
            if (!role)
                continue;
            left |= bool(role & Left);
            right |= bool(role & Right);
            pointers.push_back(p);
            mask.emplace(p, role);
        }
        if (!left || !right)
            continue;
        query.serve(pointers, [&](Id a, Id b) {
            auto keep = [&](Id writer, Id reader) {
                if ((mask.at(writer) & Left) && (mask.at(reader) & Right))
                    result[writer].set(reader);
            };
            keep(a, b);
            if (a != b)
                keep(b, a);
        });
    }
    return result;
}

void report(const Model& m, const char* kind, Id from, Id to,
            std::vector<Id> path, std::ostream& out, Times& times)
{
    auto start = Clock::now();
    llvm::json::Array labels;
    Id last = Unknown;
    for (Id site : path)
        if (site != Unknown && site != last)
        {
            labels.emplace_back(m.sites[site].label);
            last = site;
        }
    llvm::json::Object record{{"client", kind},
                              {"from", m.sites[from].label},
                              {"to", m.sites[to].label},
                              {"from_id", from},
                              {"to_id", to},
                              {"path", std::move(labels)}};
    out << llvm::formatv("{0}", llvm::json::Value(std::move(record))).str()
        << '\n';
    ++times.reports;
    times.report += seconds(start);
}

void uaf(const Model& m, Control& cfg, const Relations& alias,
         std::ostream& out, Times& times)
{
    std::unordered_map<Id, std::vector<Id>> uses;
    for (auto* list : {&m.reads, &m.writes})
        for (auto p : *list)
            uses[p.pointer].push_back(p.site);
    std::vector<Id> firstSite(m.cfg.size(), Unknown);
    for (Id i = 0; i < m.sites.size(); ++i)
        if (firstSite[m.sites[i].block] == Unknown)
            firstSite[m.sites[i].block] = i;
    std::map<Id, std::vector<Id>> released;
    for (auto f : m.frees)
        released[f.site].push_back(f.pointer);
    for (auto& [site, pointers] : released)
    {
        std::set<Id> hits;
        for (Id p : pointers)
        {
            auto row = alias.find(p);
            if (row == alias.end())
                continue;
            for (Id q : row->second)
                for (Id use : uses.at(q))
                    if (cfg.after(site, use))
                        hits.insert(use);
        }
        if (hits.empty())
            continue;
        Id origin = m.sites[site].block;
        std::vector<Id> parent(m.cfg.size(), Unknown), queue;
        for (Id child : m.cfg[origin])
        {
            parent[child] = origin;
            queue.push_back(child);
        }
        for (size_t k = 0; k < queue.size(); ++k)
            for (Id child : m.cfg[queue[k]])
                if (parent[child] == Unknown)
                {
                    parent[child] = queue[k];
                    queue.push_back(child);
                }
        // Many uses of the same release share nearly the entire CFG witness.
        // Encode their predecessor tree once, retaining every block and use.
        auto reporting = Clock::now();
        llvm::json::Array blocks, usesAfter;
        std::vector<Id> ref(m.cfg.size(), Unknown), suffix;
        for (Id use : hits)
        {
            Id block = m.sites[use].block;
            int64_t end = -1;
            if (origin != block || m.sites[site].order >= m.sites[use].order)
            {
                suffix.clear();
                Id v = block;
                while (ref[v] == Unknown)
                {
                    suffix.push_back(v);
                    if (parent[v] == origin)
                        break;
                    v = parent[v];
                }
                for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
                {
                    Id child = *it;
                    int64_t prev = parent[child] == origin
                                       ? int64_t(-1)
                                       : int64_t(ref[parent[child]]);
                    ref[child] = blocks.size();
                    blocks.emplace_back(llvm::json::Array{
                        prev, child, m.sites[firstSite[child]].label});
                }
                end = ref[block];
            }
            usesAfter.emplace_back(
                llvm::json::Array{use, m.sites[use].label, end});
        }
        llvm::json::Object record{{"client", "uaf"},
                                  {"from", m.sites[site].label},
                                  {"from_id", site},
                                  {"blocks", std::move(blocks)},
                                  {"uses", std::move(usesAfter)}};
        out << llvm::formatv("{0}", llvm::json::Value(std::move(record))).str()
            << '\n';
        times.reports += hits.size();
        times.report += seconds(reporting);
    }
}

struct Step
{
    Id vertex, store, read;
};
void taint(const Model& m, Control& cfg, const Relations& alias,
           std::ostream& out, Times& times)
{
    std::unordered_map<Id, std::vector<Port>> reads, writes;
    for (auto r : m.reads)
        reads[r.pointer].push_back(r);
    for (auto w : m.writes)
        writes[w.value].push_back(w);
    std::map<Id, std::vector<Id>> sources;
    for (auto e : m.sources)
        sources[e.site].push_back(e.value);
    for (auto& [site, roots] : sources)
    {
        std::vector<Step> parent(m.flow.size(), {Unknown, Unknown, Unknown});
        std::vector<Id> work;
        auto add = [&](Id v, Step step) {
            if (parent[v].vertex != Unknown)
                return;
            parent[v] = step;
            work.push_back(v);
        };
        for (Id root : roots)
            add(root, {root, Unknown, Unknown});
        for (size_t k = 0; k < work.size(); ++k)
        {
            Id v = work[k];
            for (Id w : m.flow[v])
                add(w, {v, Unknown, Unknown});
            auto ws = writes.find(v);
            if (ws == writes.end())
                continue;
            for (const Port& w : ws->second)
            {
                auto row = alias.find(w.pointer);
                if (row == alias.end())
                    continue;
                for (Id q : row->second)
                    for (const Port& r : reads.at(q))
                        if (parent[r.value].vertex == Unknown &&
                            cfg.after(w.site, r.site))
                            add(r.value, {v, w.site, r.site});
            }
        }
        std::set<Id> emitted;
        for (auto sink : m.sinks)
        {
            if (parent[sink.value].vertex == Unknown ||
                !emitted.insert(sink.site).second)
                continue;
            std::vector<Id> path{sink.site};
            for (Id v = sink.value;; v = parent[v].vertex)
            {
                path.push_back(m.flowSite[v]);
                if (parent[v].vertex == v)
                    break;
                path.push_back(parent[v].read);
                path.push_back(parent[v].store);
            }
            path.push_back(site);
            std::reverse(path.begin(), path.end());
            report(m, "taint", site, sink.site, std::move(path), out, times);
        }
    }
}
} // namespace
void analyze(const Model& m, bool isUaf, const std::string& backend, Id device,
             Id tile, bool verify, std::ostream& out, Times& times)
{
    auto start = Clock::now();
    Query query(m.index, backend, device, tile, verify, times);
    times.index += seconds(start);
    start = Clock::now();
    Control cfg(m);
    auto alias = relations(m, cfg, isUaf, query);
    times.graph += seconds(start) - times.query - times.consume;
    start = Clock::now();
    if (isUaf)
        uaf(m, cfg, alias, out, times);
    else
        taint(m, cfg, alias, out, times);
    out.flush();
    if (!out)
        fail("writing client reports failed");
    times.solve = seconds(start) - times.report;
}
} // namespace clients
