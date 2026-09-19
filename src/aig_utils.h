#ifndef TARGET_REDUCTION_AIG_UTILS_H
#define TARGET_REDUCTION_AIG_UTILS_H

#include <cstdio>
#include <type_traits>

namespace aig_utils {

inline int getNodeId(int lit) { return lit >> 1; }
inline int getNodeInv(int lit) { return lit & 1; }

// AIGER uses literal 0 for false; the internal representation uses 0 for true.
inline unsigned invertConstTrueFalse(unsigned lit)
{
    return lit < 2 ? 1 - lit : lit;
}

template<typename T>
inline T reportFailure(const char *func, const char *detail)
{
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int>,
                  "reportFailure only supports bool and int return types");
    std::fprintf(stderr, "AIG error: %s: %s\n", func, detail);
    if constexpr (std::is_same_v<T, bool>) {
        return false;
    } else {
        return -1;
    }
}

} // namespace aig_utils

#endif // TARGET_REDUCTION_AIG_UTILS_H
