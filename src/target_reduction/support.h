/*!
    \file support.h
    \brief Shared support for the Target-Reduction implementation.
*/

#ifndef TARGET_REDUCTION_SUPPORT_H
#define TARGET_REDUCTION_SUPPORT_H

#pragma once

#include <cstdint>
#include <vector>

#include "../aig_utils.h"
#include "../utils/packed_truthtable.h"

namespace target_reduction {

using aig_utils::getNodeId;
using aig_utils::getNodeInv;
using aig_utils::reportFailure;
using packed_tt::readLitWord;
using packed_tt::wordMask;

// Query packed bit vectors.
int countSetBits(const std::vector<uint64_t> &bits);
bool hasAnyBit(const std::vector<uint64_t> &bits);

} // namespace target_reduction

#endif // TARGET_REDUCTION_SUPPORT_H
