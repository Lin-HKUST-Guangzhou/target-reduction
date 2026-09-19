/*!
    \file target_reduction.h
    \brief Standalone Target-Reduction optimization manager.
*/

#ifndef __TARGET_REDUCTION_H__
#define __TARGET_REDUCTION_H__

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../utils/miaig.h"
#include "../utils/packed_truthtable.h"

class AIGMan;

struct TargetReductionParams
{
    int maxNumFanins = 8;
    int maxWiresPerFanin = 3;
    int maxCandidateWires = 2048;
    bool preserveLevel = true;
    int verbose = 0;
};

class TargetReductionMan
{
public:
    TargetReductionMan() = default;

    // Release all Target-Reduction state.
    ~TargetReductionMan();

    // Load an AIG into Target-Reduction's MIAIG state.
    bool loadAigToMiaig(const AIGMan &aigman, int maxNumFanins = -1);

    // Replace the loaded MIAIG state.
    bool setMiaig(const Miaig &input);

    // Run one Target-Reduction optimization pass.
    int run();

    // Check current outputs against the backup network.
    bool equivalenceCheck();

    // Clear all allocated Target-Reduction state.
    void clear();

    // Return whether backup and working MIAIGs are loaded.
    bool isLoaded() const;

    TargetReductionParams params;
    Miaig miaigBackup;
    Miaig miaig;
    PackedTT tt;
    PackedTT ttTemp;
    std::vector<uint64_t> careSet;
    std::vector<uint64_t> careOnSet;

private:
    // A candidate wire that may replace the selected target fanin x of node f.
    struct ReplacementWireCandidate {
        int srcNodeId = -1;
        int inv = 0;
    };

    // A function-preserving wire together with its required-zero coverage.
    struct ValidWireCandidate {
        ReplacementWireCandidate wire;
        std::vector<uint64_t> coverageWords;
        bool coversAllRequired = false;
    };

    // Sparse word indices shared by candidate filtering and wire-set search.
    struct WireSearchContext {
        std::vector<int> careOnWordIdxs;
        std::vector<int> requiredWordIdxs;
        std::vector<uint64_t> uncoveredWords;
        std::vector<std::size_t> prefilterActiveIdxs;
    };

    // The wire set selected for one target fanin.
    struct WireSelectionResult {
        bool found = false;
        int gain = 0;
        std::vector<ReplacementWireCandidate> wires;
    };

    // All data needed to remove one selected target fanin x from node f.
    struct FaninReductionPlan {
        int targetFaninIdx = -1;
        int targetFaninLit = -1;
        int targetFaninNodeId = -1;
        int mffcSize = 0;
        int requiredBitCount = 0;
        int maxWires = 0;
        WireSelectionResult selection;
    };

    // Pass context ---------------------------------------------------------

    // Build CPU views needed by one Target-Reduction pass.
    bool initializeContext();

    // Refresh truth-table state after processing one changed node.
    bool refreshContextAfterNode(int changedNodeId);

    // Step 1: target-fanin selection ---------------------------------------

    // Collect internal nodes f from high level to low level.
    void collectReverseTopoNodes(std::vector<int> &reverseTopoOrder) const;

    // Snapshot eligible original fanin literals in their current order.
    void collectRemovableFaninLits(int nodeId,
                                   std::vector<int> &pendingTargetFaninLits) const;

    // Locate a snapshotted target fanin in the current fanin array of node f.
    int findCurrentFaninIndex(int nodeId, int targetFaninLit) const;

    // Care-set and required-set analysis -----------------------------------

    // Invalidate cached TFO scratch data.
    void invalidateTfoScratch();

    // Collect cached TFO data for one node.
    bool collectTfoScratch(int nodeId);

    // Simulate the network after flipping one node.
    bool updateFlippedTTComp(int nodeId, PackedTT &out);

    // Compute the observable care set for one node.
    bool computeCareSet(int nodeId);

    // Build care-off bits for node f under its current TFO context.
    bool buildNodeCareOffWords(int nodeId, std::vector<uint64_t> &nodeCareOffWords);

    // For one target fanin edge, find care-off bits where this edge must be forced to zero.
    bool buildRequiredZeroOnCareOffWords(int nodeId,
                                         int targetFaninIdx,
                                         int targetFaninLit,
                                         const std::vector<uint64_t> &nodeCareOffWords,
                                         std::vector<uint64_t> &requiredWords) const;

    // Step 2: candidate-wire collection ------------------------------------

    // Filter structural source nodes: constants, fanins, and the TFO of f are illegal.
    bool faninCandidatesFiltering(int nodeId);

    // Build sparse truth-table word lists without changing their original order.
    void buildWireSearchContext(const std::vector<uint64_t> &requiredWords,
                                   WireSearchContext &searchContext) const;

    // Enumerate source ids and polarities and cache each valid wire's coverage.
    void collectValidWireCandidates(
        int nodeId,
        const std::vector<uint64_t> &requiredWords,
        const WireSearchContext &searchContext,
        std::vector<ValidWireCandidate> &validWires) const;

    // Step 3: wire-set selection -------------------------------------------

    // Return the first valid wire that covers the complete required-zero set.
    bool selectSingleWire(const std::vector<ValidWireCandidate> &validWires,
                             WireSelectionResult &result) const;

    // Greedily cover remaining required-zero bits using the existing candidate order.
    bool selectMultiWire(const std::vector<ValidWireCandidate> &validWires,
                            WireSearchContext &searchContext,
                            int maxWireCanAdd,
                            WireSelectionResult &result) const;

    // Select replacement wires for one target fanin edge.
    WireSelectionResult selectCandidateWires(
        int nodeId,
        const std::vector<uint64_t> &requiredWords,
        int maxWireCanAdd,
        int targetFaninNodeId) const;

    // Mark root MFFC nodes. Single-fanout gain discounts marked nodes reused as sources.
    int collectMffcMarks(int rootNodeId, std::vector<unsigned char> &mffcMarks) const;

    // Compute the MFFC size for one root node.
    int computeMffcSize(int rootNodeId) const;

    // Estimate the structural gain of one selected wire set.
    int estimateSelectionGain(
        int targetFaninNodeId,
        const std::vector<ReplacementWireCandidate> &selectedWires) const;

    // Prepare, but do not mutate the graph for, one target-fanin reduction.
    bool prepareFaninReduction(int nodeId,
                               int targetFaninIdx,
                               int targetFaninLit,
                               const std::vector<uint64_t> &nodeCareOffWords,
                               bool &candidateNodesReady,
                               FaninReductionPlan &plan);

    // Step 4: fanin removal and cleanup ------------------------------------

    // Remove one fanin edge and clean newly dead logic.
    bool removeTargetFaninAndCleanup(int nodeId, int targetFaninIdx);

    // Replace one node with a constant literal.
    int reduceNodeToConstant(int targetFaninNodeId, int constLit);

    // Add selected wires and remove the old edge.
    bool commitFaninReduction(int nodeId, const FaninReductionPlan &plan);

    // Overall workflow -----------------------------------------------------

    // Repeatedly reduce eligible original target fanins x of one node f.
    bool optimizeNode(int nodeId);

    std::vector<bool> candidateNodes;
    std::vector<int> tfoNodesScratch;
    std::vector<unsigned char> tfoMarksScratch;
    int tfoRootScratch = -1;
};

#endif
