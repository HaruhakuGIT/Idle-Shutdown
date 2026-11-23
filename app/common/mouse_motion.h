#pragma once

#include <cmath>
#include <cstdint>

// Count displacement within a gesture. Opposite movements cancel each other out.
class MouseMotion {
public:
    bool moved(std::uintptr_t source, std::uint32_t now, double dx, double dy) {
        if (dx == 0 && dy == 0) {
            return false;
        }
        if (device != source || now - lastMotion >= 250) {
            x = y = 0;
        }
        device = source;
        lastMotion = now;
        x += dx;
        y += dy;
        return std::abs(x) + std::abs(y) >= 12;
    }

private:
    std::uintptr_t device{};
    std::uint32_t lastMotion{};
    double x{};
    double y{};
};
