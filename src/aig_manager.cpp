#include "aig_manager.h"

#include "target_reduction/target_reduction.h"
#include "utils/strash.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using aig_utils::invertConstTrueFalse;

int getFileSize(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::printf("read: cannot open %s\n", path);
        return 0;
    }
    std::fseek(file, 0, SEEK_END);
    const int size = std::ftell(file);
    std::fclose(file);
    return size;
}

unsigned decodeAigerDelta(char **position)
{
    unsigned value = 0;
    unsigned index = 0;
    unsigned char byte;
    while ((byte = *(*position)++) & 0x80) {
        value |= (byte & 0x7f) << (7 * index++);
    }
    return value | (byte << (7 * index));
}

void encodeAigerDelta(char *buffer, int &cursor, unsigned value)
{
    while (value & ~0x7f) {
        buffer[cursor++] = static_cast<char>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    buffer[cursor++] = static_cast<char>(value);
}

} // namespace

AIGMan::AIGMan() = default;

AIGMan::~AIGMan()
{
    clearHost();
}

void AIGMan::setAigCreated(bool created)
{
    aigCreated = created;
}

int AIGMan::readFile(const char *path)
{
    const int fileSize = getFileSize(path);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr || fileSize <= 0) {
        if (file != nullptr) {
            std::fclose(file);
        }
        return 0;
    }

    char *contents = static_cast<char *>(std::malloc(std::size_t(fileSize)));
    if (contents == nullptr) {
        std::fclose(file);
        return 0;
    }
    const std::size_t bytesRead = std::fread(contents, 1, std::size_t(fileSize), file);
    std::fclose(file);
    if (bytesRead != std::size_t(fileSize)) {
        std::free(contents);
        return 0;
    }

    const int status = readFromMemory(contents, fileSize);
    std::free(contents);
    aigCreated = status != 0;
    if (!aigCreated) {
        return status;
    }

    moduleName = path;
    modulePath = path;
    updateLevel(pLevel, pFanin0, pFanin1, nObjs, nPIs);
    printStats();
    return status;
}

int AIGMan::readFromMemory(char *contents, int fileSize)
{
    (void)fileSize;
    clearHost();

    if (std::strncmp(contents, "aig", 3) != 0 || contents[3] != ' ') {
        std::printf("read: only binary combinational AIGER is supported.\n");
        return 0;
    }

    char *cursor = contents;
    while (*cursor != ' ') ++cursor;
    const int total = std::atoi(++cursor);
    while (*cursor != ' ') ++cursor;
    const int inputs = std::atoi(++cursor);
    while (*cursor != ' ') ++cursor;
    const int latches = std::atoi(++cursor);
    while (*cursor != ' ') ++cursor;
    const int outputs = std::atoi(++cursor);
    while (*cursor != ' ') ++cursor;
    const int ands = std::atoi(++cursor);
    while (*cursor != ' ' && *cursor != '\n') ++cursor;

    if (latches != 0 || *cursor != '\n' || total != inputs + ands) {
        std::printf("read: unsupported or inconsistent AIGER header.\n");
        return 0;
    }
    ++cursor;

    nObjs = total + 1;
    nPIs = inputs;
    nPOs = outputs;
    nNodes = ands;
    allocHost();
    std::memset(pFanin0, -1, std::size_t(nObjs) * sizeof(int));
    std::memset(pFanin1, -1, std::size_t(nObjs) * sizeof(int));

    char *drivers = cursor;
    for (int output = 0; output < nPOs;) {
        if (*cursor++ == '\n') {
            ++output;
        }
    }

    for (int index = 0; index < nNodes; ++index) {
        const std::size_t nodeId = std::size_t(index + nPIs + 1);
        const unsigned lhs = unsigned(nodeId << 1);
        const unsigned rhs1 = lhs - decodeAigerDelta(&cursor);
        const unsigned rhs0 = rhs1 - decodeAigerDelta(&cursor);
        assert(rhs0 <= rhs1);

        pFanin0[nodeId] = invertConstTrueFalse(rhs0);
        pFanin1[nodeId] = invertConstTrueFalse(rhs1);
        ++pNumFanouts[rhs0 >> 1];
        ++pNumFanouts[rhs1 >> 1];
    }

    cursor = drivers;
    for (int output = 0; output < nPOs; ++output) {
        pOuts[output] = invertConstTrueFalse(std::atoi(cursor));
        ++pNumFanouts[std::size_t(pOuts[output] >> 1)];
        while (*cursor++ != '\n') {}
    }
    return 1;
}

