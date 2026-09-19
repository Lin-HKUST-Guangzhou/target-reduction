/*!
    \file strash.cpp
    \brief Structural hashing for the default 2-input AIG.
*/

#include "strash.h"
#include "../aig_utils.h"

#include "../aig_manager.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

uint64_t makeKey(int lit0, int lit1)
{
    return (uint64_t(uint32_t(lit0)) << 32) | uint32_t(lit1);
}

int andConstSimplify(int lit0, int lit1)
{
    constexpr int kConst1 = 0;
    constexpr int kConst0 = 1;

    if (lit0 == kConst0 || lit1 == kConst0) {
        return kConst0;
    }
    if (lit0 == kConst1) {
        return lit1;
    }
    if (lit1 == kConst1) {
        return lit0;
    }
    if (lit0 == lit1) {
        return lit0;
    }
    if ((lit0 ^ 1) == lit1) {
        return kConst0;
    }
    return -1;
}

} // namespace

namespace strash
{

bool run(AIGMan &aigman)
{
    if (aigman.nObjs <= 0 || aigman.nPIs < 0 || aigman.nNodes < 0 ||
        aigman.pFanin0 == nullptr || aigman.pFanin1 == nullptr ||
        aigman.pOuts == nullptr || aigman.pNumFanouts == nullptr) {
        return false;
    }

    std::vector<int> oldToNewLit(std::size_t(aigman.nObjs), -1);
    oldToNewLit[0] = 0;
    for (int piId = 1; piId <= aigman.nPIs; ++piId) {
        oldToNewLit[std::size_t(piId)] = piId << 1;
    }

    std::vector<std::pair<int, int>> newFanins;
    std::unordered_map<uint64_t, int> hashTable;

    std::function<int(int)> rewriteLit = [&](int oldLit) -> int {
        const int oldId = aig_utils::getNodeId(oldLit);
        const int oldInv = aig_utils::getNodeInv(oldLit);
        if (oldId < 0 || oldId >= aigman.nObjs) {
            return oldLit;
        }
        if (oldToNewLit[std::size_t(oldId)] >= 0) {
            return oldToNewLit[std::size_t(oldId)] ^ oldInv;
        }
        if (oldId <= aigman.nPIs) {
            oldToNewLit[std::size_t(oldId)] = oldId << 1;
            return oldToNewLit[std::size_t(oldId)] ^ oldInv;
        }

        int lit0 = rewriteLit(aigman.pFanin0[oldId]);
        int lit1 = rewriteLit(aigman.pFanin1[oldId]);
        if (lit0 > lit1) {
            std::swap(lit0, lit1);
        }

        const int simplifiedLit = andConstSimplify(lit0, lit1);
        if (simplifiedLit >= 0) {
            oldToNewLit[std::size_t(oldId)] = simplifiedLit;
            return simplifiedLit ^ oldInv;
        }

        const uint64_t key = makeKey(lit0, lit1);
        auto it = hashTable.find(key);
        if (it != hashTable.end()) {
            const int reusedLit = it->second << 1;
            oldToNewLit[std::size_t(oldId)] = reusedLit;
            return reusedLit ^ oldInv;
        }

        const int newNodeId = aigman.nPIs + 1 + static_cast<int>(newFanins.size());
        newFanins.emplace_back(lit0, lit1);
        hashTable.emplace(key, newNodeId);
        const int newLit = newNodeId << 1;
        oldToNewLit[std::size_t(oldId)] = newLit;
        return newLit ^ oldInv;
    };

    std::vector<int> newOuts(std::size_t(aigman.nPOs), 0);
    for (int poIdx = 0; poIdx < aigman.nPOs; ++poIdx) {
        newOuts[std::size_t(poIdx)] = rewriteLit(aigman.pOuts[poIdx]);
    }

    const int newNNodes = static_cast<int>(newFanins.size());
    const int newNObjs = aigman.nPIs + newNNodes + 1;

    int *newFanin0 = static_cast<int *>(std::malloc(std::size_t(newNObjs) * sizeof(int)));
    int *newFanin1 = static_cast<int *>(std::malloc(std::size_t(newNObjs) * sizeof(int)));
    int *newOutArray = static_cast<int *>(std::malloc(std::size_t(aigman.nPOs) * sizeof(int)));
    int *newNumFanouts = static_cast<int *>(std::calloc(std::size_t(newNObjs), sizeof(int)));
    int *newLevel = static_cast<int *>(std::malloc(std::size_t(newNObjs) * sizeof(int)));
    if (newFanin0 == nullptr || newFanin1 == nullptr || newOutArray == nullptr ||
        newNumFanouts == nullptr || newLevel == nullptr) {
        std::free(newFanin0);
        std::free(newFanin1);
        std::free(newOutArray);
        std::free(newNumFanouts);
        std::free(newLevel);
        return false;
    }

    std::fill(newFanin0, newFanin0 + newNObjs, -1);
    std::fill(newFanin1, newFanin1 + newNObjs, -1);
    std::fill(newLevel, newLevel + newNObjs, 0);

    for (int idx = 0; idx < newNNodes; ++idx) {
        const int nodeId = aigman.nPIs + 1 + idx;
        const int lit0 = newFanins[std::size_t(idx)].first;
        const int lit1 = newFanins[std::size_t(idx)].second;
        newFanin0[nodeId] = lit0;
        newFanin1[nodeId] = lit1;
        ++newNumFanouts[aig_utils::getNodeId(lit0)];
        ++newNumFanouts[aig_utils::getNodeId(lit1)];
    }

    for (int poIdx = 0; poIdx < aigman.nPOs; ++poIdx) {
        const int lit = newOuts[std::size_t(poIdx)];
        newOutArray[poIdx] = lit;
        ++newNumFanouts[aig_utils::getNodeId(lit)];
    }

    std::free(aigman.pFanin0);
    std::free(aigman.pFanin1);
    std::free(aigman.pOuts);
    std::free(aigman.pNumFanouts);
    std::free(aigman.pLevel);

    aigman.pFanin0 = newFanin0;
    aigman.pFanin1 = newFanin1;
    aigman.pOuts = newOutArray;
    aigman.pNumFanouts = newNumFanouts;
    aigman.pLevel = newLevel;
    aigman.nNodes = newNNodes;
    aigman.nObjs = newNObjs;
    aigman.updateLevel(aigman.pLevel, aigman.pFanin0, aigman.pFanin1, aigman.nObjs, aigman.nPIs);
    return true;
}

} // namespace strash
