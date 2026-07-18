//===- PointerAnalysis.cpp -- Base class of pointer analyses------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * PointerAnalysis.cpp
 *
 *  Created on: May 14, 2013
 *      Author: Yulei Sui
 */

#include "Util/Options.h"
#include "SVFIR/SVFModule.h"
#include "Util/SVFUtil.h"

#include "MemoryModel/PointerAnalysisImpl.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/PAGBuilderFromFile.h"
#include "Util/PTAStat.h"
#include "Graphs/ThreadCallGraph.h"
#include "Graphs/ICFG.h"
#include "Graphs/CallGraph.h"
#include "Util/CallGraphBuilder.h"

#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace SVF;
using namespace SVFUtil;

namespace
{

std::string normalizeExtFunName(const SVFFunction* fun)
{
    if (fun == nullptr)
        return "";

    std::string name = fun->getName();
    if (!name.empty() && name[0] == '\01')
        name.erase(0, 1);
    return name;
}

bool isTaintSourceLikeFun(const SVFFunction* fun)
{
    const std::string name = normalizeExtFunName(fun);

    return name == "gets" || name == "fgets" || name == "fgets_unlocked" ||
           name == "gzgets" || name == "read" || name == "pread" ||
           name == "pread64" || name == "recv" || name == "__recv" ||
           name == "recvfrom" || name == "recvmsg" || name == "fread" ||
           name == "scanf" || name == "__isoc99_scanf" ||
           name == "vscanf" || name == "__isoc99_vscanf" ||
           name == "fscanf" || name == "__isoc99_fscanf" ||
           name == "vfscanf" || name == "__isoc99_vfscanf" ||
           name == "sscanf" || name == "__isoc99_sscanf" ||
           name == "vsscanf" || name == "__isoc99_vsscanf" ||
           name == "getenv";
}

bool isTaintSinkLikeFun(const SVFFunction* fun)
{
    const std::string name = normalizeExtFunName(fun);

    return name == "system" || name == "popen" ||
           name == "execl" || name == "execlp" || name == "execle" ||
           name == "execv" || name == "execvp" || name == "execvpe" ||
           name == "strcpy" || name == "__strcpy_chk" ||
           name == "strncpy" || name == "__strncpy_chk" ||
           name == "strcat" || name == "__strcat_chk" ||
           name == "strncat" || name == "__strncat_chk" ||
           name == "sprintf" || name == "__sprintf_chk" ||
           name == "snprintf" || name == "__snprintf_chk" ||
           name == "vsprintf" || name == "__vsprintf_chk" ||
           name == "vsnprintf" || name == "__vsnprintf_chk" ||
           name == "memcpy" || name == "__memcpy_chk" ||
           name == "memmove" || name == "__memmove_chk";
}

bool isTaintRelatedFun(const SVFFunction* fun)
{
    return isTaintSourceLikeFun(fun) || isTaintSinkLikeFun(fun);
}

} // End anonymous namespace


SVFIR* PointerAnalysis::pag = nullptr;

const std::string PointerAnalysis::aliasTestMayAlias            = "MAYALIAS";
const std::string PointerAnalysis::aliasTestMayAliasMangled     = "_Z8MAYALIASPvS_";
const std::string PointerAnalysis::aliasTestNoAlias             = "NOALIAS";
const std::string PointerAnalysis::aliasTestNoAliasMangled      = "_Z7NOALIASPvS_";
const std::string PointerAnalysis::aliasTestPartialAlias        = "PARTIALALIAS";
const std::string PointerAnalysis::aliasTestPartialAliasMangled = "_Z12PARTIALALIASPvS_";
const std::string PointerAnalysis::aliasTestMustAlias           = "MUSTALIAS";
const std::string PointerAnalysis::aliasTestMustAliasMangled    = "_Z9MUSTALIASPvS_";
const std::string PointerAnalysis::aliasTestFailMayAlias        = "EXPECTEDFAIL_MAYALIAS";
const std::string PointerAnalysis::aliasTestFailMayAliasMangled = "_Z21EXPECTEDFAIL_MAYALIASPvS_";
const std::string PointerAnalysis::aliasTestFailNoAlias         = "EXPECTEDFAIL_NOALIAS";
const std::string PointerAnalysis::aliasTestFailNoAliasMangled  = "_Z20EXPECTEDFAIL_NOALIASPvS_";

