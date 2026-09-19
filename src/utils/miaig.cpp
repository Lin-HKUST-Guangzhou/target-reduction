/*!
    \file miaig.cpp
    \brief Implementation of multi-input AIG
*/

#include "miaig.h"

#include "../aig_utils.h"
#include "../aig_manager.h"

#include <algorithm>
#include <cstddef>

bool Miaig::loadAigToMiaig(const AIGMan &aigman, int maxNumFanins)
{
    if (maxNumFanins < 2 || aigman.nObjs <= 0 || aigman.nPIs < 0 || aigman.nPOs < 0) {
        clear();
        return aig_utils::reportFailure<bool>(__func__, "invalid maxNumFanins or AIG dimensions");
    }

    nObjs = aigman.nObjs;
    nPIs = aigman.nPIs;
    nPOs = aigman.nPOs;
    nNodes = aigman.nNodes;
    this->maxNumFanins = maxNumFanins;

    fis.assign(std::size_t(nObjs) * maxNumFanins, 1);
    faninCounts.assign(nObjs, 0);
    pos.assign(aigman.pOuts, aigman.pOuts + nPOs);
    numFos.assign(aigman.pNumFanouts, aigman.pNumFanouts + nObjs);
    levels.assign(nObjs, 0);

    for (int nodeId = nPIs + 1; nodeId < nObjs; ++nodeId) {
        fis[std::size_t(nodeId) * maxNumFanins] = aigman.pFanin0[nodeId];
        fis[std::size_t(nodeId) * maxNumFanins + 1] = aigman.pFanin1[nodeId];
        faninCounts[std::size_t(nodeId)] = 2;
    }

    hostReady = 1;
    return updateLevel();
}

bool Miaig::setMiaig(const Miaig &input)
{
    if (&input == this) {
        return true;
    }
    if (!input.hostReady) {
        clear();
        return aig_utils::reportFailure<bool>(__func__, "input MIAIG host data is not ready");
    }

    *this = input;
    return true;
}

bool Miaig::addFanin(int targetNodeId, int srcNodeId, bool inv)
{
    if (!hostReady || targetNodeId <= nPIs || targetNodeId >= nObjs ||
        srcNodeId < 0 || srcNodeId >= nObjs) {
        return aig_utils::reportFailure<bool>(__func__, "invalid state, targetNodeId, or srcNodeId");
    }

    int freeSlot = -1;
    for (int faninIdx = 0; faninIdx < maxNumFanins; ++faninIdx) {
        const int lit = fis[std::size_t(targetNodeId) * maxNumFanins + faninIdx];
        if (aig_utils::getNodeId(lit) == 0 && aig_utils::getNodeInv(lit) == 1) {
            freeSlot = faninIdx;
            break;
        }
    }
    if (freeSlot < 0) {
        return aig_utils::reportFailure<bool>(__func__, "no free fanin slot available");
    }

    const int lit = (srcNodeId << 1) | (inv ? 1 : 0);
    fis[std::size_t(targetNodeId) * maxNumFanins + freeSlot] = lit;
    ++faninCounts[std::size_t(targetNodeId)];
    if (srcNodeId != 0) {
        ++numFos[srcNodeId];
    }

    return updateLevel();
}

bool Miaig::setConstant(int nodeId, int value)
{
    if (!hostReady || nodeId <= nPIs || nodeId >= nObjs || (value != 0 && value != 1)) {
        return aig_utils::reportFailure<bool>(__func__, "invalid state, nodeId, or constant value");
    }

    for (int faninIdx = 0; faninIdx < maxNumFanins; ++faninIdx) {
        const std::size_t offset = std::size_t(nodeId) * maxNumFanins + faninIdx;
        const int oldLit = fis[offset];
        const int oldNodeId = aig_utils::getNodeId(oldLit);
        if (oldNodeId > 0 && oldNodeId < nObjs && numFos[oldNodeId] > 0) {
            --numFos[oldNodeId];
        }
        fis[offset] = 1;
    }

    fis[std::size_t(nodeId) * maxNumFanins] = value;
    faninCounts[std::size_t(nodeId)] = 1;
    return updateLevel();
}

