//===- PTAStat.cpp -- Base class for statistics in SVF-----------------------//
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
 * PTAStat.cpp
 *
 *  Created on: Oct 13, 2013
 *      Author: Yulei Sui
 */

#include <iomanip>
#include "Graphs/PTACallGraph.h"
#include "Util/PTAStat.h"
#include "MemoryModel/PointerAnalysisImpl.h"
#include "SVFIR/SVFIR.h"
#include <queue>
#include <set>

using namespace SVF;
using namespace std;

PTAStat::PTAStat(PointerAnalysis* p) : SVFStat(),
    pta(p),
    _vmrssUsageBefore(0),
    _vmrssUsageAfter(0),
    _vmsizeUsageBefore(0),
    _vmsizeUsageAfter(0)
{
    u32_t vmrss = 0;
    u32_t vmsize = 0;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    setMemUsageBefore(vmrss, vmsize);
}

void PTAStat::performStat()
{

    SVFStat::performStat();

    callgraphStat();

    SVFIR* pag = SVFIR::getPAG();
    for(SVFIR::iterator it = pag->begin(), eit = pag->end(); it!=eit; ++it)
    {
        PAGNode* node = it->second;
        if(SVFUtil::isa<ObjVar>(node))
        {
            if(pta->isLocalVarInRecursiveFun(node->getId()))
            {
                localVarInRecursion.set(node->getId());
            }
        }
    }

    std::cout << "开始统计 indirect call sites" << std::endl;
    const SVFIR::CallSiteToFunPtrMap& indirectCallsites = pag->getIndirectCallsites();
    u32_t totalIndirectCallsitePtsSize = 0;
    u32_t indirectCallsiteSetSize = indirectCallsites.size();
    double averageIndirectCallsitePtsSize = 0;
    for (const auto& pair : indirectCallsites)
    {
        // const CallICFGNode* indirectCallsite = pair.first;
        NodeID funcNodeId = pair.second;
        const PointsTo& pts = pta->getPts(funcNodeId);
        u32_t size = pts.count();
        totalIndirectCallsitePtsSize += size;
    }
    if (0 != indirectCallsiteSetSize)
    {
        averageIndirectCallsitePtsSize = (double)totalIndirectCallsitePtsSize / indirectCallsiteSetSize;
    }

    switch(pta->getAnalysisTy())
    {
    case PointerAnalysis::PTATY::AndersenWaveDiff_WPA:
        timeStatMap["zzz_ander_TotalIndirectCallsitePtsSize:"] = totalIndirectCallsitePtsSize;
        timeStatMap["zzz_ander_IndirectCallsiteSetSize:"] = indirectCallsiteSetSize;
        timeStatMap["zzz_ander_AverageIndirectCallsitePtsSize:"] = averageIndirectCallsitePtsSize;
        break;
    case PointerAnalysis::PTATY::AndersenSCD_WPA:
        timeStatMap["zzz_sander_TotalIndirectCallsitePtsSize:"] = totalIndirectCallsitePtsSize;
        timeStatMap["zzz_sander_IndirectCallsiteSetSize:"] = indirectCallsiteSetSize;
        timeStatMap["zzz_sander_AverageIndirectCallsitePtsSize:"] = averageIndirectCallsitePtsSize;
        break;
    case PointerAnalysis::PTATY::FSSPARSE_WPA:
        timeStatMap["zzz_fspta_TotalIndirectCallsitePtsSize:"] = totalIndirectCallsitePtsSize;
        timeStatMap["zzz_fspta_IndirectCallsiteSetSize:"] = indirectCallsiteSetSize;
        timeStatMap["zzz_fspta_AverageIndirectCallsitePtsSize:"] = averageIndirectCallsitePtsSize;
        break;
    case PointerAnalysis::PTATY::VFS_WPA:
        timeStatMap["zzz_vfspta_TotalIndirectCallsitePtsSize:"] = totalIndirectCallsitePtsSize;
        timeStatMap["zzz_vfspta_IndirectCallsiteSetSize:"] = indirectCallsiteSetSize;
        timeStatMap["zzz_vfspta_AverageIndirectCallsitePtsSize:"] = averageIndirectCallsitePtsSize;
        break;
    default:
        timeStatMap["zzz_TotalIndirectCallsitePtsSize:"] = totalIndirectCallsitePtsSize;
        timeStatMap["zzz_IndirectCallsiteSetSize:"] = indirectCallsiteSetSize;
        timeStatMap["zzz_AverageIndirectCallsitePtsSize:"] = averageIndirectCallsitePtsSize;
    }
    std::cout << "结束统计 indirect call sites" << std::endl;

    PTNumStatMap["LocalVarInRecur"] = localVarInRecursion.count();

    u32_t vmrss = 0;
    u32_t vmsize = 0;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    setMemUsageAfter(vmrss, vmsize);
    timeStatMap["MemoryUsageVmrss"] = _vmrssUsageAfter - _vmrssUsageBefore;
    timeStatMap["MemoryUsageVmsize"] = _vmsizeUsageAfter - _vmsizeUsageBefore;
}

