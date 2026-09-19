/*!
    \file packed_truthtable.cpp
    \brief Implementation of packed truthtable
*/

#include "packed_truthtable.h"
#include "miaig.h"

#include <cstddef>

namespace
{

constexpr uint64_t kTruths6[6] = {
    0xAAAAAAAAAAAAAAAAULL,
    0xCCCCCCCCCCCCCCCCULL,
    0xF0F0F0F0F0F0F0F0ULL,
    0xFF00FF00FF00FF00ULL,
    0xFFFF0000FFFF0000ULL,
    0xFFFFFFFF00000000ULL,
};

inline int numTruthWords(int nPIs)
{
    if (nPIs < 0) return 0;
    return nPIs <= 6 ? 1 : 1 << (nPIs - 6);
}

template<int FaninCount>
inline void simulateAndWordsFixed(uint64_t *dst,
                                  const uint64_t *const *src,
                                  const uint64_t *invMask,
                                  int nWords,
                                  uint64_t tailMask)
{
    for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        uint64_t word = src[0][wordIdx] ^ invMask[0];
        if (FaninCount >= 2) word &= src[1][wordIdx] ^ invMask[1];
        if (FaninCount >= 3) word &= src[2][wordIdx] ^ invMask[2];
        if (FaninCount >= 4) word &= src[3][wordIdx] ^ invMask[3];
        if (FaninCount >= 5) word &= src[4][wordIdx] ^ invMask[4];
        if (FaninCount >= 6) word &= src[5][wordIdx] ^ invMask[5];
        if (FaninCount >= 7) word &= src[6][wordIdx] ^ invMask[6];
        if (FaninCount >= 8) word &= src[7][wordIdx] ^ invMask[7];
        dst[wordIdx] = word;
    }
    dst[nWords - 1] &= tailMask;
}

template<typename SourceResolver>
inline void simulateAndNodeWords(uint64_t *dst,
                                 int nWords,
                                 uint64_t tailMask,
                                 const int *lits,
                                 int faninCount,
                                 SourceResolver resolveSource)
{
    constexpr int kSpecializedMaxFanins = 8;
    if (faninCount <= 0) {
        for (int wordIdx = 0; wordIdx + 1 < nWords; ++wordIdx) {
            dst[wordIdx] = ~0ULL;
        }
        dst[nWords - 1] = tailMask;
        return;
    }

    if (faninCount > kSpecializedMaxFanins) {
        for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
            uint64_t word = ~0ULL;
            for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
                const int lit = lits[faninIdx];
                word &= resolveSource(lit)[wordIdx] ^
                        (aig_utils::getNodeInv(lit) ? ~0ULL : 0ULL);
            }
            dst[wordIdx] = word;
        }
        dst[nWords - 1] &= tailMask;
        return;
    }

    const uint64_t *src[kSpecializedMaxFanins];
    uint64_t invMask[kSpecializedMaxFanins];
    for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
        const int lit = lits[faninIdx];
        src[faninIdx] = resolveSource(lit);
        invMask[faninIdx] = aig_utils::getNodeInv(lit) ? ~0ULL : 0ULL;
    }

    switch (faninCount) {
    case 1: simulateAndWordsFixed<1>(dst, src, invMask, nWords, tailMask); return;
    case 2: simulateAndWordsFixed<2>(dst, src, invMask, nWords, tailMask); return;
    case 3: simulateAndWordsFixed<3>(dst, src, invMask, nWords, tailMask); return;
    case 4: simulateAndWordsFixed<4>(dst, src, invMask, nWords, tailMask); return;
    case 5: simulateAndWordsFixed<5>(dst, src, invMask, nWords, tailMask); return;
    case 6: simulateAndWordsFixed<6>(dst, src, invMask, nWords, tailMask); return;
    case 7: simulateAndWordsFixed<7>(dst, src, invMask, nWords, tailMask); return;
    case 8: simulateAndWordsFixed<8>(dst, src, invMask, nWords, tailMask); return;
    default: return;
    }
}

inline void initPiTruthWords(uint64_t *dst, int nPIs, int piIdx, int nWords)
{
    if (piIdx < 6) {
        const uint64_t pattern = kTruths6[piIdx];
        for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
            dst[wordIdx] = pattern;
        }
    } else {
        const int periodBit = 1 << (piIdx - 6);
        for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
            dst[wordIdx] = (wordIdx & periodBit) ? ~0ULL : 0ULL;
        }
    }
    dst[nWords - 1] &= packed_tt::wordMask(nPIs, nWords - 1);
}

inline void writeLiteralWords(uint64_t *dst, const uint64_t *nodeTT, int nWords, int nPIs, int lit)
{
    const uint64_t *src = nodeTT + std::size_t(aig_utils::getNodeId(lit)) * nWords;
    const uint64_t invMask = aig_utils::getNodeInv(lit) ? ~0ULL : 0ULL;
    for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        dst[wordIdx] = src[wordIdx] ^ invMask;
    }
    dst[nWords - 1] &= packed_tt::wordMask(nPIs, nWords - 1);
}