/*!
 * Constructor
 */
PointerAnalysis::PointerAnalysis(SVFIR* p, PTATY ty, bool alias_check) :
    svfMod(nullptr),ptaTy(ty),stat(nullptr),callgraph(nullptr),callGraphSCC(nullptr),icfg(nullptr),chgraph(nullptr)
{
    pag = p;
    OnTheFlyIterBudgetForStat = Options::StatBudget();
    print_stat = Options::PStat();
    ptaImplTy = BaseImpl;
    alias_validation = (alias_check && Options::EnableAliasCheck());
}

/*!
 * Destructor
 */
PointerAnalysis::~PointerAnalysis()
{
    destroy();
    // do not delete the SVFIR for now
    //delete pag;
}


void PointerAnalysis::destroy()
{
    delete callgraph;
    callgraph = nullptr;

    delete callGraphSCC;
    callGraphSCC = nullptr;

    delete stat;
    stat = nullptr;
}

/*!
 * Initialization of pointer analysis
 */
void PointerAnalysis::initialize()
{
    assert(pag && "SVFIR has not been built!");

    svfMod = pag->getModule();
    chgraph = pag->getCHG();

    /// initialise pta call graph for every pointer analysis instance
    if(Options::EnableThreadCallGraph())
    {
        CallGraphBuilder bd;
        callgraph = bd.buildThreadCallGraph();
    }
    else
    {
        CallGraphBuilder bd;
        callgraph = bd.buildPTACallGraph();
    }
    callGraphSCCDetection();

    // dump callgraph
    if (Options::CallGraphDotGraph())
        getCallGraph()->dump("callgraph_initial");
}


/*!
 * Return TRUE if this node is a local variable of recursive function.
 */
bool PointerAnalysis::isLocalVarInRecursiveFun(NodeID id) const
{
    const BaseObjVar* baseObjVar = pag->getBaseObject(id);
    assert(baseObjVar && "base object not found!!");
    if(SVFUtil::isa<StackObjVar>(baseObjVar))
    {
        if(const SVFFunction* svffun = pag->getGNode(id)->getFunction())
        {
            return callGraphSCC->isInCycle(getCallGraph()->getCallGraphNode(svffun)->getId());
        }
    }
    return false;
}

/*!
 * Reset field sensitivity
 */
void PointerAnalysis::resetObjFieldSensitive()
{
    for (SVFIR::iterator nIter = pag->begin(); nIter != pag->end(); ++nIter)
    {
        if(ObjVar* node = SVFUtil::dyn_cast<ObjVar>(nIter->second))
            const_cast<MemObj*>(node->getMemObj())->setFieldSensitive();
    }
}

/*
 * Dump statistics
 */

void PointerAnalysis::dumpStat()
{

    if(print_stat && stat)
    {
        stat->performStat();
    }
}

/*!
 * Finalize the analysis after solving
 * Given the alias results, verify whether it is correct or not using alias check functions
 */
void PointerAnalysis::finalize()
{

    /// Print statistics
    dumpStat();

    /// Dump results
    if (Options::PTSPrint())
    {
        dumpTopLevelPtsTo();
        //dumpAllPts();
        //dumpCPts();
    }

    if (Options::FreePTSPrint())
        dumpFreeTopLevelPtsTo();

    if (Options::TaintPTSPrint())
        dumpTaintTopLevelPtsTo();

    if (Options::TypePrint())
        dumpAllTypes();

    if(Options::PTSAllPrint())
        dumpAllPts();

    if(Options::FreePTSAllPrint())
        dumpAllFreePts();

    if(Options::TaintPTSAllPrint())
        dumpAllTaintPts();

    if (Options::FuncPointerPrint())
        printIndCSTargets();

    getCallGraph()->verifyCallGraph();

    if (Options::CallGraphDotGraph())
        getCallGraph()->dump("callgraph_final");

    if(!pag->isBuiltFromFile() && alias_validation)
        validateTests();

    if (!Options::UsePreCompFieldSensitive())
        resetObjFieldSensitive();
}

/*!
 * Validate test cases
 */
