/*!
    \file support.cpp
    \brief Target-Reduction state management and graph support.
*/

#include "support.h"

#include "target_reduction.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace target_reduction {

// Count set bits across a vector of truth-table words.
int countSetBits(const std::vector<uint64_t> &bits)
{
    int count = 0;
    for (uint64_t word : bits) {
        count += __builtin_popcountll(word);
    }
    return count;
}

// Check whether any truth-table word contains a set bit.
bool hasAnyBit(const std::vector<uint64_t> &bits)
{
    for (uint64_t word : bits) {
        if (word != 0ULL) {
            return true;
        }
    }
    return false;
}

} // namespace target_reduction

namespace {

using target_reduction::getNodeId;

// Collect one node's TFO in the existing level order.
bool collectTfoNodes(const Miaig &miaig,
                     int nodeId,
                     std::vector<int> &tfoNodesScratch,
                     std::vector<unsigned char> &tfoMarksScratch)
{
    tfoNodesScratch.clear();
    tfoNodesScratch.push_back(nodeId);
    tfoMarksScratch.assign(std::size_t(miaig.nObjs), 0);
    tfoMarksScratch[std::size_t(nodeId)] = 1;

    const int nodeLevel = miaig.levels[nodeId];
    for (int level = nodeLevel + 1;
         level + 1 < int(miaig.levelOffsets.size()); ++level) {
        const int begin = miaig.levelOffsets[level];
        const int end = miaig.levelOffsets[level + 1];
        for (int pos = begin; pos < end; ++pos) {
            const int updateNodeId = miaig.levelNodes[pos];
            if (updateNodeId <= miaig.nPIs) {
                continue;
            }

            bool inTfo = false;
            const int faninCount = miaig.faninCounts[std::size_t(updateNodeId)];
            for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
                const int lit =
                    miaig.fis[std::size_t(updateNodeId) * miaig.maxNumFanins + faninIdx];
                const int faninNodeId = getNodeId(lit);
                if (faninNodeId >= 0 && faninNodeId < miaig.nObjs &&
                    tfoMarksScratch[std::size_t(faninNodeId)] != 0) {
                    inTfo = true;
                    break;
                }
            }

            if (inTfo) {
                tfoMarksScratch[std::size_t(updateNodeId)] = 1;
                tfoNodesScratch.push_back(updateNodeId);
            }
        }
    }
    return true;
}

} // namespace

using target_reduction::getNodeId;
using target_reduction::reportFailure;

// Release all owned Target-Reduction state.
TargetReductionMan::~TargetReductionMan()
{
    clear();
}

// Load an AIG into Target-Reduction's MIAIG working state.
bool TargetReductionMan::loadAigToMiaig(const AIGMan &aigman, int maxNumFanins)
{
    const int fanins = maxNumFanins > 0 ? maxNumFanins : params.maxNumFanins;
    if (!miaigBackup.loadAigToMiaig(aigman, fanins)) {
        clear();
        return reportFailure<bool>(
            __func__, "miaigBackup.loadAigToMiaig(aigman, fanins) failed");
    }
    if (!miaig.setMiaig(miaigBackup)) {
        clear();
        return reportFailure<bool>(
            __func__, "miaig.setMiaig(miaigBackup) failed");
    }
    tt.clear();
    ttTemp.clear();
    careSet.clear();
    careOnSet.clear();
    candidateNodes.clear();
    invalidateTfoScratch();
    params.maxNumFanins = fanins;
    return true;
}

// Replace Target-Reduction's working state with an existing MIAIG.
bool TargetReductionMan::setMiaig(const Miaig &input)
{
    if (!miaigBackup.setMiaig(input) || !miaig.setMiaig(miaigBackup)) {
        clear();
        return reportFailure<bool>(
            __func__, "failed to copy input MIAIG into backup/current state");
    }
    tt.clear();
    ttTemp.clear();
    careSet.clear();
    careOnSet.clear();
    candidateNodes.clear();
    invalidateTfoScratch();
    params.maxNumFanins = miaig.maxNumFanins;
    return true;
}

