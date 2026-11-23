#include "mouse_motion.h"

#include <cstdio>
#include <limits>

int main() {
    MouseMotion motion;
    if (motion.moved(1, 10, 5, -1) || motion.moved(1, 11, -5, 1) ||
        motion.moved(1, 12, 6, 0) || motion.moved(1, 262, 6, 0) ||
        !motion.moved(1, 263, 4, 2)) {
        std::fputs("Small bumps or separate gestures counted as deliberate movement.\n", stderr);
        return 1;
    }

    motion = {};
    if (motion.moved(1, 10, 8, 0) || motion.moved(2, 11, 8, 0) ||
        !motion.moved(2, 12, 4, 0)) {
        std::fputs("Movement from different devices was combined.\n", stderr);
        return 1;
    }

    motion = {};
    if (motion.moved(1, std::numeric_limits<std::uint32_t>::max(), -7, 0) ||
        !motion.moved(1, 1, -5, 0)) {
        std::fputs("A gesture was lost when the input timestamp wrapped.\n", stderr);
        return 1;
    }
}