void PointerAnalysis::validateTests()
{
    validateSuccessTests(aliasTestMayAlias);
    validateSuccessTests(aliasTestNoAlias);
    validateSuccessTests(aliasTestMustAlias);
    validateSuccessTests(aliasTestPartialAlias);
    validateExpectedFailureTests(aliasTestFailMayAlias);
    validateExpectedFailureTests(aliasTestFailNoAlias);

    validateSuccessTests(aliasTestMayAliasMangled);
    validateSuccessTests(aliasTestNoAliasMangled);
    validateSuccessTests(aliasTestMustAliasMangled);
    validateSuccessTests(aliasTestPartialAliasMangled);
    validateExpectedFailureTests(aliasTestFailMayAliasMangled);
    validateExpectedFailureTests(aliasTestFailNoAliasMangled);
}


void PointerAnalysis::dumpAllTypes()
{
    for (OrderedNodeSet::iterator nIter = this->getAllValidPtrs().begin();
            nIter != this->getAllValidPtrs().end(); ++nIter)
    {
        const PAGNode* node = getPAG()->getGNode(*nIter);
        if (SVFUtil::isa<DummyObjVar, DummyValVar>(node))
            continue;

        outs() << "##<" << node->getValue()->getName() << "> ";
        outs() << "Source Loc: " << node->getValue()->getSourceLoc();
        outs() << "\nNodeID " << node->getId() << "\n";

        const SVFType* type = node->getValue()->getType();
        pag->getSymbolInfo()->printFlattenFields(type);
    }
}

/*!
 * Dump points-to of top-level pointers (ValVar)
 */
void PointerAnalysis::dumpPts(NodeID ptr, const PointsTo& pts)
{

    const PAGNode* node = pag->getGNode(ptr);
    /// print the points-to set of node which has the maximum pts size.
    if (SVFUtil::isa<DummyObjVar> (node))
    {
        outs() << "##<Dummy Obj > id:" << node->getId();
    }
    else if (!SVFUtil::isa<DummyValVar>(node) && !SVFModule::pagReadFromTXT())
    {
        if (node->hasValue())
        {
            outs() << "##<" << node->getValue()->getName() << "> ";
            outs() << "Source Loc: " << node->getValue()->getSourceLoc();
        }
    }
    outs() << "\nPtr " << node->getId() << " ";

    if (pts.empty())
    {
        outs() << "\t\tPointsTo: {empty}\n\n";
    }
    else
    {
        outs() << "\t\tPointsTo: { ";
        for (PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit;
                ++it)
            outs() << *it << " ";
        outs() << "}\n\n";
    }

    outs() << "";

    for (PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit; ++it)
    {
        const PAGNode* node = pag->getGNode(*it);
        if(SVFUtil::isa<ObjVar>(node) == false)
            continue;
        NodeID ptd = node->getId();
        outs() << "!!Target NodeID " << ptd << "\t [";
        const PAGNode* pagNode = pag->getGNode(ptd);
        if (SVFUtil::isa<DummyValVar>(node))
            outs() << "DummyVal\n";
        else if (SVFUtil::isa<DummyObjVar>(node))
            outs() << "Dummy Obj id: " << node->getId() << "]\n";
        else
        {
            if (!SVFModule::pagReadFromTXT())
            {
                if (node->hasValue())
                {
                    outs() << "<" << pagNode->getValue()->getName() << "> ";
                    outs() << "Source Loc: "
                           << pagNode->getValue()->getSourceLoc() << "] \n";
                }
            }
        }
    }
}

/*!
 * Dump only the points-to set without target node details.
 */
void PointerAnalysis::dumpPtsOnly(NodeID ptr, const PointsTo& pts)
{
    outs() << "\nPtr " << ptr << " ";

    if (pts.empty())
    {
        outs() << "\t\tPointsTo: {empty}\n";
    }
    else
    {
        outs() << "\t\tPointsTo: { ";
        for (PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit; ++it)
            outs() << *it << " ";
        outs() << "}\n";
    }
}

/*!
 * Collect the union of points-to targets of pointer arguments passed to
 * free-like deallocation APIs.
 */