// Build all CPU-side views used by one Target-Reduction pass.
bool TargetReductionMan::initializeContext()
{
    if (!isLoaded()) {
        return reportFailure<bool>(__func__, "no loaded MIAIG state");
    }
    if (!tt.simulate(miaig)) {
        return reportFailure<bool>(
            __func__, "tt.simulate(miaig) failed");
    }
    if (!ttTemp.initFromMiaig(miaig)) {
        return reportFailure<bool>(
            __func__, "ttTemp.initFromMiaig(miaig) failed");
    }
    careSet.assign(std::size_t(tt.nWords), 0ULL);
    careOnSet.assign(std::size_t(tt.nWords), 0ULL);
    invalidateTfoScratch();
    return true;
}

// Incrementally resimulate one optimized node and its TFO.
bool TargetReductionMan::refreshContextAfterNode(int changedNodeId)
{
    if (!isLoaded() || changedNodeId <= miaig.nPIs || changedNodeId >= miaig.nObjs) {
        return reportFailure<bool>(__func__, "invalid state or changedNodeId");
    }
    if (!tt.resimulateTfo(miaig, changedNodeId)) {
        return reportFailure<bool>(
            __func__, "tt.resimulateTfo(miaig, changedNodeId) failed");
    }
    if (!ttTemp.hasHostData() ||
        ttTemp.nPIs != miaig.nPIs || ttTemp.nObjs != miaig.nObjs ||
        ttTemp.nPOs != miaig.nPOs ||
        ttTemp.nodeTT.size() != std::size_t(miaig.nObjs) * tt.nWords ||
        ttTemp.poTT.size() != std::size_t(miaig.nPOs) * tt.nWords) {
        if (!ttTemp.initFromMiaig(miaig)) {
            return reportFailure<bool>(
                __func__, "ttTemp.initFromMiaig(miaig) failed");
        }
    }
    if (careSet.size() != std::size_t(tt.nWords)) {
        careSet.assign(std::size_t(tt.nWords), 0ULL);
    }
    if (careOnSet.size() != std::size_t(tt.nWords)) {
        careOnSet.assign(std::size_t(tt.nWords), 0ULL);
    }
    invalidateTfoScratch();
    return true;
}

// Clear cached TFO scratch data.
void TargetReductionMan::invalidateTfoScratch()
{
    tfoNodesScratch.clear();
    tfoMarksScratch.clear();
    tfoRootScratch = -1;
}

// Collect or reuse cached TFO data for one node.
bool TargetReductionMan::collectTfoScratch(int nodeId)
{
    if (!isLoaded() || miaig.levelOffsets.empty() ||
        nodeId <= miaig.nPIs || nodeId >= miaig.nObjs) {
        return reportFailure<bool>(
            __func__, "invalid state or nodeId for TFO collection");
    }
    if (tfoRootScratch == nodeId && !tfoNodesScratch.empty() &&
        tfoMarksScratch.size() == std::size_t(miaig.nObjs)) {
        return true;
    }
    if (!collectTfoNodes(
            miaig, nodeId, tfoNodesScratch, tfoMarksScratch)) {
        invalidateTfoScratch();
        return reportFailure<bool>(
            __func__, "collectTfoNodes(miaig, nodeId, ...) failed");
    }
    tfoRootScratch = nodeId;
    return true;
}

// Simulate the network after flipping one node's truth table.
bool TargetReductionMan::updateFlippedTTComp(int nodeId, PackedTT &out)
{
    if (!tt.hasHostData() ||
        nodeId < 0 || nodeId >= miaig.nObjs) {
        return reportFailure<bool>(
            __func__, "invalid truth-table state or nodeId");
    }
    if (out.nObjs != tt.nObjs || out.nPIs != tt.nPIs ||
        out.nPOs != tt.nPOs || out.nWords != tt.nWords) {
        return reportFailure<bool>(
            __func__, "output TT shape does not match base TT");
    }
    if (!collectTfoScratch(nodeId)) {
        return reportFailure<bool>(
            __func__, "collectTfoScratch(nodeId) failed");
    }

    return out.simulateFlippedTfo(
        miaig, tt, nodeId, tfoNodesScratch, tfoMarksScratch);
}