void PTAStat::callgraphStat()
{

    PTACallGraph* graph = pta->getCallGraph();
    PointerAnalysis::CallGraphSCC* callgraphSCC = new PointerAnalysis::CallGraphSCC(graph);
    callgraphSCC->find();

    unsigned totalNode = 0;
    unsigned totalCycle = 0;
    unsigned nodeInCycle = 0;
    unsigned maxNodeInCycle = 0;
    unsigned totalEdge = 0;
    unsigned edgeInCycle = 0;

    NodeSet sccRepNodeSet;
    PTACallGraph::iterator it = graph->begin();
    PTACallGraph::iterator eit = graph->end();
    for (; it != eit; ++it)
    {
        totalNode++;
        if(callgraphSCC->isInCycle(it->first))
        {
            sccRepNodeSet.insert(callgraphSCC->repNode(it->first));
            nodeInCycle++;
            const NodeBS& subNodes = callgraphSCC->subNodes(it->first);
            if(subNodes.count() > maxNodeInCycle)
                maxNodeInCycle = subNodes.count();
        }

        PTACallGraphNode::const_iterator edgeIt = it->second->InEdgeBegin();
        PTACallGraphNode::const_iterator edgeEit = it->second->InEdgeEnd();
        for (; edgeIt != edgeEit; ++edgeIt)
        {
            PTACallGraphEdge*edge = *edgeIt;
            totalEdge+= edge->getDirectCalls().size() + edge->getIndirectCalls().size();
            if(callgraphSCC->repNode(edge->getSrcID()) == callgraphSCC->repNode(edge->getDstID()))
            {
                edgeInCycle+=edge->getDirectCalls().size() + edge->getIndirectCalls().size();
            }
        }
    }

    totalCycle = sccRepNodeSet.size();

    // FunReachableFromEntry
    std::cout << "开始统计 FunReachableFromEntry" << std::endl;
    std::vector<PTACallGraphNode*> entry = graph->getCallGraphEntry();
    int funReachableFromEntry = 0;
    std::set<PTACallGraphNode*> visited;
    for (std::vector<PTACallGraphNode*>::iterator it = entry.begin();
         it != entry.end(); ++it)
    {
        std::queue<PTACallGraphNode*> queue;
        if (visited.find(*it) == visited.end())
        {
            queue.push(*it);
            visited.insert(*it);
            ++funReachableFromEntry; // visit
        }
        while (!queue.empty())
        {
            PTACallGraphNode* current = queue.front();
            queue.pop();

            for (PTACallGraphEdge* edge : current->getOutEdges())
            {
                PTACallGraphNode* dst = edge->getDstNode();
                if (visited.find(dst) == visited.end())
                {
                    queue.push(dst);
                    visited.insert(dst);
                    ++funReachableFromEntry;
                }
            }
        }
    }
    switch(pta->getAnalysisTy())
    {
    case PointerAnalysis::PTATY::AndersenWaveDiff_WPA:
        PTNumStatMap["zzz_ander_FunReachableFromEntry:"] = funReachableFromEntry;
        break;
    case PointerAnalysis::PTATY::AndersenSCD_WPA:
        PTNumStatMap["zzz_sander_FunReachableFromEntry:"] = funReachableFromEntry;
        break;
    case PointerAnalysis::PTATY::FSSPARSE_WPA:
        PTNumStatMap["zzz_fspta_FunReachableFromEntry:"] = funReachableFromEntry;
        break;
    case PointerAnalysis::PTATY::VFS_WPA:
        PTNumStatMap["zzz_vfspta_FunReachableFromEntry:"] = funReachableFromEntry;
        break;
    default:
        PTNumStatMap["zzz_pta_FunReachableFromEntry:"] = funReachableFromEntry;
    }
    PTNumStatMap["zzz_FunReachableFromEntry:"] = funReachableFromEntry;
    std::cout << "结束统计 FunReachableFromEntry" << std::endl;

    PTNumStatMap["TotalNode"] = totalNode;
    PTNumStatMap["TotalCycle"] = totalCycle;
    PTNumStatMap["NodeInCycle"] = nodeInCycle;
    PTNumStatMap["MaxNodeInCycle"] = maxNodeInCycle;
    PTNumStatMap["TotalEdge"] = totalEdge;
    PTNumStatMap["CalRetPairInCycle"] = edgeInCycle;

    if(pta->getAnalysisTy() >= PointerAnalysis::PTATY::Andersen_BASE && pta->getAnalysisTy() <= PointerAnalysis::PTATY::Steensgaard_WPA)
        SVFStat::printStat("PTACallGraph Stats (Andersen analysis)");
    else if(pta->getAnalysisTy() >= PointerAnalysis::PTATY::FSDATAFLOW_WPA && pta->getAnalysisTy() <= PointerAnalysis::PTATY::FSCS_WPA)
        SVFStat::printStat("PTACallGraph Stats (Flow-sensitive analysis)");
    else if(pta->getAnalysisTy() >= PointerAnalysis::PTATY::CFLFICI_WPA && pta->getAnalysisTy() <= PointerAnalysis::PTATY::CFLFSCS_WPA)
        SVFStat::printStat("PTACallGraph Stats (CFL-R analysis)");
    else if(pta->getAnalysisTy() >= PointerAnalysis::PTATY::FieldS_DDA && pta->getAnalysisTy() <= PointerAnalysis::PTATY::Cxt_DDA)
        SVFStat::printStat("PTACallGraph Stats (DDA analysis)");
    else
        SVFStat::printStat("PTACallGraph Stats");

    delete callgraphSCC;
}