int Miaig::countDecomposedAnds() const
{
    if (!hostReady || nObjs <= 0 || nPIs < 0 || nPOs < 0 || maxNumFanins < 2 ||
        fis.size() != std::size_t(nObjs) * maxNumFanins ||
        pos.size() != std::size_t(nPOs) ||
        numFos.size() != std::size_t(nObjs) ||
        levels.size() != std::size_t(nObjs) ||
        levelNodes.size() != std::size_t(nObjs) || levelOffsets.empty()) {
        return aig_utils::reportFailure<int>(__func__, "invalid MIAIG state for counting decomposed ANDs");
    }

    std::vector<int> mappedLit(nObjs, -1);
    mappedLit[0] = 1;
    for (int piId = 1; piId <= nPIs; ++piId) {
        mappedLit[piId] = piId << 1;
    }

    int andCount = 0;
    const int basePiCount = nPIs;
    auto appendAnd = [&andCount, basePiCount](int lit0, int lit1) {
        if (lit1 < lit0) {
            std::swap(lit0, lit1);
        }
        const int newNodeId = basePiCount + 1 + andCount;
        ++andCount;
        return newNodeId << 1;
    };

    for (std::size_t level = 1; level + 1 < levelOffsets.size(); ++level) {
        for (int posIdx = levelOffsets[level];
             posIdx < levelOffsets[level + 1]; ++posIdx) {
            const int nodeId = levelNodes[std::size_t(posIdx)];
            if (nodeId <= nPIs || numFos[std::size_t(nodeId)] <= 0) {
                continue;
            }
            std::vector<int> activeLits;
            activeLits.reserve(maxNumFanins);
            bool forcedConst0 = false;

            for (int faninIdx = 0; faninIdx < maxNumFanins; ++faninIdx) {
                const int faninLit = fis[std::size_t(nodeId) * maxNumFanins + faninIdx];
                const int faninNodeId = aig_utils::getNodeId(faninLit);
                const int faninInv = aig_utils::getNodeInv(faninLit);

                if (faninNodeId < 0 || faninNodeId >= nObjs || mappedLit[faninNodeId] < 0) {
                    return aig_utils::reportFailure<int>(__func__, "fanin mapping is invalid during decomposition count");
                }

                int lit = mappedLit[faninNodeId] ^ faninInv;
                if (lit == 0) {
                    continue;
                }
                if (lit == 1) {
                    forcedConst0 = true;
                    break;
                }

                bool duplicate = false;
                for (int existing : activeLits) {
                    if (existing == lit) {
                        duplicate = true;
                        break;
                    }
                    if ((existing ^ 1) == lit) {
                        forcedConst0 = true;
                        duplicate = true;
                        break;
                    }
                }
                if (forcedConst0) {
                    break;
                }
                if (!duplicate) {
                    activeLits.push_back(lit);
                }
            }

            if (forcedConst0) {
                mappedLit[nodeId] = 1;
                continue;
            }
            if (activeLits.empty()) {
                mappedLit[nodeId] = 0;
                continue;
            }
            if (activeLits.size() == 1) {
                mappedLit[nodeId] = activeLits.front();
                continue;
            }

            int currentLit = appendAnd(activeLits[0], activeLits[1]);
            for (std::size_t idx = 2; idx < activeLits.size(); ++idx) {
                currentLit = appendAnd(currentLit, activeLits[idx]);
            }
            mappedLit[nodeId] = currentLit;
        }
    }

    return andCount;
}