// Mark and count the MFFC nodes that would be deleted if root loses all fanouts.
int TargetReductionMan::collectMffcMarks(
    int rootNodeId,
    std::vector<unsigned char> &mffcMarks) const
{
    if (!isLoaded() || rootNodeId < 0 || rootNodeId >= miaig.nObjs) {
        return reportFailure<int>(__func__, "invalid state or rootNodeId");
    }
    if (rootNodeId <= miaig.nPIs ||
        miaig.numFos[std::size_t(rootNodeId)] <= 0) {
        mffcMarks.assign(std::size_t(miaig.nObjs), 0);
        return 0;
    }

    std::vector<int> remainingFanouts = miaig.numFos;
    mffcMarks.assign(std::size_t(miaig.nObjs), 0);

    std::function<int(int)> dereference = [&](int nodeId) -> int {
        if (nodeId <= miaig.nPIs || nodeId >= miaig.nObjs ||
            mffcMarks[std::size_t(nodeId)] != 0) {
            return 0;
        }
        mffcMarks[std::size_t(nodeId)] = 1;

        int size = 1;
        const int faninCount = miaig.faninCounts[std::size_t(nodeId)];
        for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
            const int lit =
                miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + faninIdx];
            const int faninNodeId = getNodeId(lit);
            if (faninNodeId <= miaig.nPIs || faninNodeId >= miaig.nObjs) {
                continue;
            }
            if (remainingFanouts[std::size_t(faninNodeId)] > 0) {
                --remainingFanouts[std::size_t(faninNodeId)];
            }
            if (remainingFanouts[std::size_t(faninNodeId)] == 0) {
                size += dereference(faninNodeId);
            }
        }
        return size;
    };
    return dereference(rootNodeId);
}

// Return the number of nodes in one MFFC.
int TargetReductionMan::computeMffcSize(int rootNodeId) const
{
    std::vector<unsigned char> mffcMarks;
    return collectMffcMarks(rootNodeId, mffcMarks);
}

// Remove target fanin x from node f and recursively clean newly dead fanins.
bool TargetReductionMan::removeTargetFaninAndCleanup(int nodeId, int targetFaninIdx)
{
    if (!isLoaded() || nodeId <= miaig.nPIs || nodeId >= miaig.nObjs ||
        targetFaninIdx < 0 ||
        targetFaninIdx >= miaig.faninCounts[std::size_t(nodeId)]) {
        return reportFailure<bool>(
            __func__, "invalid state, nodeId, or targetFaninIdx");
    }

    const int faninCount = miaig.faninCounts[std::size_t(nodeId)];
    const std::size_t nodeOffset = std::size_t(nodeId) * miaig.maxNumFanins;
    const int removedLit = miaig.fis[nodeOffset + targetFaninIdx];
    const int removedNodeId = getNodeId(removedLit);
    if (removedNodeId > 0 && removedNodeId < miaig.nObjs &&
        miaig.numFos[std::size_t(removedNodeId)] > 0) {
        --miaig.numFos[std::size_t(removedNodeId)];
    }

    if (faninCount <= 1) {
        miaig.fis[nodeOffset] = 1;
        miaig.faninCounts[std::size_t(nodeId)] = 1;
    } else {
        const int lastFaninIdx = faninCount - 1;
        if (targetFaninIdx != lastFaninIdx) {
            miaig.fis[nodeOffset + targetFaninIdx] =
                miaig.fis[nodeOffset + lastFaninIdx];
        }
        miaig.fis[nodeOffset + lastFaninIdx] = 1;
        miaig.faninCounts[std::size_t(nodeId)] = lastFaninIdx;
    }

    if (removedNodeId > 0 && removedNodeId < miaig.nObjs &&
        miaig.numFos[std::size_t(removedNodeId)] == 0) {
        std::vector<int> stack{removedNodeId};
        std::vector<unsigned char> processed(std::size_t(miaig.nObjs), 0);
        while (!stack.empty()) {
            const int nodeId = stack.back();
            stack.pop_back();
            if (nodeId <= miaig.nPIs || nodeId >= miaig.nObjs ||
                miaig.numFos[std::size_t(nodeId)] != 0 ||
                processed[std::size_t(nodeId)] != 0) {
                continue;
            }
            processed[std::size_t(nodeId)] = 1;

            const int deadFaninCount = miaig.faninCounts[std::size_t(nodeId)];
            for (int faninIdx = 0; faninIdx < deadFaninCount; ++faninIdx) {
                const std::size_t offset =
                    std::size_t(nodeId) * miaig.maxNumFanins + faninIdx;
                const int lit = miaig.fis[offset];
                const int faninNodeId = getNodeId(lit);
                if (faninNodeId <= 0 || faninNodeId >= miaig.nObjs ||
                    miaig.numFos[std::size_t(faninNodeId)] <= 0) {
                    miaig.fis[offset] = 1;
                    continue;
                }
                --miaig.numFos[std::size_t(faninNodeId)];
                miaig.fis[offset] = 1;
                if (miaig.numFos[std::size_t(faninNodeId)] == 0) {
                    stack.push_back(faninNodeId);
                }
            }
            for (int faninIdx = deadFaninCount;
                 faninIdx < miaig.maxNumFanins; ++faninIdx) {
                miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + faninIdx] = 1;
            }
            miaig.faninCounts[std::size_t(nodeId)] = 1;
        }
    }

    if (!miaig.updateLevel()) {
        return reportFailure<bool>(
            __func__, "miaig.updateLevel() failed after removing target fanin");
    }
    return true;
}

