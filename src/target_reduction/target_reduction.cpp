/*!
    \file target_reduction.cpp
    \brief Standalone Target-Reduction optimization flow.
*/

#include "target_reduction.h"
#include "support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

using target_reduction::countSetBits;
using target_reduction::getNodeId;
using target_reduction::getNodeInv;
using target_reduction::hasAnyBit;
using target_reduction::readLitWord;
using target_reduction::reportFailure;
using target_reduction::wordMask;

// -----------------------------------------------------------------------------
// Step 1: target-fanin selection
// -----------------------------------------------------------------------------

// Collect live internal nodes from high level to low level.
void TargetReductionMan::collectReverseTopoNodes(std::vector<int> &reverseTopoOrder) const
{
    reverseTopoOrder.clear();
    reverseTopoOrder.reserve(std::size_t(
        miaig.nObjs > miaig.nPIs ? miaig.nObjs - miaig.nPIs - 1 : 0));
    for (int level = static_cast<int>(miaig.levelOffsets.size()) - 2;
         level >= 1; --level) {
        const int begin = miaig.levelOffsets[std::size_t(level)];
        const int end = miaig.levelOffsets[std::size_t(level + 1)];
        for (int pos = begin; pos < end; ++pos) {
            const int nodeId = miaig.levelNodes[std::size_t(pos)];
            if (nodeId > miaig.nPIs && nodeId < miaig.nObjs &&
                miaig.numFos[std::size_t(nodeId)] > 0) {
                reverseTopoOrder.push_back(nodeId);
            }
        }
    }
}

// Snapshot removable original fanins in their current array order.
void TargetReductionMan::collectRemovableFaninLits(
    int nodeId,
    std::vector<int> &pendingTargetFaninLits) const
{
    pendingTargetFaninLits.clear();
    pendingTargetFaninLits.reserve(
        std::size_t(miaig.faninCounts[std::size_t(nodeId)]));

    for (int targetFaninIdx = 0;
         targetFaninIdx < miaig.faninCounts[std::size_t(nodeId)]; ++targetFaninIdx) {
        const int lit =
            miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + targetFaninIdx];
        const int targetFaninNodeId = getNodeId(lit);
        if (targetFaninNodeId > miaig.nPIs &&
            targetFaninNodeId < miaig.nObjs &&
            miaig.numFos[std::size_t(targetFaninNodeId)] == 1 &&
            miaig.faninCounts[std::size_t(targetFaninNodeId)] > 1) {
            pendingTargetFaninLits.push_back(lit);
        }
    }
}

// Find a snapshotted literal after previous commits may have reordered fanins.
int TargetReductionMan::findCurrentFaninIndex(int nodeId, int targetFaninLit) const
{
    for (int targetFaninIdx = 0;
         targetFaninIdx < miaig.faninCounts[std::size_t(nodeId)]; ++targetFaninIdx) {
        if (miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + targetFaninIdx] ==
            targetFaninLit) {
            return targetFaninIdx;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Care-set and required-set analysis
// -----------------------------------------------------------------------------

// Compute the observable care set of one node.
bool TargetReductionMan::computeCareSet(int nodeId)
{
    if (nodeId < 0 || nodeId >= miaig.nObjs) {
        return reportFailure<bool>(__func__, "invalid nodeId for care-set computation");
    }
    if (!collectTfoScratch(nodeId)) {
        return reportFailure<bool>(__func__, "collectTfoScratch(nodeId) failed");
    }
    if (!tt.hasHostData() ||
        ttTemp.nObjs != miaig.nObjs || ttTemp.nWords != tt.nWords || ttTemp.nPOs != tt.nPOs ||
        careSet.size() != std::size_t(tt.nWords)) {
        return reportFailure<bool>(__func__, "TT/tempTT/careSet state is inconsistent");
    }

    if (!updateFlippedTTComp(nodeId, ttTemp)) {
        return reportFailure<bool>(__func__, "updateFlippedTTComp(nodeId, ttTemp) failed");
    }

    std::fill(careSet.begin(), careSet.end(), 0ULL);
    for (int poIdx = 0; poIdx < tt.nPOs; ++poIdx) {
        const int lit = miaig.pos[poIdx];
        const uint64_t *origWords = tt.poTT.data() + std::size_t(poIdx) * tt.nWords;
        const PackedTT &altTT =
            tfoMarksScratch[std::size_t(getNodeId(lit))] ? ttTemp : tt;
        for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
            const uint64_t altWord = readLitWord(
                altTT.nodeTT.data(), altTT.nPIs, altTT.nWords, lit, wordIdx);
            careSet[std::size_t(wordIdx)] |= origWords[wordIdx] ^ altWord;
        }
    }
    for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
        careSet[std::size_t(wordIdx)] &= wordMask(tt.nPIs, wordIdx);
    }
    return true;
}