PointsTo PointerAnalysis::collectFreeTargetObjects()
{
    PointsTo freeTargets;

    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        bool isDeallocCall = SaberCheckerAPI::getCheckerAPI()->isMemDealloc(cs);

        if (!isDeallocCall && getCallGraph())
        {
            PTACallGraph::FunctionSet callees;
            getCallGraph()->getCallees(cs, callees);
            for (PTACallGraph::FunctionSet::const_iterator cit = callees.begin(),
                    ecit = callees.end(); cit != ecit; ++cit)
            {
                if (SaberCheckerAPI::getCheckerAPI()->isMemDealloc(*cit))
                {
                    isDeallocCall = true;
                    break;
                }
            }
        }

        if (!isDeallocCall)
            continue;

        SVFIR::SVFVarList& arglist = it->second;
        for (SVFIR::SVFVarList::const_iterator ait = arglist.begin(),
                aeit = arglist.end(); ait != aeit; ++ait)
        {
            const PAGNode* pagNode = *ait;
            if (pagNode->isPointer())
                freeTargets |= getPts(pagNode->getId());
        }
    }

    return freeTargets;
}

/*!
 * Return true if ptr may point to any object that is passed to a free-like API.
 */
bool PointerAnalysis::mayPointToFreeTarget(NodeID ptr, const PointsTo& freeTargets)
{
    if (freeTargets.empty())
        return false;

    const PointsTo& pts = getPts(ptr);
    return pts.intersects(freeTargets);
}

/*!
 * Collect the union of points-to targets of pointers used by taint source/sink
 * APIs. Source returns, source output arguments, and sink input arguments are
 * all included so the dump covers the call boundary objects relevant to taint.
 */
PointsTo PointerAnalysis::collectTaintTargetObjects()
{
    PointsTo taintTargets;

    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        bool isTaintCall = isTaintRelatedFun(cs->getCalledFunction());

        if (!isTaintCall && getCallGraph())
        {
            PTACallGraph::FunctionSet callees;
            getCallGraph()->getCallees(cs, callees);
            for (PTACallGraph::FunctionSet::const_iterator cit = callees.begin(),
                    ecit = callees.end(); cit != ecit; ++cit)
            {
                if (isTaintRelatedFun(*cit))
                {
                    isTaintCall = true;
                    break;
                }
            }
        }

        if (!isTaintCall)
            continue;

        SVFIR::SVFVarList& arglist = it->second;
        for (SVFIR::SVFVarList::const_iterator ait = arglist.begin(),
                aeit = arglist.end(); ait != aeit; ++ait)
        {
            const PAGNode* pagNode = *ait;
            if (pagNode->isPointer())
                taintTargets |= getPts(pagNode->getId());
        }

        const RetICFGNode* retNode = cs->getRetICFGNode();
        if (retNode != nullptr)
        {
            const SVFVar* actualRet = retNode->getActualRet();
            if (actualRet != nullptr && actualRet->isPointer())
                taintTargets |= getPts(actualRet->getId());
        }
    }

    return taintTargets;
}

/*!
 * Return true if ptr may point to any object related to taint source/sink APIs.
 */
bool PointerAnalysis::mayPointToTaintTarget(NodeID ptr, const PointsTo& taintTargets)
{
    if (taintTargets.empty())
        return false;

    const PointsTo& pts = getPts(ptr);
    return pts.intersects(taintTargets);
}

/*!
 * Dump points-to sets of top-level pointers that may alias a free target.
 */
void PointerAnalysis::dumpFreeTopLevelPtsTo()
{
    PointsTo freeTargets = collectFreeTargetObjects();
    outs() << "==================Free-related Top-Level Points-To Sets==================\n";

    for (OrderedNodeSet::iterator nIter = this->getAllValidPtrs().begin();
            nIter != this->getAllValidPtrs().end(); ++nIter)
    {
        const PAGNode* node = getPAG()->getGNode(*nIter);
        if (getPAG()->isValidTopLevelPtr(node) && mayPointToFreeTarget(node->getId(), freeTargets))
            dumpPtsOnly(node->getId(), getPts(node->getId()));
    }

    outs().flush();
}

/*!
 * Dump points-to sets of top-level pointers that may alias a taint target.
 */
void PointerAnalysis::dumpTaintTopLevelPtsTo()
{
    PointsTo taintTargets = collectTaintTargetObjects();
    outs() << "==================Taint-related Top-Level Points-To Sets==================\n";

    for (OrderedNodeSet::iterator nIter = this->getAllValidPtrs().begin();
            nIter != this->getAllValidPtrs().end(); ++nIter)
    {
        const PAGNode* node = getPAG()->getGNode(*nIter);
        if (getPAG()->isValidTopLevelPtr(node) && mayPointToTaintTarget(node->getId(), taintTargets))
            dumpPtsOnly(node->getId(), getPts(node->getId()));
    }

    outs().flush();
}

