#include "Driver.h"
#include "MemoryModel/PointerAnalysisImpl.h"
#include "Util/CommandLine.h"
#include "llvm/Support/JSON.h"
#ifdef __linux__
#    include <sys/resource.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
namespace clients
{
namespace
{
Option<std::string> client("alias-client", "Post-PTA client: uaf or taint", "");
Option<std::string> backend("client-backend",
                            "Query backend: cpu, cuda, bitmap or pestrie",
                            "cpu");
Option<std::string> input("client-load", "Replay a complete client snapshot",
                          "");
Option<std::string> output("client-save",
                           "Write a reusable program and points-to snapshot",
                           "");
Option<std::string> reports(
    "client-report", "Write JSON Lines reports to this file (default stdout)",
    "");
Option<Id> device("client-device", "CUDA device ordinal", 0);
Option<Id> tile("client-tile", "Maximum answers per query tile", 1048576);
Option<bool> verify(
    "client-verify",
    "Compare every backend answer with the normalized CPU intersection", false);
} // namespace
bool requested()
{
    return !client().empty() || !output().empty() || replaying();
}
bool replaying()
{
    return !input().empty();
}
void validate(const std::vector<std::string>& modules)
{
    if (client() != "uaf" && client() != "taint" &&
        !(client().empty() && !output().empty()))
        fail("select -alias-client=uaf or -alias-client=taint");
    if (!tile())
        fail("tile size must be positive");
    if (replaying() && (!modules.empty() || !output().empty()))
        fail("use one snapshot or bitcode input");
    std::vector<std::string> paths = modules;
    for (const auto& path : {input(), output(), reports()})
    {
        if (path.empty())
            continue;
        for (const auto& prior : paths)
            if (std::filesystem::weakly_canonical(path) ==
                std::filesystem::weakly_canonical(prior))
                fail("input, snapshot and report paths must be distinct");
        paths.push_back(path);
    }
}
void run(SVF::BVDataPTAImpl* pta, Clock::time_point wholeStart,
         double irSeconds, double ptaSeconds)
{
    auto start = Clock::now();
    Times t;
    Model m = replaying() ? load(input()) : extract(*pta, t);
    double loading = replaying() ? seconds(start) : 0;
    if (pta && verify())
    {
        for (auto a = m.index.pointerGroup.begin();
             a != m.index.pointerGroup.end(); ++a)
            for (auto b = a; b != m.index.pointerGroup.end(); ++b)
            {
                bool native =
                    a->first == Unknown || b->first == Unknown ||
                    pta->alias(a->first, b->first) != SVF::AliasResult::NoAlias;
                if (native != m.index.alias(a->second, b->second))
                    fail("snapshot differs from SVF alias at nodes " +
                         std::to_string(a->first) + "," +
                         std::to_string(b->first));
            }
    }
    double writing = 0;
    if (!output().empty())
    {
        auto writeStart = Clock::now();
        save(m, output());
        writing = seconds(writeStart);
    }
    if (client().empty())
    {
        llvm::json::Object preparation{
            {"ir_s", irSeconds},  {"pta_s", ptaSeconds},
            {"graph_s", t.graph}, {"index_s", t.index},
            {"save_s", writing},  {"total_s", seconds(wholeStart)}};
        llvm::errs() << "CLIENT_PREPARE "
                     << llvm::formatv("{0}",
                                      llvm::json::Value(std::move(preparation)))
                     << "\n";
        return;
    }
    std::ofstream file;
    if (!reports().empty())
    {
        file.open(reports());
        if (!file)
            fail("cannot open report " + reports());
    }
    analyze(m, client() == "uaf", backend(), device(), tile(), verify(),
            file.is_open() ? file : std::cout, t);
    if (file.is_open())
    {
        file.close();
        if (!file)
            fail("closing report failed");
    }
    double elapsed = seconds(start), total = seconds(wholeStart);
    llvm::json::Object unknown;
    for (auto& [name, n] : m.unknownCalls)
        unknown[name] = int64_t(n);
    llvm::json::Object metrics{{"client", client()},
                               {"backend", backend()},
                               {"mode", replaying() ? "snapshot" : "live-pta"},
                               {"baseline_index", t.baselineIndex},
                               {"cuda_buffer_bytes", int64_t(t.deviceBytes)},
                               {"client_s", elapsed},
                               {"total_s", total},
                               {"load_s", loading},
                               {"ir_s", irSeconds},
                               {"pta_s", ptaSeconds},
                               {"save_s", writing},
                               {"graph_candidates_s", t.graph},
                               {"gpu_kernel_s", t.gpuKernel},
                               {"gpu_copy_s", t.gpuCopy},
                               {"index_s", t.index},
                               {"query_transfer_s", t.query},
                               {"consume_s", t.consume},
                               {"solve_s", t.solve},
                               {"report_s", t.report},
                               {"candidates", int64_t(t.candidates)},
                               {"pairs", int64_t(t.pairs)},
                               {"hits", int64_t(t.hits)},
                               {"tiles", int64_t(t.tiles)},
                               {"reports", int64_t(t.reports)},
                               {"frees", int64_t(m.frees.size())},
                               {"reads", int64_t(m.reads.size())},
                               {"writes", int64_t(m.writes.size())},
                               {"sources", int64_t(m.sources.size())},
                               {"sinks", int64_t(m.sinks.size())},
                               {"unknown_calls", std::move(unknown)}};
#ifdef __linux__
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage))
        metrics["wpa_peak_rss_bytes"] = int64_t(usage.ru_maxrss) * 1024;
#endif
    llvm::errs() << "CLIENT_METRICS "
                 << llvm::formatv("{0}", llvm::json::Value(std::move(metrics)))
                 << "\n";
}
} // namespace clients