void AIGMan::saveFile(const char *path)
{
    if (!aigCreated) {
        std::printf("write: no AIG is loaded.\n");
        return;
    }

    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::printf("write: cannot open %s\n", path);
        return;
    }

    std::fprintf(file, "aig %d %d 0 %d %d\n", nObjs - 1, nPIs, nPOs, nNodes);
    for (int output = 0; output < nPOs; ++output) {
        std::fprintf(file, "%d\n", invertConstTrueFalse(pOuts[output]));
    }

    std::vector<char> buffer(std::size_t(nNodes) * 30);
    int cursor = 0;
    for (int nodeId = nPIs + 1; nodeId < nObjs; ++nodeId) {
        const int rhs0 = invertConstTrueFalse(pFanin0[nodeId]);
        const int rhs1 = invertConstTrueFalse(pFanin1[nodeId]);
        assert(2 * nodeId - rhs1 >= 0);
        assert(rhs1 - rhs0 >= 0);
        encodeAigerDelta(buffer.data(), cursor, unsigned(2 * nodeId - rhs1));
        encodeAigerDelta(buffer.data(), cursor, unsigned(rhs1 - rhs0));
    }
    std::fwrite(buffer.data(), 1, std::size_t(cursor), file);
    std::fprintf(file, "c\n");
    std::fclose(file);
    std::printf("Output AIG file saved at path: %s\n", path);
}

void AIGMan::allocHost()
{
    pFanin0 = static_cast<int *>(std::malloc(std::size_t(nObjs) * sizeof(int)));
    pFanin1 = static_cast<int *>(std::malloc(std::size_t(nObjs) * sizeof(int)));
    pOuts = static_cast<int *>(std::malloc(std::size_t(nPOs) * sizeof(int)));
    pNumFanouts = static_cast<int *>(std::calloc(std::size_t(nObjs), sizeof(int)));
    pLevel = static_cast<int *>(std::malloc(std::size_t(nObjs) * sizeof(int)));
}

void AIGMan::clearHost()
{
    std::free(pFanin0);
    std::free(pFanin1);
    std::free(pOuts);
    std::free(pNumFanouts);
    std::free(pLevel);
    pFanin0 = nullptr;
    pFanin1 = nullptr;
    pOuts = nullptr;
    pNumFanouts = nullptr;
    pLevel = nullptr;
    nObjs = nPIs = nPOs = nNodes = nLevels = 0;
    aigCreated = false;
}

void AIGMan::printStats() const
{
    std::printf("AIG: %d nodes | %d PIs | %d POs | %d levels\n",
                nNodes, nPIs, nPOs, nLevels);
}

void AIGMan::updateLevel(int *levels, int *fanin0, int *fanin1,
                         int numObjects, int numPIs)
{
    int maxLevel = 0;
    for (int nodeId = 0; nodeId <= numPIs; ++nodeId) {
        levels[nodeId] = 0;
    }
    for (int nodeId = numPIs + 1; nodeId < numObjects; ++nodeId) {
        const std::size_t id0 = std::size_t(fanin0[nodeId] >> 1);
        const std::size_t id1 = std::size_t(fanin1[nodeId] >> 1);
        assert(id0 < std::size_t(nodeId) && id1 < std::size_t(nodeId));
        levels[nodeId] = 1 + std::max(levels[id0], levels[id1]);
        maxLevel = std::max(maxLevel, levels[nodeId]);
    }
    nLevels = maxLevel;
}

void AIGMan::strash()
{
    if (!aigCreated) {
        std::printf("strash: no AIG is loaded.\n");
        return;
    }
    if (!strash::run(*this)) {
        std::printf("strash: failed.\n");
    }
}