/*!
 * Dump all points-to sets whose points-to targets may alias a free target.
 */
void PointerAnalysis::dumpAllFreePts()
{
    PointsTo freeTargets = collectFreeTargetObjects();
    outs() << "==================Free-related Points-To Sets==================\n";

    OrderedNodeSet pagNodes;
    for(SVFIR::iterator it = pag->begin(), eit = pag->end(); it!=eit; it++)
        pagNodes.insert(it->first);

    for (NodeID n : pagNodes)
    {
        if (!mayPointToFreeTarget(n, freeTargets))
            continue;

        dumpPtsOnly(n, getPts(n));
    }

    outs().flush();
}

/*!
 * Dump all points-to sets whose points-to targets may alias a taint target.
 */
void PointerAnalysis::dumpAllTaintPts()
{
    PointsTo taintTargets = collectTaintTargetObjects();
    outs() << "==================Taint-related Points-To Sets==================\n";

    OrderedNodeSet pagNodes;
    for(SVFIR::iterator it = pag->begin(), eit = pag->end(); it!=eit; it++)
        pagNodes.insert(it->first);

    for (NodeID n : pagNodes)
    {
        if (!mayPointToTaintTarget(n, taintTargets))
            continue;

        dumpPtsOnly(n, getPts(n));
    }

    outs().flush();
}

/*!
 * Print indirect call targets at an indirect callsite
 */
void PointerAnalysis::printIndCSTargets(const CallICFGNode* cs, const FunctionSet& targets)
{
    outs() << "\nNodeID: " << getFunPtr(cs);
    outs() << "\nCallSite: ";
    outs() << cs->toString();
    outs() << "\tLocation: " << cs->getSourceLoc();
    outs() << "\t with Targets: ";

    if (!targets.empty())
    {
        FunctionSet::const_iterator fit = targets.begin();
        FunctionSet::const_iterator feit = targets.end();
        for (; fit != feit; ++fit)
        {
            const SVFFunction* callee = *fit;
            outs() << "\n\t" << callee->getName();
        }
    }
    else
    {
        outs() << "\n\tNo Targets!";
    }

    outs() << "\n";
}

/*!
 * Print all indirect callsites
 */
void PointerAnalysis::printIndCSTargets()
{
    outs() << "==================Function Pointer Targets==================\n";
    const CallEdgeMap& callEdges = getIndCallMap();
    CallEdgeMap::const_iterator it = callEdges.begin();
    CallEdgeMap::const_iterator eit = callEdges.end();
    for (; it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        const FunctionSet& targets = it->second;
        printIndCSTargets(cs, targets);
    }

    const CallSiteToFunPtrMap& indCS = getIndirectCallsites();
    CallSiteToFunPtrMap::const_iterator csIt = indCS.begin();
    CallSiteToFunPtrMap::const_iterator csEit = indCS.end();
    for (; csIt != csEit; ++csIt)
    {
        const CallICFGNode* cs = csIt->first;
        if (hasIndCSCallees(cs) == false)
        {
            outs() << "\nNodeID: " << csIt->second;
            outs() << "\nCallSite: ";
            outs() << cs->toString();
            outs() << "\tLocation: " << cs->getSourceLoc();
            outs() << "\n\t!!!has no targets!!!\n";
        }
    }
}



/*!
 * Resolve indirect calls
 */