// Replace one node by a constant literal.
int TargetReductionMan::reduceNodeToConstant(int targetFaninNodeId, int constLit)
{
    if (constLit < 0) {
        return reportFailure<int>(__func__, "invalid constant literal");
    }

    int removedCount = 0;
    const int faninCount = miaig.faninCounts[std::size_t(targetFaninNodeId)];
    for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
        const int oldLit =
            miaig.fis[std::size_t(targetFaninNodeId) * miaig.maxNumFanins + faninIdx];
        if (getNodeId(oldLit) > 0) {
            ++removedCount;
        }
    }

    if (!miaig.setConstant(targetFaninNodeId, constLit)) {
        return reportFailure<int>(
            __func__, "miaig.setConstant(targetFaninNodeId, constLit) failed");
    }
    if (params.verbose > 0) {
        std::printf("Reduce node %d to const%d\n", targetFaninNodeId, constLit);
    }
    return removedCount;
}

// Compare current MIAIG outputs against the loaded backup.
bool TargetReductionMan::equivalenceCheck()
{
    if (!isLoaded()) {
        return reportFailure<bool>(
            __func__, "no loaded MIAIG state for equivalence check");
    }

    PackedTT backupTT;
    if (!miaigBackup.hasHostData()) {
        return reportFailure<bool>(__func__, "miaigBackup host data missing");
    }
    if (!backupTT.simulate(miaigBackup)) {
        return reportFailure<bool>(
            __func__, "backupTT.simulate(miaigBackup) failed");
    }

    PackedTT currentTT;
    if (!miaig.hasHostData()) {
        return reportFailure<bool>(__func__, "current miaig host data missing");
    }
    if (!currentTT.simulate(miaig)) {
        return reportFailure<bool>(
            __func__, "currentTT.simulate(miaig) failed");
    }
    return backupTT.poTT == currentTT.poTT;
}

// Clear all Target-Reduction manager state.
void TargetReductionMan::clear()
{
    ttTemp.clear();
    miaigBackup.clear();
    tt.clear();
    careSet.clear();
    careOnSet.clear();
    candidateNodes.clear();
    invalidateTfoScratch();
    miaig.clear();
}

// Check whether both backup and working MIAIGs are loaded.
bool TargetReductionMan::isLoaded() const
{
    return miaigBackup.hasHostData() && miaig.hasHostData();
}
