#include "Model.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace clients
{
void fail(const std::string& msg)
{
    std::cerr << "alias-client: " << msg << '\n';
    std::exit(EXIT_FAILURE);
}
bool Index::alias(Id a, Id b) const
{
    if (a == Unknown || b == Unknown)
        return true;
    if (a == Empty || b == Empty)
        return false;
    if (a == b)
        return true;
    auto x = groups[a], y = groups[b];
    if (objects[x.offset + x.size - 1] < objects[y.offset] ||
        objects[y.offset + y.size - 1] < objects[x.offset])
        return false;
    auto i = x.offset, j = y.offset;
    while (i < x.offset + x.size && j < y.offset + y.size)
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

void save(const Model& m, const std::string& path)
{
    llvm::SmallString<256> tmp;
    int fd;
    auto ec = llvm::sys::fs::createUniqueFile(path + ".%%%%%%", fd, tmp);
    if (ec)
        fail(ec.message());
    llvm::raw_fd_ostream out(fd, true);
    // Strings are length-prefixed to support whitespace and arbitrary IR names.
    auto string = [&](const std::string& s) {
        out << s.size() << ' ' << s << '\n';
    };
    auto ids = [&](const std::vector<Id>& v) {
        out << v.size();
        for (Id id : v)
            out << ' ' << id;
        out << '\n';
    };
    auto edges = [&](const std::vector<std::vector<Id>>& graph) {
        out << graph.size() << '\n';
        for (const auto& row : graph)
            ids(row);
    };
    out << "SVF-CLIENT-1\n";
    edges(m.cfg);
    out << m.sites.size() << '\n';
    for (const Site& s : m.sites)
    {
        out << s.block << ' ' << s.order << ' ';
        string(s.label);
    }
    edges(m.flow);
    ids(m.flowSite);
    for (const auto* ports : {&m.reads, &m.writes, &m.frees})
    {
        out << ports->size() << '\n';
        for (auto p : *ports)
            out << p.site << ' ' << p.pointer << ' ' << p.value << '\n';
    }
    for (const auto* ends : {&m.sources, &m.sinks})
    {
        out << ends->size() << '\n';
        for (auto e : *ends)
            out << e.site << ' ' << e.value << '\n';
    }
    out << m.unknownCalls.size() << '\n';
    for (const auto& [name, n] : m.unknownCalls)
    {
        out << n << ' ';
        string(name);
    }
    ids(m.index.objects);
    out << m.index.groups.size() << '\n';
    for (auto g : m.index.groups)
        out << g.offset << ' ' << g.size << '\n';
    out << m.index.pointerGroup.size() << '\n';
    for (auto [p, g] : m.index.pointerGroup)
        out << p << ' ' << g << '\n';
    out.close();
    if (out.has_error() || std::rename(tmp.c_str(), path.c_str()))
    {
        llvm::sys::fs::remove(tmp);
        fail("could not publish snapshot " + path);
    }
}

Model load(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        fail("cannot open snapshot " + path);
    uint64_t bytes = in.tellg();
    in.seekg(0);
    std::string header;
    std::getline(in, header);
    if (header != "SVF-CLIENT-1")
        fail("unsupported snapshot format");
    auto number = [&](uint64_t max) {
        uint64_t n;
        if (!(in >> n) || n > max)
            fail("invalid snapshot integer in " + path);
        return n;
    };
    auto count = [&]() {
        return number(std::min<uint64_t>(bytes, Unknown - 1));
    };
    auto id = [&]() -> Id { return number(UINT32_MAX); };
    auto string = [&]() {
        size_t n = count();
        std::string s(n, '\0');
        if (in.get() != ' ' || !in.read(s.data(), n))
            fail("invalid snapshot string");
        return s;
    };
    auto ids = [&]() {
        std::vector<Id> v;
        for (size_t n = count(); n; --n)
            v.push_back(id());
        return v;
    };
    auto edges = [&](std::vector<std::vector<Id>>& graph) {
        size_t n = count();
        for (size_t i = 0; i < n; ++i)
        {
            graph.push_back(ids());
            for (Id target : graph.back())
                if (target >= n)
                    fail("invalid graph edge");
        }
    };
    Model m;
    edges(m.cfg);
    for (size_t n = count(); n; --n)
    {
        Id block = id(), order = id();
        if (block >= m.cfg.size())
            fail("invalid block ID");
        m.sites.push_back({block, order, string()});
    }
    edges(m.flow);
    m.flowSite = ids();
    if (m.flowSite.size() != m.flow.size())
        fail("invalid flow location table");
    for (Id s : m.flowSite)
        if (s != Unknown && s >= m.sites.size())
            fail("invalid flow site");
    for (auto* ports : {&m.reads, &m.writes, &m.frees})
    {
        for (size_t n = count(); n; --n)
        {
            Port p{id(), id(), id()};
            if (p.site >= m.sites.size() ||
                (ports != &m.frees && p.value >= m.flow.size()))
                fail("invalid memory port");
            ports->push_back(p);
        }
    }
    for (auto* ends : {&m.sources, &m.sinks})
    {
        for (size_t n = count(); n; --n)
        {
            Endpoint e{id(), id()};
            if (e.site >= m.sites.size() || e.value >= m.flow.size())
                fail("invalid endpoint");
            ends->push_back(e);
        }
    }
    for (size_t n = count(); n; --n)
    {
        auto calls = number(UINT64_MAX);
        m.unknownCalls.emplace(string(), calls);
    }
    m.index.objects = ids();
    for (size_t n = count(); n; --n)
    {
        Range r{number(m.index.objects.size()), number(m.index.objects.size())};
        if (!r.size || r.size > m.index.objects.size() - r.offset)
            fail("invalid group range");
        auto begin = m.index.objects.begin() + r.offset;
        if (!std::is_sorted(begin, begin + r.size))
            fail("unsorted points-to range");
        m.index.groups.push_back(r);
    }
    for (size_t n = count(); n; --n)
    {
        Id p = id(), g = id();
        if ((g < Unknown && g >= m.index.groups.size()) ||
            !m.index.pointerGroup.emplace(p, g).second)
            fail("invalid pointer mapping");
    }
    for (auto* ports : {&m.reads, &m.writes, &m.frees})
        for (auto p : *ports)
            if (!m.index.pointerGroup.count(p.pointer))
                fail("missing pointer mapping");
    in >> std::ws;
    if (!in.eof())
        fail("trailing snapshot data");
    return m;
}
} // namespace clients