void PointerAnalysis::resolveIndCalls(const CallICFGNode* cs, const PointsTo& target, CallEdgeMap& newEdges)
{

    assert(pag->isIndirectCallSites(cs) && "not an indirect callsite?");
    /// discover indirect pointer target
    for (PointsTo::iterator ii = target.begin(), ie = target.end();
            ii != ie; ii++)
    {

        if(getNumOfResolvedIndCallEdge() >= Options::IndirectCallLimit())
        {
            wrnMsg("Resolved Indirect Call Edges are Out-Of-Budget, please increase the limit");
            return;
        }

        if(ObjVar* objPN = SVFUtil::dyn_cast<ObjVar>(pag->getGNode(*ii)))
        {
            const MemObj* obj = pag->getObject(objPN);

            if(obj->isFunction())
            {
                const SVFFunction* calleefun = SVFUtil::cast<CallGraphNode>(obj->getGNode())->getFunction();
                const SVFFunction* callee = calleefun->getDefFunForMultipleModule();

                if(SVFUtil::matchArgs(cs, callee) == false)
                    continue;

                if(0 == getIndCallMap()[cs].count(callee))
                {
                    newEdges[cs].insert(callee);
                    getIndCallMap()[cs].insert(callee);

                    callgraph->addIndirectCallGraphEdge(cs, cs->getCaller(), callee);
                    // FIXME: do we need to update llvm call graph here?
                    // The indirect call is maintained by ourself, We may update llvm's when we need to
                    //PTACallGraphNode* callgraphNode = callgraph->getOrInsertFunction(cs.getCaller());
                    //callgraphNode->addCalledFunction(cs,callgraph->getOrInsertFunction(callee));
                }
            }
        }
    }
}

/*
 * Get virtual functions "vfns" based on CHA
 */
void PointerAnalysis::getVFnsFromCHA(const CallICFGNode* cs, VFunSet &vfns)
{
    if (chgraph->csHasVFnsBasedonCHA(cs))
        vfns = chgraph->getCSVFsBasedonCHA(cs);
}

/*
 * Get virtual functions "vfns" from PoninsTo set "target" for callsite "cs"
 */
void PointerAnalysis::getVFnsFromPts(const CallICFGNode* cs, const PointsTo &target, VFunSet &vfns)
{

    if (chgraph->csHasVtblsBasedonCHA(cs))
    {
        Set<const SVFGlobalValue*> vtbls;
        const VTableSet &chaVtbls = chgraph->getCSVtblsBasedonCHA(cs);
        for (PointsTo::iterator it = target.begin(), eit = target.end(); it != eit; ++it)
        {
            const PAGNode *ptdnode = pag->getGNode(*it);
            if (ptdnode->hasValue())
            {
                if ((isa<ObjVar>(ptdnode) && isa<GlobalObjVar>(pag->getBaseObject(ptdnode->getId())))
                        || (isa<ValVar>(ptdnode) && isa<GlobalValVar>(pag->getBaseValVar(ptdnode->getId()))))
                {
                    const SVFGlobalValue* globalValue = SVFUtil::dyn_cast<SVFGlobalValue>(ptdnode->getValue());
                    if (chaVtbls.find(globalValue) != chaVtbls.end())
                        vtbls.insert(globalValue);
                }

            }
        }
        chgraph->getVFnsFromVtbls(cs, vtbls, vfns);
    }
}

/*
 * Connect callsite "cs" to virtual functions in "vfns"
 */
void PointerAnalysis::connectVCallToVFns(const CallICFGNode* cs, const VFunSet &vfns, CallEdgeMap& newEdges)
{
    //// connect all valid functions
    for (VFunSet::const_iterator fit = vfns.begin(),
            feit = vfns.end(); fit != feit; ++fit)
    {
        const SVFFunction* callee = *fit;
        callee = callee->getDefFunForMultipleModule();
        if (getIndCallMap()[cs].count(callee) > 0)
            continue;
        if(cs->arg_size() == callee->arg_size() ||
                (cs->isVarArg() && callee->isVarArg()))
        {
            newEdges[cs].insert(callee);
            getIndCallMap()[cs].insert(callee);
            const CallICFGNode* callBlockNode = cs;
            callgraph->addIndirectCallGraphEdge(callBlockNode, cs->getCaller(),callee);
        }
    }
}

/// Resolve cpp indirect call edges
void PointerAnalysis::resolveCPPIndCalls(const CallICFGNode* cs, const PointsTo& target, CallEdgeMap& newEdges)
{
    assert(cs->isVirtualCall() && "not cpp virtual call");

    VFunSet vfns;
    if (Options::ConnectVCallOnCHA())
        getVFnsFromCHA(cs, vfns);
    else
        getVFnsFromPts(cs, target, vfns);
    connectVCallToVFns(cs, vfns, newEdges);
}

/*!
 * Find the alias check functions annotated in the C files
 * check whether the alias analysis results consistent with the alias check function itself
 */