void AIGMan::targetReduction(int rounds, int earlyStop, int timeLimit,
                          int maxWiresPerFanin, int maxNumFanins,
                          int maxCandidateWires, bool preserveLevel,
                          int verbose)
{
    const int originalSize = nNodes;
    const int originalLevel = nLevels;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsedSeconds = [&]() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start).count();
    };
    const auto timeExceeded = [&]() {
        return timeLimit > 0 && elapsedSeconds() > timeLimit;
    };

    TargetReductionMan manager;
    manager.params.maxWiresPerFanin = std::max(1, maxWiresPerFanin);
    manager.params.maxCandidateWires = std::max(0, maxCandidateWires);
    manager.params.preserveLevel = preserveLevel;
    manager.params.verbose = std::max(0, verbose);
    if (!manager.loadAigToMiaig(*this, maxNumFanins)) {
        std::printf("target-reduction: failed to load AIG as MIAIG.\n");
        return;
    }

    int completedRounds = 0;
    int nonImprovingRounds = 0;
    while (rounds <= 0 || completedRounds < rounds) {
        if (timeExceeded()) {
            std::printf("target-reduction: time limit reached before round %d.\n",
                        completedRounds + 1);
            break;
        }

        const Miaig previousMiaig = manager.miaig;
        const int roundOriginalNodes = nNodes;
        const int roundOriginalLevel = nLevels;
        const auto restorePreviousRound = [&]() {
            if (!previousMiaig.decompose(*this)) {
                std::printf("target-reduction: failed to restore previous completed round.\n");
                return false;
            }
            setAigCreated(true);
            strash();
            if (!manager.loadAigToMiaig(*this, maxNumFanins)) {
                std::printf("target-reduction: failed to reload previous completed round.\n");
                return false;
            }
            return true;
        };

        if (manager.run() < 0) {
            std::printf("target-reduction: optimization failed; keeping the last completed round.\n");
            return;
        }
        if (timeExceeded()) {
            manager.setMiaig(previousMiaig);
            std::printf("target-reduction: round %d exceeded time limit and was discarded.\n",
                        completedRounds + 1);
            break;
        }

        if (!manager.miaig.decompose(*this)) {
            std::printf("target-reduction: failed to decompose MIAIG back to AIG.\n");
            restorePreviousRound();
            return;
        }
        setAigCreated(true);
        if (timeExceeded()) {
            restorePreviousRound();
            std::printf("target-reduction: round %d exceeded time limit and was discarded.\n",
                        completedRounds + 1);
            break;
        }

        strash();
        if (timeExceeded()) {
            restorePreviousRound();
            std::printf("target-reduction: round %d exceeded time limit and was discarded.\n",
                        completedRounds + 1);
            break;
        }
        if (!manager.loadAigToMiaig(*this, maxNumFanins)) {
            std::printf("target-reduction: failed to reload optimized AIG.\n");
            restorePreviousRound();
            return;
        }
        if (timeExceeded()) {
            restorePreviousRound();
            std::printf("target-reduction: round %d exceeded time limit and was discarded.\n",
                        completedRounds + 1);
            break;
        }

        std::printf("Iteration %7d(%d) : %5d (AND2 = %5d Level = %3d) -> "
                    "%5d (AND2 = %5d Level = %3d) Elapsed time = %10.2f sec\n",
                    completedRounds + 1, completedRounds + 1,
                    roundOriginalNodes, roundOriginalNodes, roundOriginalLevel,
                    nNodes, nNodes, nLevels, elapsedSeconds());
        ++completedRounds;

        if (nNodes < roundOriginalNodes) {
            nonImprovingRounds = 0;
        } else {
            ++nonImprovingRounds;
        }
        if (earlyStop > 0 && nonImprovingRounds >= earlyStop) {
            std::printf("target-reduction: early stop after %d consecutive non-improving rounds.\n",
                        nonImprovingRounds);
            break;
        }
    }

    if (!manager.equivalenceCheck()) {
        std::printf("target-reduction: internal equivalence check failed.\n");
        return;
    }
    std::printf("target-reduction: nPIs: %d | nPOs: %d | size: %d | level: %d | "
                "opt. size: %d | opt. level: %d | rounds: %d | runtime: %.4f seconds\n",
                nPIs, nPOs, originalSize, originalLevel, nNodes, nLevels,
                completedRounds, elapsedSeconds());
}