bool Miaig::decompose(AIGMan &out) const
{
    if (!hostReady || nObjs <= 0 || nPIs < 0 || nPOs < 0 || maxNumFanins < 2 ||
        fis.size() != std::size_t(nObjs) * maxNumFanins ||
        pos.size() != std::size_t(nPOs) ||
        numFos.size() != std::size_t(nObjs) ||
        levels.size() != std::size_t(nObjs) ||
        levelNodes.size() != std::size_t(nObjs) || levelOffsets.empty()) {
        return aig_utils::reportFailure<bool>(__func__, "invalid MIAIG state for decomposition");
    }


    std::vector<int> mappedLit(nObjs, -1);
    mappedLit[0] = 1; // Miaig const0 -> AIGMan internal const0 literal.
    for (int piId = 1; piId <= nPIs; ++piId) {
        mappedLit[piId] = piId << 1;
    }

    std::vector<std::pair<int, int>> newAnds;
    const int basePiCount = nPIs;
    auto appendAnd = [&newAnds, basePiCount](int lit0, int lit1) {
        if (lit1 < lit0) {
            std::swap(lit0, lit1);
        }
        const int newNodeId = basePiCount + 1 + static_cast<int>(newAnds.size());
        newAnds.emplace_back(lit0, lit1);
        return newNodeId << 1;
    };

    for (std::size_t level = 1; level + 1 < levelOffsets.size(); ++level) {
        for (int posIdx = levelOffsets[level];
             posIdx < levelOffsets[level + 1]; ++posIdx) {
            const int nodeId = levelNodes[std::size_t(posIdx)];
            if (nodeId <= nPIs || numFos[std::size_t(nodeId)] <= 0) {
                continue;
            }
            std::vector<int> activeLits;
            activeLits.reserve(maxNumFanins);
            bool forcedConst0 = false;

            for (int faninIdx = 0; faninIdx < maxNumFanins; ++faninIdx) {
                const int faninLit = fis[std::size_t(nodeId) * maxNumFanins + faninIdx];
                const int faninNodeId = aig_utils::getNodeId(faninLit);
                const int faninInv = aig_utils::getNodeInv(faninLit);

                if (faninNodeId < 0 || faninNodeId >= nObjs || mappedLit[faninNodeId] < 0) {
                    return aig_utils::reportFailure<bool>(__func__, "fanin mapping is invalid during decomposition");
                }

                int lit = mappedLit[faninNodeId] ^ faninInv;
                if (lit == 0) {
                    continue; // const1 on an AND input is neutral.
                }
                if (lit == 1) {
                    forcedConst0 = true;
                    break;
                }

                bool duplicate = false;
                for (int existing : activeLits) {
                    if (existing == lit) {
                        duplicate = true;
                        break;
                    }
                    if ((existing ^ 1) == lit) {
                        forcedConst0 = true;
                        duplicate = true;
                        break;
                    }
                }
                if (forcedConst0) {
                    break;
                }
                if (!duplicate) {
                    activeLits.push_back(lit);
                }
            }

            if (forcedConst0) {
                mappedLit[nodeId] = 1;
                continue;
            }
            if (activeLits.empty()) {
                mappedLit[nodeId] = 0;
                continue;
            }
            if (activeLits.size() == 1) {
                mappedLit[nodeId] = activeLits.front();
                continue;
            }

            int currentLit = appendAnd(activeLits[0], activeLits[1]);
            for (std::size_t idx = 2; idx < activeLits.size(); ++idx) {
                currentLit = appendAnd(currentLit, activeLits[idx]);
            }
            mappedLit[nodeId] = currentLit;
        }
    }

    out.clearHost();
    out.nPIs = nPIs;
    out.nPOs = nPOs;
    out.nNodes = static_cast<int>(newAnds.size());
    out.nObjs = out.nPIs + out.nNodes + 1;
    out.allocHost();
    std::fill(out.pFanin0, out.pFanin0 + out.nObjs, -1);
    std::fill(out.pFanin1, out.pFanin1 + out.nObjs, -1);
    std::fill(out.pOuts, out.pOuts + out.nPOs, 0);
    std::fill(out.pNumFanouts, out.pNumFanouts + out.nObjs, 0);

    for (std::size_t idx = 0; idx < newAnds.size(); ++idx) {
        const int nodeId = nPIs + 1 + static_cast<int>(idx);
        out.pFanin0[nodeId] = newAnds[idx].first;
        out.pFanin1[nodeId] = newAnds[idx].second;
        ++out.pNumFanouts[aig_utils::getNodeId(newAnds[idx].first)];
        ++out.pNumFanouts[aig_utils::getNodeId(newAnds[idx].second)];
    }

    for (int poIdx = 0; poIdx < nPOs; ++poIdx) {
        const int poLit = pos[poIdx];
        const int poNodeId = aig_utils::getNodeId(poLit);
        const int poInv = aig_utils::getNodeInv(poLit);
        if (poNodeId < 0 || poNodeId >= nObjs || mappedLit[poNodeId] < 0) {
            out.clearHost();
            return aig_utils::reportFailure<bool>(__func__, "PO mapping is invalid during decomposition");
        }
        out.pOuts[poIdx] = mappedLit[poNodeId] ^ poInv;
        ++out.pNumFanouts[aig_utils::getNodeId(out.pOuts[poIdx])];
    }

    out.updateLevel(out.pLevel, out.pFanin0, out.pFanin1, out.nObjs, out.nPIs);
    out.setAigCreated(1);
    return true;
}

bool Miaig::hasHostData() const
{
    return hostReady != 0;
}