inline void simulateNodeTruth(uint64_t *nodeTT, int nPIs, int nWords, int nodeId,
                              const int *lits, int faninCount)
{
    uint64_t *dst = nodeTT + std::size_t(nodeId) * nWords;
    simulateAndNodeWords(
        dst, nWords, packed_tt::wordMask(nPIs, nWords - 1), lits, faninCount,
        [nodeTT, nWords](int lit) {
            return nodeTT + std::size_t(aig_utils::getNodeId(lit)) * nWords;
        });
}

inline const uint64_t *selectNodeWords(const PackedTT &baseTT,
                                       const PackedTT &updatedTT,
                                       const std::vector<unsigned char> &updatedMarks,
                                       int nodeId)
{
    const PackedTT &source = updatedMarks[std::size_t(nodeId)] ? updatedTT : baseTT;
    return source.nodeTT.data() + std::size_t(nodeId) * source.nWords;
}

inline void simulateMixedNodeTruth(const Miaig &miaig,
                                   const PackedTT &baseTT,
                                   PackedTT &updatedTT,
                                   const std::vector<unsigned char> &updatedMarks,
                                   int nodeId)
{
    uint64_t *dst = updatedTT.nodeTT.data() + std::size_t(nodeId) * updatedTT.nWords;
    const int *lits =
        miaig.fis.data() + std::size_t(nodeId) * miaig.maxNumFanins;
    simulateAndNodeWords(
        dst, updatedTT.nWords,
        packed_tt::wordMask(updatedTT.nPIs, updatedTT.nWords - 1),
        lits, miaig.faninCounts[std::size_t(nodeId)],
        [&baseTT, &updatedTT, &updatedMarks](int lit) {
            return selectNodeWords(
                baseTT, updatedTT, updatedMarks, aig_utils::getNodeId(lit));
        });
}

} // namespace

bool PackedTT::initFromMiaig(const Miaig &miaig)
{
    if (!miaig.hasHostData() || miaig.nObjs <= 0 || miaig.nPIs < 0 || miaig.nPOs < 0) {
        clear();
        return aig_utils::reportFailure<bool>(__func__, "invalid MIAIG host data or dimensions");
    }

    nPIs = miaig.nPIs;
    nObjs = miaig.nObjs;
    nPOs = miaig.nPOs;
    nWords = numTruthWords(nPIs);

    nodeTT.assign(std::size_t(nObjs) * nWords, 0ULL);
    poTT.assign(std::size_t(nPOs) * nWords, 0ULL);

    hostReady = 1;
    return true;
}

void PackedTT::clear()
{
    nPIs = 0;
    nObjs = 0;
    nPOs = 0;
    nWords = 0;
    poTT.clear();
    nodeTT.clear();

    hostReady = 0;
}

bool PackedTT::hasHostData() const
{
    return hostReady != 0;
}

bool PackedTT::simulate(const Miaig &miaig)
{
    if (!miaig.hasHostData() || miaig.nObjs <= 0 ||
        miaig.nPIs < 0 || miaig.nPOs < 0) {
        return aig_utils::reportFailure<bool>(__func__, "invalid MIAIG state");
    }
    if (int(miaig.levels.size()) != miaig.nObjs ||
        int(miaig.levelNodes.size()) != miaig.nObjs ||
        miaig.levelOffsets.empty()) {
        return aig_utils::reportFailure<bool>(__func__, "MIAIG level order is invalid");
    }

    if (nPIs != miaig.nPIs || nObjs != miaig.nObjs || nPOs != miaig.nPOs ||
        nodeTT.size() != std::size_t(miaig.nObjs) * numTruthWords(miaig.nPIs) ||
        poTT.size() != std::size_t(miaig.nPOs) * numTruthWords(miaig.nPIs)) {
        if (!initFromMiaig(miaig)) {
            return aig_utils::reportFailure<bool>(__func__, "initFromMiaig(miaig) failed");
        }
    }

    for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        nodeTT[wordIdx] = 0ULL;
    }
    for (int piIdx = 0; piIdx < nPIs; ++piIdx) {
        const int nodeId = piIdx + 1;
        initPiTruthWords(nodeTT.data() + std::size_t(nodeId) * nWords, nPIs, piIdx, nWords);
    }

    for (int level = 1; level + 1 < int(miaig.levelOffsets.size()); ++level) {
        const int begin = miaig.levelOffsets[level];
        const int end = miaig.levelOffsets[level + 1];
        for (int pos = begin; pos < end; ++pos) {
            const int nodeId = miaig.levelNodes[pos];
            if (nodeId <= miaig.nPIs) {
                continue;
            }
            simulateNodeTruth(nodeTT.data(), nPIs, nWords, nodeId,
                              miaig.fis.data() + std::size_t(nodeId) * miaig.maxNumFanins,
                              miaig.faninCounts[std::size_t(nodeId)]);
        }
    }

    for (int poIdx = 0; poIdx < nPOs; ++poIdx) {
        const int lit = miaig.pos[poIdx];
        writeLiteralWords(poTT.data() + std::size_t(poIdx) * nWords, nodeTT.data(), nWords, nPIs, lit);
    }

    hostReady = 1;
    return true;
}