// Split the care set of node f by its current function value.
bool TargetReductionMan::buildNodeCareOffWords(
    int nodeId,
    std::vector<uint64_t> &nodeCareOffWords)
{
    if (!computeCareSet(nodeId)) {
        return reportFailure<bool>(__func__, "computeCareSet(nodeId) failed");
    }
    if (!tt.hasHostData() || nodeId < 0 || nodeId >= tt.nObjs ||
        careSet.size() != std::size_t(tt.nWords) ||
        careOnSet.size() != std::size_t(tt.nWords)) {
        return reportFailure<bool>(
            __func__, "invalid node truth table or care masks");
    }

    const uint64_t *nodeWords =
        tt.nodeTT.data() + std::size_t(nodeId) * tt.nWords;
    nodeCareOffWords.assign(std::size_t(tt.nWords), 0ULL);
    for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
        const uint64_t mask = wordMask(tt.nPIs, wordIdx);
        const uint64_t nodeWord = nodeWords[wordIdx];
        careOnSet[std::size_t(wordIdx)] =
            careSet[std::size_t(wordIdx)] & nodeWord;
        nodeCareOffWords[std::size_t(wordIdx)] =
            careSet[std::size_t(wordIdx)] &
            (~nodeWord) & mask;
    }
    return true;
}

// Required bits are care-off patterns where this fanin is the only blocking zero.
bool TargetReductionMan::buildRequiredZeroOnCareOffWords(
    int nodeId,
    int targetFaninIdx,
    int targetFaninLit,
    const std::vector<uint64_t> &nodeCareOffWords,
    std::vector<uint64_t> &requiredWords) const
{
    requiredWords.assign(std::size_t(tt.nWords), 0ULL);
    for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
        const uint64_t mask = wordMask(tt.nPIs, wordIdx);
        const uint64_t targetFaninWord =
            readLitWord(tt.nodeTT.data(), tt.nPIs, tt.nWords, targetFaninLit, wordIdx);
        uint64_t otherFaninsWord = mask;
        const int faninCount = miaig.faninCounts[std::size_t(nodeId)];
        for (int otherFaninIdx = 0; otherFaninIdx < faninCount; ++otherFaninIdx) {
            if (otherFaninIdx == targetFaninIdx) {
                continue;
            }
            const int otherLit =
                miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + otherFaninIdx];
            otherFaninsWord &=
                readLitWord(tt.nodeTT.data(), tt.nPIs, tt.nWords, otherLit, wordIdx);
        }
        requiredWords[std::size_t(wordIdx)] =
            nodeCareOffWords[std::size_t(wordIdx)] & otherFaninsWord &
            (~targetFaninWord) & mask;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Step 2: candidate-wire collection
// -----------------------------------------------------------------------------

// Exclude sources that are structurally illegal for node f.
bool TargetReductionMan::faninCandidatesFiltering(int nodeId)
{
    if (!isLoaded() || miaig.levelOffsets.empty() ||
        nodeId <= miaig.nPIs || nodeId >= miaig.nObjs) {
        return reportFailure<bool>(
            __func__, "invalid state or nodeId for candidate filtering");
    }
    if (!collectTfoScratch(nodeId)) {
        return reportFailure<bool>(__func__, "collectTfoScratch(nodeId) failed");
    }

    candidateNodes.clear();
    candidateNodes.assign(miaig.nObjs, true);

    candidateNodes[0] = false;
    candidateNodes[nodeId] = false;
    for (int faninIdx = 0; faninIdx < miaig.maxNumFanins; ++faninIdx) {
        const int lit = miaig.fis[nodeId * miaig.maxNumFanins + faninIdx];
        if (lit <= 1) {
            continue;
        }
        candidateNodes[getNodeId(lit)] = false;
    }

    // A target, its current fanins, and every node in its TFO are excluded;
    // this avoids duplicate edges and combinational cycles.
    for (int tfoNodeId : tfoNodesScratch) {
        if (tfoNodeId >= 0 && tfoNodeId < miaig.nObjs) {
            candidateNodes[std::size_t(tfoNodeId)] = false;
        }
    }

    return true;
}

