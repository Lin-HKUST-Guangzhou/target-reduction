#ifndef TARGET_REDUCTION_AIG_MANAGER_H
#define TARGET_REDUCTION_AIG_MANAGER_H

#include <string>

#include "aig_utils.h"

class AIGMan {
public:
    AIGMan();
    ~AIGMan();

    int readFile(const char *path);
    void saveFile(const char *path);
    void printStats() const;
    void strash();

    // Run Target-Reduction with round, early-stop, and time controls.
    void targetReduction(int rounds = 1, int earlyStop = 0, int timeLimit = 0,
                      int maxWiresPerFanin = 3, int maxNumFanins = 8,
                      int maxCandidateWires = 2048,
                      bool preserveLevel = true, int verbose = 0);

    void allocHost();
    void clearHost();
    void setAigCreated(bool created);
    void updateLevel(int *levels, int *fanin0, int *fanin1,
                     int numObjects, int numPIs);

    int nObjs = 0;
    int nPIs = 0;
    int nPOs = 0;
    int nNodes = 0;
    int nLevels = 0;

    int *pFanin0 = nullptr;
    int *pFanin1 = nullptr;
    int *pOuts = nullptr;
    int *pNumFanouts = nullptr;
    int *pLevel = nullptr;

    std::string moduleName = "module";
    std::string modulePath;
    std::string moduleInfo;

private:
    int readFromMemory(char *contents, int fileSize);

    bool aigCreated = false;
};

#endif
