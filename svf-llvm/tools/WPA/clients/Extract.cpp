#include "MemoryModel/PointerAnalysisImpl.h"
#include "Model.h"
#include "SVF-LLVM/LLVMModule.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"

#include <set>
#include <unordered_map>

namespace clients
{
using namespace llvm;
namespace
{
class Extractor
{
public:
    explicit Extractor(SVF::BVDataPTAImpl& p) : pta(p) {}
    Model run(Times& times);

private:
    SVF::BVDataPTAImpl& pta;
    SVF::LLVMModuleSet* modules = SVF::LLVMModuleSet::getLLVMModuleSet();
    Model m;
    std::unordered_map<const Instruction*, Id> sites;
    std::unordered_map<const Value*, Id> values;
    std::map<const Function*, std::vector<const ReturnInst*>> returns;
    std::map<Id, std::vector<Id>> continuations;
    std::vector<const Function*> functions;
    std::set<Id> pointers;
    Id newFlow(Id site)
    {
        Id id = m.flow.size();
        m.flow.emplace_back();
        m.flowSite.push_back(site);
        return id;
    }
    Id value(const Value* v);
    Id pointer(const Value* v);
    void layout();
    void connect(const Instruction& inst);
    void call(const CallBase& call, Id site);
    void external(const CallBase& call, StringRef name, Id site);
    void buildIndex();
};

Id Extractor::value(const Value* v)
{
    auto it = values.find(v);
    if (it != values.end())
        return it->second;
    Id site = Unknown;
    if (auto i = dyn_cast<Instruction>(v))
        site = sites.at(i);
    else if (auto a = dyn_cast<Argument>(v))
        site = sites.at(&a->getParent()->getEntryBlock().front());
    Id id = newFlow(site);
    values.emplace(v, id);
    return id;
}

Id Extractor::pointer(const Value* v)
{
    auto sv = modules->getSVFValue(v);
    auto* pag = pta.getPAG();
    Id id = pag->hasValueNode(sv) ? pag->getValueNode(sv) : Unknown;
    pointers.insert(id);
    return id;
}

void Extractor::layout()
{
    for (Module& mod : modules->getLLVMModules())
    {
        for (const Function& f : mod)
        {
            if (f.isDeclaration() ||
                SVF::SVFUtil::isExtCall(modules->getSVFFunction(&f)))
                continue;
            functions.push_back(&f);
            unsigned ordinal = 0;
            for (const BasicBlock& bb : f)
            {
                Id block = m.cfg.size();
                m.cfg.emplace_back();
                for (const Instruction& inst : bb)
                {
                    Id site = m.sites.size();
                    sites.emplace(&inst, site);
                    std::string label = f.getName().str() + "#" +
                                        std::to_string(++ordinal) + "/" +
                                        inst.getOpcodeName();
                    if (auto loc = inst.getDebugLoc())
                        label += "@" + loc->getFilename().str() + ":" +
                                 std::to_string(loc.getLine());
                    m.sites.push_back({block, ordinal, std::move(label)});
                    if (auto ret = dyn_cast<ReturnInst>(&inst))
                        returns[&f].push_back(ret);
                    if (isa<CallBase>(inst) && !isa<DbgInfoIntrinsic>(inst) &&
                        inst.getNextNode())
                    {
                        block = m.cfg.size();
                        m.cfg.emplace_back();
                    }
                }
            }
        }
    }
}

void Extractor::external(const CallBase& c, StringRef name, Id s)
{
    auto arg = [&](unsigned i) { return c.getArgOperand(i); };
    auto port = [&](std::vector<Port>& list, unsigned i, Id flow) {
        if (i < c.arg_size() && arg(i)->getType()->isPointerTy())
            list.push_back({s, pointer(arg(i)), flow});
    };
    auto copy = [&](unsigned to, unsigned from) {
        Id bytes = newFlow(s);
        port(m.reads, from, bytes);
        port(m.writes, to, bytes);
    };
    auto returned = [&](unsigned i) {
        if (i < c.arg_size() && !c.getType()->isVoidTy())
        {
            Id from = value(arg(i)), to = value(&c);
            m.flow[from].push_back(to);
        }
    };
    // Match LLVM overloaded memory intrinsics by stem.
    if (name == "free" || name == "cfree" || name.startswith("_ZdlPv") ||
        name.startswith("_ZdaPv"))
    {
        port(m.frees, 0, 0);
    }
    else if (name == "realloc")
    {
        port(m.frees, 0, 0);
        if (c.getType()->isPointerTy())
        {
            Id bytes = newFlow(s);
            port(m.reads, 0, bytes);
            m.writes.push_back({s, pointer(&c), bytes});
        }
    }
    else if (name == "malloc" || name == "calloc" || name.startswith("_Znwm") ||
             name.startswith("_Znam") || name.startswith("llvm.dbg.") ||
             name.startswith("llvm.lifetime.") || name == "llvm.assume")
    {
        return;
    }
    else if (name == "memcpy" || name == "memmove" || name == "strcpy" ||
             name == "strncpy" || name == "strcat" || name == "strncat" ||
             name.startswith("llvm.memcpy.") ||
             name.startswith("llvm.memmove."))
    {
        copy(0, 1);
        returned(0);
    }
    else if (name == "memset" || name.startswith("llvm.memset."))
    {
        if (c.arg_size() > 1)
            port(m.writes, 0, value(arg(1)));
        returned(0);
    }
    else if (name == "read" || name == "pread" || name == "recv" ||
             name == "recvfrom" || name == "fread" || name == "fgets" ||
             name == "gets")
    {
        Id source = newFlow(s);
        m.sources.push_back({s, source});
        port(m.writes,
             name == "fread" || name == "fgets" || name == "gets" ? 0 : 1,
             source);
        if (name == "fgets" || name == "gets")
            returned(0);
    }
    else if (name == "scanf" || name == "__isoc99_scanf" || name == "fscanf" ||
             name == "__isoc99_fscanf")
    {
        unsigned first = name.contains("fscanf") ? 2 : 1;
        Id source = newFlow(s);
        m.sources.push_back({s, source});
        for (unsigned i = first; i < c.arg_size(); ++i)
            port(m.writes, i, source);
    }
    else if (name == "sscanf" || name == "__isoc99_sscanf")
    {
        for (unsigned i = 2; i < c.arg_size(); ++i)
            copy(i, 0);
    }
    else if (name == "strdup" && c.getType()->isPointerTy())
    {
        Id bytes = newFlow(s);
        port(m.reads, 0, bytes);
        m.writes.push_back({s, pointer(&c), bytes});
    }
    else if (name == "getenv" && c.getType()->isPointerTy())
    {
        Id source = newFlow(s);
        m.sources.push_back({s, source});
        m.writes.push_back({s, pointer(&c), source});
    }
    else if (name == "getchar" || name == "getc" || name == "fgetc")
    {
        m.sources.push_back({s, value(&c)});
    }
    else if (name == "system" || name == "popen" || name == "puts" ||
             name == "printf" || name == "fprintf" || name == "fwrite" ||
             name == "fputs" || name == "putchar" || name == "write" ||
             name == "send" || name == "execv" || name == "execve")
    {
        Id sink = newFlow(s);
        m.sinks.push_back({s, sink});
        unsigned i =
            (name == "write" || name == "send" || name == "fprintf") ? 1 : 0;
        if (name == "putchar" && c.arg_size())
            m.flow[value(arg(0))].push_back(sink);
        port(m.reads, i, sink);
        if (name == "printf" || name == "fprintf")
        {
            for (unsigned j = i + 1; j < c.arg_size(); ++j)
            {
                m.flow[value(arg(j))].push_back(sink);
                port(m.reads, j, sink);
            }
        }
    }
    else
    {
        // Weak external summary: inputs can affect the return and writable
        // pointer arguments. Count coverage per callee in the final output.
        ++m.unknownCalls[name.str()];
        Id effect = newFlow(s);
        for (unsigned i = 0; i < c.arg_size(); ++i)
        {
            if (arg(i)->getType()->isMetadataTy())
                continue;
            m.flow[value(arg(i))].push_back(effect);
            if (!c.doesNotAccessMemory() && !c.onlyWritesMemory())
                port(m.reads, i, effect);
            if (!c.doesNotAccessMemory() && !c.onlyReadsMemory())
                port(m.writes, i, effect);
        }
        if (!c.getType()->isVoidTy())
        {
            Id dst = value(&c);
            m.flow[effect].push_back(dst);
        }
    }
}

void Extractor::call(const CallBase& c, Id s)
{
    std::vector<const Function*> targets;
    if (auto direct =
            dyn_cast<Function>(c.getCalledOperand()->stripPointerCasts()))
    {
        const auto* def =
            modules->getSVFFunction(direct)->getDefFunForMultipleModule();
        targets.push_back(cast<Function>(modules->getLLVMValue(def)));
    }
    else if (!c.isInlineAsm() && modules->hasICFGNode(&c))
    {
        auto* cs = modules->getCallICFGNode(&c);
        if (pta.hasIndCSCallees(cs))
            for (auto* dest : pta.getIndCSCallees(cs))
                targets.push_back(cast<Function>(
                    modules->getLLVMValue(dest->getDefFunForMultipleModule())));
    }
    std::sort(targets.begin(), targets.end(),
              [](const Function* a, const Function* b) {
                  return std::make_pair(
                             StringRef(a->getParent()->getModuleIdentifier()),
                             a->getName()) <
                         std::make_pair(
                             StringRef(b->getParent()->getModuleIdentifier()),
                             b->getName());
              });
    bool bypass = targets.empty() || isa<InvokeInst>(c) || isa<CallBrInst>(c);
    if (targets.empty())
        external(c, "<unresolved>", s);
    Id block = m.sites[s].block;
    for (auto* target : targets)
    {
        if (target->isDeclaration() ||
            SVF::SVFUtil::isExtCall(modules->getSVFFunction(target)))
        {
            external(c, target->getName(), s);
            bypass = true;
            continue;
        }
        Id entry = sites.at(&target->getEntryBlock().front());
        m.cfg[block].push_back(m.sites[entry].block);
        for (auto& formal : target->args())
        {
            if (formal.getArgNo() >= c.arg_size())
                break;
            Id src = value(c.getArgOperand(formal.getArgNo()));
            Id dst = value(&formal);
            m.flow[src].push_back(dst);
        }
        for (const auto* ret : returns[target])
        {
            auto& out = m.cfg[m.sites[sites.at(ret)].block];
            const auto& next = continuations[s];
            out.insert(out.end(), next.begin(), next.end());
            if (const Value* v = ret->getReturnValue())
            {
                Id src = value(v), dst = value(&c);
                m.flow[src].push_back(dst);
            }
        }
    }
    if (bypass)
    {
        const auto& next = continuations[s];
        m.cfg[block].insert(m.cfg[block].end(), next.begin(), next.end());
    }
}

void Extractor::connect(const Instruction& inst)
{
    Id s = sites.at(&inst), block = m.sites[s].block;
    if (isa<DbgInfoIntrinsic>(inst))
        return;
    if (auto c = dyn_cast<CallBase>(&inst))
    {
        if (inst.isTerminator())
        {
            for (auto* next : successors(inst.getParent()))
                continuations[s].push_back(
                    m.sites[sites.at(&next->front())].block);
        }
        else
            continuations[s].push_back(
                m.sites[sites.at(inst.getNextNode())].block);
        call(*c, s);
        return;
    }
    if (inst.isTerminator())
        for (auto* next : successors(inst.getParent()))
            m.cfg[block].push_back(m.sites[sites.at(&next->front())].block);
    if (auto load = dyn_cast<LoadInst>(&inst))
        m.reads.push_back({s, pointer(load->getPointerOperand()), value(load)});
    else if (auto store = dyn_cast<StoreInst>(&inst))
        m.writes.push_back({s, pointer(store->getPointerOperand()),
                            value(store->getValueOperand())});
    else if (auto rmw = dyn_cast<AtomicRMWInst>(&inst))
    {
        Id result = value(rmw), combined = newFlow(s);
        m.reads.push_back({s, pointer(rmw->getPointerOperand()), result});
        m.flow[value(rmw->getValOperand())].push_back(combined);
        m.flow[result].push_back(combined);
        m.writes.push_back({s, pointer(rmw->getPointerOperand()), combined});
    }
    else if (auto cmp = dyn_cast<AtomicCmpXchgInst>(&inst))
    {
        Id addr = pointer(cmp->getPointerOperand());
        m.reads.push_back({s, addr, value(cmp)});
        m.writes.push_back({s, addr, value(cmp->getNewValOperand())});
    }
    else if (!inst.getType()->isVoidTy())
    {
        Id dst = value(&inst);
        for (const Use& op : inst.operands())
            if (!isa<BasicBlock>(op) && !op->getType()->isMetadataTy())
                m.flow[value(op)].push_back(dst);
    }
}

void Extractor::buildIndex()
{
    std::map<std::vector<Id>, Id> equivalent;
    for (Id p : pointers)
    {
        Id group = Unknown;
        if (p != Unknown)
        {
            SVF::PointsTo pts;
            pta.expandFIObjs(pta.getPts(p), pts);
            if (!pta.containBlackHoleNode(pts))
            {
                group = Empty;
                if (!pts.empty())
                {
                    std::vector<Id> objects;
                    for (auto o : pts)
                        objects.push_back(o);
                    std::sort(objects.begin(), objects.end());
                    auto entry =
                        equivalent.emplace(objects, m.index.groups.size());
                    group = entry.first->second;
                    if (entry.second)
                    {
                        m.index.groups.push_back(
                            {m.index.objects.size(), objects.size()});
                        m.index.objects.insert(m.index.objects.end(),
                                               objects.begin(), objects.end());
                    }
                }
            }
        }
        m.index.pointerGroup.emplace(p, group);
    }
}

Model Extractor::run(Times& times)
{
    auto start = Clock::now();
    layout();
    for (auto f : functions)
        for (auto& bb : *f)
            for (auto& inst : bb)
                connect(inst);
    for (auto& edges : m.cfg)
    {
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    }
    times.graph = seconds(start);
    start = Clock::now();
    buildIndex();
    times.index = seconds(start);
    return std::move(m);
}
} // namespace
Model extract(SVF::BVDataPTAImpl& pta, Times& times)
{
    return Extractor(pta).run(times);
}
} // namespace clients
