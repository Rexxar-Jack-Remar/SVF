#include "Query.h"
#ifdef SVF_CLIENT_CUDA
#    include "Cuda.h"
#endif
#ifdef SVF_CLIENT_BASELINES
#    include "Util/CommandLine.h"
#    include "pestrie/query.hh"
#    include "llvm/ADT/StringExtras.h"
#    include "llvm/Support/FileSystem.h"
#    include "llvm/Support/Path.h"
#    include "llvm/Support/Program.h"
#    include "llvm/Support/SHA256.h"
#    include <cstring>
#    include <fstream>
#endif
#include <unordered_map>

namespace clients
{
namespace
{
#ifdef SVF_CLIENT_BASELINES
Option<std::string> cacheDir("client-index-cache",
                             "Persist Bitmap/Pestrie indexes in this directory",
                             "");

std::string cacheKey(const Index& index)
{
    llvm::SHA256 hash;
    auto append = [&](const auto& n) {
        hash.update(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t*>(&n), sizeof(n)));
    };
    hash.update("svf-alias-baselines-21d73b4-v1");
    append(index.objects.size());
    hash.update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(index.objects.data()),
        index.objects.size() * sizeof(Id)));
    append(index.groups.size());
    for (auto g : index.groups)
    {
        append(g.offset);
        append(g.size);
    }
    for (auto [p, g] : index.pointerGroup)
    {
        append(p);
        append(g);
    }
    return llvm::toHex(hash.final(), true);
}
std::unique_ptr<IQuery> loadBaseline(const std::string& file, bool bitmap,
                                     size_t rows)
{
    FILE* fp = std::fopen(file.c_str(), "rb");
    char header[4];
    if (!fp || std::fread(header, 1, 4, fp) != 4 ||
        std::memcmp(header, bitmap ? BITMAP_PT_1 : PESTRIE_PT_1, 4))
        fail("invalid baseline index " + file);
    auto* q = bitmap ? load_bitmap_index(fp, PT_MATRIX, false)
                     : load_pestrie_index(fp, PT_MATRIX, false);
    std::fclose(fp);
    if (!q || size_t(q->nOfPtrs()) != rows)
        fail("invalid baseline pointer map");
    return std::unique_ptr<IQuery>(q);
}

std::unique_ptr<IQuery> buildBaseline(const Index& index, bool bitmap,
                                      Times& times)
{
    if (index.groups.empty())
        return {};
    llvm::SmallString<256> cache(cacheDir());
    std::string cachedFile, prefix = "svf-alias-index";
    if (!cache.empty())
    {
        if (auto ec = llvm::sys::fs::make_absolute(cache))
            fail(ec.message());
        if (auto ec = llvm::sys::fs::create_directories(cache))
            fail(ec.message());
        cachedFile = cache.str().str() + "/" + cacheKey(index) +
                     (bitmap ? ".ptb" : ".ptp");
        if (llvm::sys::fs::exists(cachedFile))
        {
            times.baselineIndex = "loaded";
            return loadBaseline(cachedFile, bitmap, index.pointerGroup.size());
        }
        prefix = cache.str().str() + "/build";
    }
    times.baselineIndex = "built";
    llvm::SmallString<256> dir;
    if (auto ec = llvm::sys::fs::createUniqueDirectory(prefix, dir))
        fail(ec.message());
    auto ptm = dir.str().str() + "/points.ptm",
         file = dir.str().str() + "/index";
    // Dense object IDs avoid allocating columns for unused SVF node numbers.
    std::map<Id, Id> columns;
    for (Id object : index.objects)
        columns.emplace(object, columns.size());
    std::ofstream out(ptm);
    out << index.pointerGroup.size() << ' ' << columns.size() << '\n';
    for (auto [ptr, g] : index.pointerGroup)
    {
        if (g >= Unknown)
        {
            out << "0\n";
            continue;
        }
        Range r = index.groups[g];
        out << r.size;
        for (uint64_t j = 0; j < r.size; ++j)
            out << ' ' << columns.at(index.objects[r.offset + j]);
        out << '\n';
    }
    out.close();
    if (!out)
        fail("cannot write baseline input " + ptm);
    auto wpa = llvm::sys::fs::getMainExecutable(
        nullptr, reinterpret_cast<void*>(&buildBaseline));
    llvm::SmallString<256> tool(llvm::sys::path::parent_path(wpa));
    llvm::sys::path::append(tool, bitmap ? "svf-bit-index" : "svf-pes-index");
    if (llvm::sys::ExecuteAndWait(tool, {tool.str(), ptm, file}) != 0)
        fail("baseline indexer failed; retained input " + ptm);
    auto result = loadBaseline(file, bitmap, index.pointerGroup.size());
    if (!cachedFile.empty())
        if (auto ec = llvm::sys::fs::rename(file, cachedFile))
            fail(ec.message());
    llvm::sys::fs::remove_directories(dir);
    return result;
}
#endif
} // namespace
struct Query::State
{
    const Index& index;
    Times& times;
    bool verify;
    Id tile;
    std::vector<unsigned char> answers;
    std::unordered_map<Id, Id> row;
    std::vector<Id> groups;
#ifdef SVF_CLIENT_CUDA
    std::unique_ptr<GpuIndex> gpu;
#endif
#ifdef SVF_CLIENT_BASELINES
    std::unique_ptr<IQuery> baseline;
#endif
    State(const Index& index, Times& times, bool verify, Id tile)
        : index(index), times(times), verify(verify), tile(tile), answers(tile)
    {
    }