// Build sparse word lists used throughout candidate filtering and covering.
void TargetReductionMan::buildWireSearchContext(const std::vector<uint64_t> &requiredWords,
                                        WireSearchContext &searchContext) const
{
    searchContext = {};

    searchContext.careOnWordIdxs.reserve(std::size_t(tt.nWords));
    for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
        if (careOnSet[std::size_t(wordIdx)] != 0ULL) {
            searchContext.careOnWordIdxs.push_back(wordIdx);
        }
    }

    searchContext.requiredWordIdxs.reserve(std::size_t(tt.nWords));
    searchContext.uncoveredWords.reserve(std::size_t(tt.nWords));
    for (int wordIdx = 0; wordIdx < tt.nWords; ++wordIdx) {
        const uint64_t requiredWord = requiredWords[std::size_t(wordIdx)];
        if (requiredWord != 0ULL) {
            searchContext.requiredWordIdxs.push_back(wordIdx);
            searchContext.uncoveredWords.push_back(requiredWord);
        }
    }

    // Sample the same uniformly spaced 25% of active required words as the
    // original prefilter. Their order is part of candidate enumeration behavior.
    if (searchContext.requiredWordIdxs.size() > 4) {
        const std::size_t sampleCount = (searchContext.requiredWordIdxs.size() + 3) / 4;
        searchContext.prefilterActiveIdxs.reserve(sampleCount);
        for (std::size_t sampleIdx = 0; sampleIdx < sampleCount; ++sampleIdx) {
            searchContext.prefilterActiveIdxs.push_back(
                sampleIdx * searchContext.requiredWordIdxs.size() / sampleCount);
        }
    }
}