void PointerAnalysis::validateSuccessTests(std::string fun)
{
    // check for must alias cases, whether our alias analysis produce the correct results
    if (const SVFFunction* checkFun = svfMod->getSVFFunction(fun))
    {
        if(!checkFun->isUncalledFunction())
            outs() << "[" << this->PTAName() << "] Checking " << fun << "\n";

        for(const CallICFGNode* callNode : pag->getCallSiteSet())
        {
            if (callNode->getCalledFunction() == checkFun)
            {
                assert(callNode->getNumArgOperands() == 2
                       && "arguments should be two pointers!!");
                const SVFVar* V1 = callNode->getArgument(0);
                const SVFVar* V2 = callNode->getArgument(1);
                AliasResult aliasRes = alias(V1->getId(), V2->getId());

                bool checkSuccessful = false;
                if (fun == aliasTestMayAlias || fun == aliasTestMayAliasMangled)
                {
                    if (aliasRes == AliasResult::MayAlias || aliasRes == AliasResult::MustAlias)
                        checkSuccessful = true;
                }
                else if (fun == aliasTestNoAlias || fun == aliasTestNoAliasMangled)
                {
                    if (aliasRes == AliasResult::NoAlias)
                        checkSuccessful = true;
                }
                else if (fun == aliasTestMustAlias || fun == aliasTestMustAliasMangled)
                {
                    // change to must alias when our analysis support it
                    if (aliasRes == AliasResult::MayAlias || aliasRes == AliasResult::MustAlias)
                        checkSuccessful = true;
                }
                else if (fun == aliasTestPartialAlias || fun == aliasTestPartialAliasMangled)
                {
                    // change to partial alias when our analysis support it
                    if (aliasRes == AliasResult::MayAlias)
                        checkSuccessful = true;
                }
                else
                    assert(false && "not supported alias check!!");

                NodeID id1 = V1->getId();
                NodeID id2 = V2->getId();

                if (checkSuccessful)
                    outs() << sucMsg("\t SUCCESS :") << fun << " check <id:" << id1 << ", id:" << id2 << "> at ("
                           << callNode->getSourceLoc() << ")\n";
                else
                {
                    SVFUtil::errs() << errMsg("\t FAILURE :") << fun
                                    << " check <id:" << id1 << ", id:" << id2
                                    << "> at (" << callNode->getSourceLoc() << ")\n";
                    assert(false && "test case failed!");
                }
            }
        }
    }
}

/*!
 * Pointer analysis validator
 */
void PointerAnalysis::validateExpectedFailureTests(std::string fun)
{

    if (const SVFFunction* checkFun = svfMod->getSVFFunction(fun))
    {
        if(!checkFun->isUncalledFunction())
            outs() << "[" << this->PTAName() << "] Checking " << fun << "\n";

        for(const CallICFGNode* callNode : pag->getCallSiteSet())
        {
            if (callNode->getCalledFunction() == checkFun)
            {
                assert(callNode->arg_size() == 2
                       && "arguments should be two pointers!!");
                const SVFVar* V1 = callNode->getArgument(0);
                const SVFVar* V2 = callNode->getArgument(1);
                AliasResult aliasRes = alias(V1->getId(), V2->getId());

                bool expectedFailure = false;
                if (fun == aliasTestFailMayAlias || fun == aliasTestFailMayAliasMangled)
                {
                    // change to must alias when our analysis support it
                    if (aliasRes == AliasResult::NoAlias)
                        expectedFailure = true;
                }
                else if (fun == aliasTestFailNoAlias || fun == aliasTestFailNoAliasMangled)
                {
                    // change to partial alias when our analysis support it
                    if (aliasRes == AliasResult::MayAlias || aliasRes == AliasResult::PartialAlias || aliasRes == AliasResult::MustAlias)
                        expectedFailure = true;
                }
                else
                    assert(false && "not supported alias check!!");

                NodeID id1 = V1->getId();
                NodeID id2 = V2->getId();

                if (expectedFailure)
                    outs() << sucMsg("\t EXPECTED-FAILURE :") << fun << " check <id:" << id1 << ", id:" << id2 << "> at ("
                           << callNode->getSourceLoc() << ")\n";
                else
                {
                    SVFUtil::errs() << errMsg("\t UNEXPECTED FAILURE :") << fun << " check <id:" << id1 << ", id:" << id2 << "> at ("
                                    << callNode->getSourceLoc() << ")\n";
                    assert(false && "test case failed!");
                }
            }
        }
    }
}