bool PackedTT::resimulateTfo(const Miaig &miaig, int nodeId)
{
    if (!miaig.hasHostData() || miaig.nObjs <= 0 ||
        miaig.nPIs < 0 || miaig.nPOs < 0 ||
        nodeId < 0 || nodeId >= miaig.nObjs) {
        return aig_utils::reportFailure<bool>(__func__, "invalid MIAIG state or nodeId");
    }
    if (int(miaig.levels.size()) != miaig.nObjs ||
        int(miaig.levelNodes.size()) != miaig.nObjs ||
        miaig.levelOffsets.empty()) {
        return aig_utils::reportFailure<bool>(__func__, "MIAIG level order is invalid");
    }

    if (!hasHostData() ||
        nPIs != miaig.nPIs || nObjs != miaig.nObjs || nPOs != miaig.nPOs ||
        nodeTT.size() != std::size_t(miaig.nObjs) * numTruthWords(miaig.nPIs) ||
        poTT.size() != std::size_t(miaig.nPOs) * numTruthWords(miaig.nPIs)) {
        return simulate(miaig);
    }

    const int startLevel = miaig.levels[nodeId];
    if (startLevel < 0 || startLevel + 1 >= int(miaig.levelOffsets.size())) {
        return simulate(miaig);
    }

    std::vector<bool> tfos(miaig.nObjs, 0);
    std::vector<int> updateNodeList;
    tfos[nodeId] = true; updateNodeList.push_back(nodeId);

    // collect tfos of the target node
    for (int level = startLevel; level + 1 < int(miaig.levelOffsets.size()); ++level) {
        const int begin = miaig.levelOffsets[level];
        const int end = miaig.levelOffsets[level + 1];
        for (int i = begin; i < end; ++i) {
            const int curNodeId = miaig.levelNodes[i];
            const int faninCount = miaig.faninCounts[std::size_t(curNodeId)];
            for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
                const int lit = miaig.fis[std::size_t(curNodeId) * miaig.maxNumFanins + faninIdx];
                const int srcNodeId = aig_utils::getNodeId(lit);
                if (tfos[srcNodeId]) {
                    tfos[curNodeId] = true;
                    updateNodeList.push_back(curNodeId);
                    break;
                }
            }
        }
    }

    for (auto updateNodeId : updateNodeList) {
        if (updateNodeId <= miaig.nPIs) {
            continue;
        }
        simulateNodeTruth(nodeTT.data(), nPIs, nWords, updateNodeId,
                          miaig.fis.data() + std::size_t(updateNodeId) * miaig.maxNumFanins,
                          miaig.faninCounts[std::size_t(updateNodeId)]);
    }

    for (int poIdx = 0; poIdx < nPOs; ++poIdx) {
        const int lit = miaig.pos[poIdx];
        writeLiteralWords(poTT.data() + std::size_t(poIdx) * nWords, nodeTT.data(), nWords, nPIs, lit);
    }

    hostReady = 1;
    return true;
}

bool PackedTT::simulateFlippedTfo(
    const Miaig &miaig,
    const PackedTT &baseTT,
    int nodeId,
    const std::vector<int> &tfoNodes,
    const std::vector<unsigned char> &tfoMarks)
{
    if (!miaig.hasHostData() || !baseTT.hasHostData() ||
        nodeId < 0 || nodeId >= miaig.nObjs || tfoNodes.empty() ||
        tfoNodes.front() != nodeId ||
        tfoMarks.size() != std::size_t(miaig.nObjs) ||
        nPIs != baseTT.nPIs || nObjs != baseTT.nObjs ||
        nPOs != baseTT.nPOs || nWords != baseTT.nWords ||
        nodeTT.size() != baseTT.nodeTT.size()) {
        return aig_utils::reportFailure<bool>(
            __func__, "invalid MIAIG, truth-table, or TFO state");
    }

    const std::size_t nodeBase = std::size_t(nodeId) * nWords;
    for (int wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        const uint64_t mask = packed_tt::wordMask(nPIs, wordIdx);
        nodeTT[nodeBase + std::size_t(wordIdx)] =
            (~baseTT.nodeTT[nodeBase + std::size_t(wordIdx)]) & mask;
    }

    for (std::size_t idx = 1; idx < tfoNodes.size(); ++idx) {
        simulateMixedNodeTruth(
            miaig, baseTT, *this, tfoMarks, tfoNodes[idx]);
    }
    hostReady = 1;
    return true;
}