// Enumerate structurally legal sources and retain wires satisfying both care conditions.
void TargetReductionMan::collectValidWireCandidates(
    int nodeId,
    const std::vector<uint64_t> &requiredWords,
    const WireSearchContext &searchContext,
    std::vector<ValidWireCandidate> &validWires) const
{
    validWires.clear();
    const int maxCandidateWires = std::max(0, params.maxCandidateWires);
    validWires.reserve(std::size_t(maxCandidateWires > 0 ? maxCandidateWires : miaig.nObjs));

    const bool hasCareOnBit = !searchContext.careOnWordIdxs.empty();
    const uint64_t tailMask = wordMask(tt.nPIs, tt.nWords - 1);
    const uint64_t *nodeTT = tt.nodeTT.data();

    // Source ids, then polarities, are scanned in this exact order. The first
    // maxCandidateWires useful wires define the bounded search space.
    for (int srcNodeId = 0; srcNodeId < miaig.nObjs; ++srcNodeId) {
        if (params.preserveLevel &&
            miaig.levels[srcNodeId] > miaig.levels[nodeId]) {
            continue;
        }

        if (maxCandidateWires > 0 &&
            static_cast<int>(validWires.size()) >= maxCandidateWires) {
            break;
        }
        if (!candidateNodes[std::size_t(srcNodeId)] ||
            miaig.numFos[std::size_t(srcNodeId)] <= 0) {
            continue;
        }

        const uint64_t *srcWords = nodeTT + std::size_t(srcNodeId) * tt.nWords;
        bool legalInv[2] = {true, true};

        // Equation (care-on preserve): every selected wire must be one on all
        // observable patterns where the target function is one.
        if (hasCareOnBit) {
            for (int wordIdx : searchContext.careOnWordIdxs) {
                const uint64_t careOnWord = careOnSet[std::size_t(wordIdx)];
                const uint64_t srcWord = srcWords[wordIdx];
                if ((srcWord & careOnWord) != careOnWord) {
                    legalInv[0] = false;
                }
                if (((~srcWord) & careOnWord) != careOnWord) {
                    legalInv[1] = false;
                }
                if (!legalInv[0] && !legalInv[1]) {
                    break;
                }
            }
        }

        for (int inv = 0; inv <= 1; ++inv) {
            if (!legalInv[inv]) {
                continue;
            }

            if (!searchContext.prefilterActiveIdxs.empty()) {
                bool sampleCoversRequired = false;
                for (std::size_t activeIdx : searchContext.prefilterActiveIdxs) {
                    const int wordIdx = searchContext.requiredWordIdxs[activeIdx];
                    const uint64_t mask = (wordIdx + 1 == tt.nWords) ? tailMask : ~0ULL;
                    const uint64_t srcWord = srcWords[wordIdx];
                    const uint64_t candidateWord =
                        inv ? ((~srcWord) & mask) : (srcWord & mask);
                    const uint64_t zeroWord = (~candidateWord) & mask;
                    if ((requiredWords[std::size_t(wordIdx)] & zeroWord) != 0ULL) {
                        sampleCoversRequired = true;
                        break;
                    }
                }
                if (!sampleCoversRequired) {
                    if (hasCareOnBit) {
                        break;
                    }
                    continue;
                }
            }

            // Equation (required cover): cache exactly which required-zero bits
            // this polarity forces to zero for the later cover search.
            bool coversRequired = false;
            bool coversAllRequired = true;
            std::vector<uint64_t> coverageWords(searchContext.requiredWordIdxs.size(), 0ULL);
            for (std::size_t activeIdx = 0;
                 activeIdx < searchContext.requiredWordIdxs.size(); ++activeIdx) {
                const int wordIdx = searchContext.requiredWordIdxs[activeIdx];
                const uint64_t mask = (wordIdx + 1 == tt.nWords) ? tailMask : ~0ULL;
                const uint64_t srcWord = srcWords[wordIdx];
                const uint64_t candidateWord =
                    inv ? ((~srcWord) & mask) : (srcWord & mask);
                const uint64_t zeroWord = (~candidateWord) & mask;
                const uint64_t requiredWord = requiredWords[std::size_t(wordIdx)];
                const uint64_t coverageWord = requiredWord & zeroWord;
                coverageWords[activeIdx] = coverageWord;
                if (coverageWord != 0ULL) {
                    coversRequired = true;
                }
                if (coverageWord != requiredWord) {
                    coversAllRequired = false;
                }
            }
            if (!coversRequired) {
                if (hasCareOnBit) {
                    break;
                }
                continue;
            }

            validWires.push_back({{srcNodeId, inv}, std::move(coverageWords),
                                  coversAllRequired});
            if (maxCandidateWires > 0 &&
                static_cast<int>(validWires.size()) >= maxCandidateWires) {
                break;
            }
            if (hasCareOnBit) {
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Step 3: wire-set selection
// -----------------------------------------------------------------------------

// Select the first candidate that covers the complete required-zero set.
bool TargetReductionMan::selectSingleWire(const std::vector<ValidWireCandidate> &validWires,
                                  WireSelectionResult &result) const
{
    for (const ValidWireCandidate &candidate : validWires) {
        if (candidate.coversAllRequired) {
            result.wires.push_back(candidate.wire);
            result.found = true;
            return true;
        }
    }
    return false;
}

// Greedily cover required-zero bits, preserving candidate order and first-win ties.
bool TargetReductionMan::selectMultiWire(const std::vector<ValidWireCandidate> &validWires,
                                 WireSearchContext &searchContext,
                                 int maxWireCanAdd,
                                 WireSelectionResult &result) const
{
    std::vector<unsigned char> selectedSourceNodes(std::size_t(miaig.nObjs), 0);
    std::vector<uint64_t> coverageScratch(searchContext.requiredWordIdxs.size(), 0ULL);

    while (static_cast<int>(result.wires.size()) < maxWireCanAdd &&
           hasAnyBit(searchContext.uncoveredWords)) {
        ReplacementWireCandidate bestWire;
        std::vector<uint64_t> bestCoverage(searchContext.requiredWordIdxs.size(), 0ULL);
        int bestCoverageCount = 0;
        const int uncoveredCount = countSetBits(searchContext.uncoveredWords);
        bool foundFullCoverage = false;

        for (const ValidWireCandidate &candidate : validWires) {
            if (foundFullCoverage) {
                break;
            }
            if (selectedSourceNodes[std::size_t(candidate.wire.srcNodeId)] != 0) {
                continue;
            }

            int coverageCount = 0;
            for (std::size_t activeIdx = 0;
                 activeIdx < searchContext.requiredWordIdxs.size(); ++activeIdx) {
                const uint64_t coverageWord =
                    searchContext.uncoveredWords[activeIdx] &
                    candidate.coverageWords[activeIdx];
                coverageScratch[activeIdx] = coverageWord;
                coverageCount += __builtin_popcountll(coverageWord);
            }

            // Strict comparison retains the first candidate on equal coverage.
            if (coverageCount > bestCoverageCount) {
                bestCoverageCount = coverageCount;
                bestWire = candidate.wire;
                bestCoverage = coverageScratch;
                if (bestCoverageCount == uncoveredCount) {
                    foundFullCoverage = true;
                }
            }
        }

        if (bestCoverageCount <= 0) {
            break;
        }

        result.wires.push_back(bestWire);
        selectedSourceNodes[std::size_t(bestWire.srcNodeId)] = 1;
        for (std::size_t activeIdx = 0;
             activeIdx < searchContext.requiredWordIdxs.size(); ++activeIdx) {
            searchContext.uncoveredWords[activeIdx] &= ~bestCoverage[activeIdx];
        }
    }

    if (result.wires.empty() || hasAnyBit(searchContext.uncoveredWords)) {
        result.wires.clear();
        return false;
    }
    result.found = true;
    return true;
}

// Run candidate filtering, then single-wire and multi-wire modes in that order.
TargetReductionMan::WireSelectionResult TargetReductionMan::selectCandidateWires(
    int nodeId,
    const std::vector<uint64_t> &requiredWords,
    int maxWireCanAdd,
    int targetFaninNodeId) const
{
    WireSelectionResult result;
    if (maxWireCanAdd <= 0 ||
        miaig.faninCounts[std::size_t(nodeId)] + maxWireCanAdd > miaig.maxNumFanins ||
        !hasAnyBit(requiredWords)) {
        return result;
    }

    WireSearchContext searchContext;
    buildWireSearchContext(requiredWords, searchContext);
    if (searchContext.requiredWordIdxs.empty()) {
        return result;
    }

    std::vector<ValidWireCandidate> validWires;
    collectValidWireCandidates(
        nodeId, requiredWords, searchContext, validWires);

    if (maxWireCanAdd >= 1) {
        if (selectSingleWire(validWires, result)) {
            result.gain = estimateSelectionGain(targetFaninNodeId, result.wires);
            return result;
        }
        if (maxWireCanAdd == 1) {
            return result;
        }
    }

    if (selectMultiWire(validWires, searchContext, maxWireCanAdd, result)) {
        result.gain = estimateSelectionGain(targetFaninNodeId, result.wires);
    }
    return result;
}

// Estimate deleted MFFC nodes after accounting for selected sources that reuse them.
int TargetReductionMan::estimateSelectionGain(
    int targetFaninNodeId,
    const std::vector<ReplacementWireCandidate> &selectedWires) const
{
    if (selectedWires.empty() ||
        targetFaninNodeId <= miaig.nPIs || targetFaninNodeId >= miaig.nObjs) {
        return 0;
    }

    std::vector<unsigned char> rootMffcMarks;
    const int mffcSize = collectMffcMarks(targetFaninNodeId, rootMffcMarks);
    if (mffcSize <= 0 || rootMffcMarks.size() != std::size_t(miaig.nObjs)) {
        return -static_cast<int>(selectedWires.size());
    }

    // A selected source inside the removed MFFC preserves its marked fanin cone.
    std::vector<unsigned char> preserved(std::size_t(miaig.nObjs), 0);
    std::function<void(int)> preserveCone = [&](int nodeId) {
        if (nodeId <= miaig.nPIs || nodeId >= miaig.nObjs ||
            rootMffcMarks[std::size_t(nodeId)] == 0 ||
            preserved[std::size_t(nodeId)] != 0) {
            return;
        }
        preserved[std::size_t(nodeId)] = 1;
        const int faninCount = miaig.faninCounts[std::size_t(nodeId)];
        for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
            const int lit = miaig.fis[std::size_t(nodeId) * miaig.maxNumFanins + faninIdx];
            preserveCone(getNodeId(lit));
        }
    };
    for (const ReplacementWireCandidate &wire : selectedWires) {
        preserveCone(wire.srcNodeId);
    }

    int preservedMffcNodes = 0;
    for (unsigned char mark : preserved) {
        if (mark != 0) {
            ++preservedMffcNodes;
        }
    }
    return 1 + mffcSize - preservedMffcNodes - static_cast<int>(selectedWires.size());
}

// Build a reduction plan for one fanin without changing the MIAIG.
bool TargetReductionMan::prepareFaninReduction(
    int nodeId,
    int targetFaninIdx,
    int targetFaninLit,
    const std::vector<uint64_t> &nodeCareOffWords,
    bool &candidateNodesReady,
    FaninReductionPlan &plan)
{
    plan = {};
    const int targetFaninNodeId = getNodeId(targetFaninLit);

    // Revalidate the snapshotted fanin because earlier commits can change its cone.
    if (targetFaninNodeId <= miaig.nPIs || targetFaninNodeId >= miaig.nObjs ||
        miaig.numFos[std::size_t(targetFaninNodeId)] != 1 ||
        miaig.faninCounts[std::size_t(targetFaninNodeId)] <= 1) {
        return true;
    }

    // S_r(x): care-off patterns where this fanin is the target's only zero.
    std::vector<uint64_t> requiredWords;
    if (!buildRequiredZeroOnCareOffWords(nodeId, targetFaninIdx, targetFaninLit,
                                         nodeCareOffWords, requiredWords)) {
        return reportFailure<bool>(
            __func__, "buildRequiredZeroOnCareOffWords(...) failed during Target-Reduction");
    }
    const int requiredBitCount = countSetBits(requiredWords);
    if (requiredBitCount <= 0) {
        return true;
    }

    // A wire set cannot exceed the user budget, free slots of f, or removable MFFC size.
    int maxWireCanAdd = std::max(1, params.maxWiresPerFanin);
    const int faninMffcSize = computeMffcSize(targetFaninNodeId);
    maxWireCanAdd = std::min(
        maxWireCanAdd,
        miaig.maxNumFanins - miaig.faninCounts[std::size_t(nodeId)]);
    maxWireCanAdd = std::min(maxWireCanAdd, faninMffcSize);
    if (maxWireCanAdd <= 0) {
        if (params.verbose >= 2) {
            std::printf("target-reduction fail: node=%d target_fanin=%d mffc=%d required_bits=%d "
                        "reason=no_wire_budget\n",
                        nodeId, targetFaninNodeId, faninMffcSize, requiredBitCount);
        }
        return true;
    }

    // Structural legality depends only on node f and is built lazily once
    // for all fanin attempts made before the next commit.
    if (!candidateNodesReady) {
        if (!faninCandidatesFiltering(nodeId)) {
            return reportFailure<bool>(
                __func__, "faninCandidatesFiltering(nodeId) failed during Target-Reduction");
        }
        candidateNodesReady = true;
    }

    WireSelectionResult selection =
        selectCandidateWires(
            nodeId, requiredWords, maxWireCanAdd, targetFaninNodeId);
    if (!selection.found || selection.gain < 0) {
        if (params.verbose >= 2) {
            std::printf("target-reduction fail: node=%d target_fanin=%d mffc=%d required_bits=%d "
                        "max_wires=%d reason=no_wire_cover\n",
                        nodeId, targetFaninNodeId, faninMffcSize,
                        requiredBitCount, maxWireCanAdd);
        }
        return true;
    }

    plan.targetFaninIdx = targetFaninIdx;
    plan.targetFaninLit = targetFaninLit;
    plan.targetFaninNodeId = targetFaninNodeId;
    plan.mffcSize = faninMffcSize;
    plan.requiredBitCount = requiredBitCount;
    plan.maxWires = maxWireCanAdd;
    plan.selection = std::move(selection);
    return true;
}

// -----------------------------------------------------------------------------
// Step 4: fanin removal and cleanup
// -----------------------------------------------------------------------------

// Commit one prepared wire-addition/fanin-removal transaction.
bool TargetReductionMan::commitFaninReduction(
    int nodeId,
    const FaninReductionPlan &plan)
{
    // Preserve selected-wire order because it also fixes MIAIG fanin order.
    for (const ReplacementWireCandidate &wire : plan.selection.wires) {
        if (!miaig.addFanin(nodeId, wire.srcNodeId, wire.inv != 0)) {
            return reportFailure<bool>(
                __func__, "miaig.addFanin(nodeId, srcNodeId, inv) failed during Target-Reduction");
        }
    }
    if (!removeTargetFaninAndCleanup(nodeId, plan.targetFaninIdx)) {
        return reportFailure<bool>(
            __func__, "removeTargetFaninAndCleanup(nodeId, plan.targetFaninIdx) failed");
    }

    // Once its only fanout edge is removed, the old literal can be constantized;
    // removeTargetFaninAndCleanup has already cleaned newly dead fanin logic.
    const int constValue = getNodeInv(plan.targetFaninLit) ? 0 : 1;
    const int removedFaninEdges =
        reduceNodeToConstant(plan.targetFaninNodeId, constValue);
    if (removedFaninEdges < 0) {
        return reportFailure<bool>(
            __func__, "reduceNodeToConstant(plan.targetFaninNodeId) failed during Target-Reduction");
    }

    if (params.verbose >= 1) {
        std::printf("target-reduction commit: node=%d target_fanin=%d lit=%d mffc=%d "
                    "required_bits=%d max_wires=%d selected_wires=%zu "
                    "gain=%d const=%d removed_edges=%d\n",
                    nodeId, plan.targetFaninNodeId, plan.targetFaninLit, plan.mffcSize,
                    plan.requiredBitCount, plan.maxWires, plan.selection.wires.size(),
                    plan.selection.gain, constValue, removedFaninEdges);
    }
    return true;
}

// -----------------------------------------------------------------------------
// Overall workflow
// -----------------------------------------------------------------------------

// Process one node f while preserving its original target-fanin attempt order.
bool TargetReductionMan::optimizeNode(int nodeId)
{
    // Replacement wires are intentionally excluded from this snapshot and are
    // never reconsidered as removable fanins during the same node traversal.
    std::vector<int> pendingTargetFaninLits;
    collectRemovableFaninLits(nodeId, pendingTargetFaninLits);

    // All target fanins of f share the same care partition. A committed
    // reduction preserves f on its care set, so only S_r(x) must be rebuilt
    // for each remaining target fanin.
    std::vector<uint64_t> nodeCareOffWords;
    if (!pendingTargetFaninLits.empty() &&
        !buildNodeCareOffWords(nodeId, nodeCareOffWords)) {
        return reportFailure<bool>(
            __func__, "buildNodeCareOffWords(nodeId, ...) failed during Target-Reduction");
    }

    bool contextNeedsRefresh = false;
    while (miaig.faninCounts[std::size_t(nodeId)] < miaig.maxNumFanins &&
           !pendingTargetFaninLits.empty()) {
        bool committedOne = false;
        bool candidateNodesReady = false;
        for (std::size_t pendingIdx = 0;
             pendingIdx < pendingTargetFaninLits.size();) {
            const int targetFaninLit = pendingTargetFaninLits[pendingIdx];
            const int targetFaninIdx = findCurrentFaninIndex(nodeId, targetFaninLit);
            if (targetFaninIdx < 0) {
                pendingTargetFaninLits.erase(
                    pendingTargetFaninLits.begin() +
                    static_cast<std::ptrdiff_t>(pendingIdx));
                continue;
            }

            FaninReductionPlan plan;
            if (!prepareFaninReduction(nodeId, targetFaninIdx, targetFaninLit,
                                       nodeCareOffWords, candidateNodesReady, plan)) {
                if (contextNeedsRefresh && !refreshContextAfterNode(nodeId)) {
                    return reportFailure<bool>(
                        __func__, "failed to refresh context after a preparation error");
                }
                return false;
            }
            if (!plan.selection.found) {
                ++pendingIdx;
                continue;
            }

            if (!commitFaninReduction(nodeId, plan)) {
                return false;
            }
            pendingTargetFaninLits.erase(
                pendingTargetFaninLits.begin() +
                static_cast<std::ptrdiff_t>(pendingIdx));
            contextNeedsRefresh = true;
            committedOne = true;
            break;
        }

        if (!committedOne) {
            break;
        }
    }
    // Commits change only f and its TFO. All source and pending-fanin truth
    // tables stay valid, so one incremental refresh is sufficient for this f.
    if (contextNeedsRefresh && !refreshContextAfterNode(nodeId)) {
        return reportFailure<bool>(
            __func__, "refreshContextAfterNode(nodeId) failed after optimizing node");
    }
    return true;
}

// Run one pass by traversing nodes in the fixed reverse-level order.
int TargetReductionMan::run()
{
    if (!initializeContext()) {
        return reportFailure<int>(__func__, "initializeContext() failed");
    }

    std::vector<int> reverseTopoOrder;
    collectReverseTopoNodes(reverseTopoOrder);
    for (const int nodeId : reverseTopoOrder) {
        if (nodeId <= miaig.nPIs || nodeId >= miaig.nObjs ||
            miaig.numFos[std::size_t(nodeId)] <= 0) {
            continue;
        }
        if (!optimizeNode(nodeId)) {
            return -1;
        }
    }
    return 0;
}
