#include "sound.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>

namespace {
uint16_t read16(const std::vector<char>& wave, size_t offset) {
    return static_cast<uint16_t>(static_cast<unsigned char>(wave[offset]) |
                                 (static_cast<unsigned char>(wave[offset + 1]) << 8));
}

uint32_t read32(const std::vector<char>& wave, size_t offset) {
    return read16(wave, offset) | (static_cast<uint32_t>(read16(wave, offset + 2)) << 16);
}

bool valid(const std::vector<char>& wave, unsigned milliseconds) {
    constexpr unsigned rate = 24000;
    constexpr unsigned header = 44;
    const unsigned payload = rate * milliseconds / 1000 * 2;
    if (wave.size() != header + payload ||
        std::string_view(wave.data(), 4) != "RIFF" ||
        std::string_view(wave.data() + 8, 8) != "WAVEfmt " ||
        std::string_view(wave.data() + 36, 4) != "data" ||
        read32(wave, 4) != wave.size() - 8 ||
        read16(wave, 20) != 1 || read16(wave, 22) != 1 ||
        read32(wave, 24) != rate || read16(wave, 34) != 16 ||
        read32(wave, 40) != payload || read16(wave, header) || read16(wave, wave.size() - 2)) {
        return false;
    }

    int peak = 0;
    for (size_t offset = header; offset < wave.size(); offset += 2) {
        peak = std::max(peak, std::abs(static_cast<int>(std::bit_cast<int16_t>(read16(wave, offset)))));
    }
    return peak > 6000 && peak < 31000;
}
}  // namespace

int main() {
    return valid(sound::warningSound(), 1920) && valid(sound::countdownSound(), 410) ? 0 : 1;
}
