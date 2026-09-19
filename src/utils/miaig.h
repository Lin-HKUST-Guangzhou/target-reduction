/*!
  \file miaig.h
  \brief Header of multi-input AIG (MIAIG).
*/

#ifndef __MIAIG_H__
#define __MIAIG_H__

#pragma once

#include <vector>

class AIGMan;

class Miaig
{
public:
    Miaig() = default;

    bool loadAigToMiaig(const AIGMan &aigman, int maxNumFanins = 8);
    bool setMiaig(const Miaig &input);
    bool addFanin(int targetNodeId, int srcNodeId, bool inv);
    bool setConstant(int nodeId, int value);
    int countDecomposedAnds() const;
    bool decompose(AIGMan &out) const;
    bool hasHostData() const;
    void clear();

    bool updateLevel();

    int nObjs = 0;              // const + nPIs + internal nodes
    int nPIs = 0;
    int nPOs = 0;
    int nNodes = 0;
    int maxNumFanins = 0;
    std::vector<int> fis;
    std::vector<int> faninCounts;
    std::vector<int> pos;
    std::vector<int> numFos;    // numFos[0] is always meaningless
    std::vector<int> levels;
    std::vector<int> levelNodes;
    std::vector<int> levelOffsets;

private:
    int hostReady = 0;
};

#endif  