void Miaig::clear()
{
    nObjs = 0;
    nPIs = 0;
    nPOs = 0;
    nNodes = 0;
    maxNumFanins = 0;

    fis.clear();
    faninCounts.clear();
    pos.clear();
    numFos.clear();
    levels.clear();
    levelNodes.clear();
    levelOffsets.clear();

    hostReady = 0;
}

bool Miaig::updateLevel()
{
    if (!hostReady || nObjs <= 0 || maxNumFanins <= 0 ||
        fis.size() != std::size_t(nObjs) * maxNumFanins ||
        faninCounts.size() != std::size_t(nObjs) ||
        pos.size() != std::size_t(nPOs) ||
        numFos.size() != std::size_t(nObjs)) {
        return aig_utils::reportFailure<bool>(__func__, "invalid MIAIG state for level update");
    }

    levels.assign(nObjs, 0);

    std::vector<int> remainingFos = numFos;
    std::vector<int> reverseOrder;
    reverseOrder.reserve(std::size_t(nObjs > 0 ? nObjs - 1 : 0));
    std::vector<int> queue;
    queue.reserve(std::size_t(nObjs > 0 ? nObjs - 1 : 0));

    // Start from PO drivers and consume fanouts backwards until all reachable
    // dependencies are processed. This avoids assuming node ids are topological.
    for (int poIdx = 0; poIdx < nPOs; ++poIdx) {
        const int poNodeId = aig_utils::getNodeId(pos[poIdx]);
        if (poNodeId <= 0 || poNodeId >= nObjs) {
            continue;
        }
        if (remainingFos[poNodeId] <= 0) {
            return aig_utils::reportFailure<bool>(__func__, "PO driver fanout accounting underflow");
        }
        --remainingFos[poNodeId];
    }

    for (int nodeId = 1; nodeId < nObjs; ++nodeId) {
        if (remainingFos[nodeId] == 0) {
            queue.push_back(nodeId);
        }
    }

    for (std::size_t qHead = 0; qHead < queue.size(); ++qHead) {
        const int nodeId = queue[qHead];
        reverseOrder.push_back(nodeId);
        if (nodeId <= nPIs) {
            continue;
        }
        const int faninCount = faninCounts[std::size_t(nodeId)];
        for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
            const int lit = fis[std::size_t(nodeId) * maxNumFanins + faninIdx];
            const int faninId = aig_utils::getNodeId(lit);
            if (faninId <= 0 || faninId >= nObjs) {
                continue;
            }
            if (remainingFos[faninId] <= 0) {
                return aig_utils::reportFailure<bool>(__func__, "fanin fanout accounting underflow");
            }
            --remainingFos[faninId];
            if (remainingFos[faninId] == 0) {
                queue.push_back(faninId);
            }
        }
    }

    if (reverseOrder.size() != std::size_t(nObjs - 1)) {
        return aig_utils::reportFailure<bool>(__func__, "reverse topological traversal did not cover all nodes");
    }

    for (auto it = reverseOrder.rbegin(); it != reverseOrder.rend(); ++it) {
        const int nodeId = *it;
        if (nodeId <= nPIs) {
            continue;
        }
        int level = 0;
        const int faninCount = faninCounts[std::size_t(nodeId)];
        for (int faninIdx = 0; faninIdx < faninCount; ++faninIdx) {
            const int lit = fis[std::size_t(nodeId) * maxNumFanins + faninIdx];
            const int faninId = aig_utils::getNodeId(lit);
            if (faninId < 0 || faninId >= nObjs) {
                continue;
            }
            level = std::max(level, levels[faninId]);
        }
        levels[nodeId] = level + 1;
    }

    const int maxLevel = *std::max_element(levels.begin(), levels.end());
    levelOffsets.assign(std::size_t(maxLevel + 2), 0);
    for (int level : levels) {
        ++levelOffsets[std::size_t(level + 1)];
    }
    for (std::size_t level = 1; level < levelOffsets.size(); ++level) {
        levelOffsets[level] += levelOffsets[level - 1];
    }

    levelNodes.assign(std::size_t(nObjs), 0);
    std::vector<int> writeOffsets = levelOffsets;
    for (int nodeId = 0; nodeId < nObjs; ++nodeId) {
        const int level = levels[std::size_t(nodeId)];
        levelNodes[std::size_t(writeOffsets[std::size_t(level)]++)] = nodeId;
    }
    return true;
}