    bool cpu(Id a, Id b) const
    {
#ifdef SVF_CLIENT_BASELINES
        if (baseline && groups[a] < Unknown && groups[b] < Unknown)
            return baseline->IsAlias(a, b);
#endif
        return index.alias(groups[a], groups[b]);
    }
};
Query::Query(const Index& index, const std::string& backend, Id device, Id tile,
             bool verify, Times& times)
    : state(new State(index, times, verify, tile))
{
    if (!tile)
        fail("tile capacity must be positive");
    for (auto [p, g] : index.pointerGroup)
    {
        state->row.emplace(p, state->groups.size());
        state->groups.push_back(g);
    }
    if (backend == "cpu")
        return;
#ifdef SVF_CLIENT_CUDA
    if (backend == "cuda")
    {
        state->gpu = std::make_unique<GpuIndex>(index, state->groups, device,
                                                tile, times);
        return;
    }
#endif
#ifdef SVF_CLIENT_BASELINES
    if (backend == "bitmap" || backend == "pestrie")
    {
        state->baseline = buildBaseline(index, backend == "bitmap", times);
        return;
    }
#endif
    fail("unknown or disabled query backend: " + backend);
}
Query::~Query() = default;

void Query::serve(const std::vector<Id>& pointers, const ConsumeTile& consume)
{
    State& s = *state;
    auto start = Clock::now();
    std::vector<Id> rows;
    if (pointers.size() > UINT32_MAX)
        fail("candidate sequence exceeds the triangle decoder's 32-bit row "
             "space");
    for (Id p : pointers)
    {
        auto it = s.row.find(p);
        if (it == s.row.end())
            fail("candidate NodeID missing from snapshot: " +
                 std::to_string(p));
        rows.push_back(it->second);
    }
    uint64_t n = rows.size();
    s.times.candidates += n;
    uint64_t total = n ? n * (n - 1) / 2 : 0;
    s.times.pairs += total;
#ifdef SVF_CLIENT_CUDA
    if (s.gpu)
        s.gpu->candidates(rows);
#endif
    s.times.query += seconds(start);
    size_t first = 0, second = 1;
    for (uint64_t base = 0; base < total; base += s.tile)
    {
        Id size = std::min<uint64_t>(s.tile, total - base);
        start = Clock::now();
#ifdef SVF_CLIENT_CUDA
        if (s.gpu)
            s.gpu->query(base, size, s.answers.data());
        else
#endif
        {
            size_t i = first, j = second;
            for (Id k = 0; k < size; ++k)
            {
                s.answers[k] = s.cpu(rows[i], rows[j]);
                if (++j == n)
                    j = ++i + 1;
            }
        }
        s.times.query += seconds(start);
        start = Clock::now();
        if (s.verify)
        {
            size_t i = first, j = second;
            for (Id k = 0; k < size; ++k)
            {
                if (bool(s.answers[k]) !=
                    s.index.alias(s.groups[rows[i]], s.groups[rows[j]]))
                    fail("pair mismatch at SVF nodes " +
                         std::to_string(pointers[i]) + "," +
                         std::to_string(pointers[j]));
                if (++j == n)
                    j = ++i + 1;
            }
        }
        consume(base, llvm::ArrayRef<unsigned char>(s.answers.data(), size));
        // Move to the first pair of the next tile, skipping entire rows.
        uint64_t remaining = size;
        while (remaining >= n - second)
        {
            remaining -= n - second;
            second = ++first + 1;
            if (first + 1 == n)
                break;
        }
        second += remaining;
        ++s.times.tiles;
        s.times.consume += seconds(start);
    }
}
} // namespace clients
