/*!
    \file packed_truthtable.h
    \brief Header of packed truthtable
*/

#ifndef __PACKED_TRUTHTABLE_H__
#define __PACKED_TRUTHTABLE_H__

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../aig_utils.h"

class Miaig;

namespace packed_tt {

inline uint64_t wordMask(int nPIs, int wordIdx)
{
    const std::size_t begin = std::size_t(wordIdx) << 6;
    const std::size_t totalBits = std::size_t(1) << nPIs;
    if (begin >= totalBits) return 0ULL;

    const std::size_t remain = totalBits - begin;
    return remain >= 64 ? ~0ULL : (uint64_t(1) << remain) - 1;
}

inline uint64_t readLitWord(const uint64_t *nodeTT, int nPIs, int nWords,
                            int lit, int wordIdx)
{
    const uint64_t mask = wordMask(nPIs, wordIdx);
    if (lit < 0) return mask;

    uint64_t word = nodeTT[std::size_t(aig_utils::getNodeId(lit)) * nWords + wordIdx];
    return aig_utils::getNodeInv(lit) ? ((~word) & mask) : word;
}

} // namespace packed_tt

class PackedTT
{
public:
    PackedTT() = default;

    bool initFromMiaig(const Miaig &miaig);

    // Simulate the complete MIAIG or only the TFO affected by one changed node.
    bool simulate(const Miaig &miaig);
    bool resimulateTfo(const Miaig &miaig, int nodeId);

    // Derive a counterfactual table by flipping one node and propagating its TFO.
    bool simulateFlippedTfo(const Miaig &miaig,
                            const PackedTT &baseTT,
                            int nodeId,
                            const std::vector<int> &tfoNodes,
                            const std::vector<unsigned char> &tfoMarks);
    
    void clear();
    bool hasHostData() const;

    int nPIs = 0;
    int nObjs = 0;
    int nPOs = 0;
    int nWords = 0;
    std::vector<uint64_t> poTT;
    std::vector<uint64_t> nodeTT;

private:
    int hostReady = 0;
};

#endif
