#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace sound {
constexpr float pi = std::numbers::pi_v<float>;

float ease(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void append16(std::vector<char>& data, uint16_t value) {
    data.push_back(static_cast<char>(value));
    data.push_back(static_cast<char>(value >> 8));
}

void append32(std::vector<char>& data, uint32_t value) {
    append16(data, static_cast<uint16_t>(value));
    append16(data, static_cast<uint16_t>(value >> 16));
}

template <typename Sample>
std::vector<char> wave(int durationMs, Sample sampleAt) {
    constexpr int sampleRate = 24000;
    const int samples = sampleRate * durationMs / 1000;
    std::vector<char> data;
    data.reserve(44 + samples * 2);
    const auto tag = [&data](const char* bytes) { data.insert(data.end(), bytes, bytes + 4); };
    tag("RIFF");
    append32(data, 36 + samples * 2);
    tag("WAVE");
    tag("fmt ");
    append32(data, 16);
    append16(data, 1);
    append16(data, 1);
    append32(data, sampleRate);
    append32(data, sampleRate * 2);
    append16(data, 2);
    append16(data, 16);
    tag("data");
    append32(data, samples * 2);

    for (int i = 0; i < samples; ++i) {
        const float time = static_cast<float>(i) / sampleRate;
        const float sample = std::clamp(sampleAt(time), -1.0f, 1.0f);
        append16(data, static_cast<uint16_t>(static_cast<int16_t>(sample * 32767.0f)));
    }
    return data;
}

float sirenPulse(float age, float duration, float frequency) {
    if (age < 0 || age >= duration) {
        return 0;
    }
    const float envelope = ease(age / 0.08f) * ease((duration - age) / 0.25f);
    const float phase = 2 * pi * (frequency * age - 22 * age * age);
    const float detuned = phase + 2 * pi * 4.4f * age;
    const float beating = 0.80f + 0.20f * std::cos(2 * pi * 12 * age);
    return envelope * beating * (0.24f * std::sin(phase) + 0.19f * std::sin(detuned) +
                                  0.14f * std::sin(2 * phase) + 0.07f * std::sin(2.38f * phase));
}

std::vector<char> warningSound() {
    return wave(1920, [](float t) {
        const float pressure = ease(t / 0.035f) * ease((1.90f - t) / 0.38f);
        const float rumble = pressure * (0.15f * std::sin(2 * pi * 48 * t) +
                                         0.07f * std::sin(2 * pi * 77 * t));
        return rumble + sirenPulse(t, 0.78f, 139) + sirenPulse(t - 0.70f, 1.18f, 121);
    });
}

std::vector<char> countdownSound() {
    return wave(410, [](float t) {
        const float attack = ease(t / 0.006f);
        const float body = attack * std::exp(-9.5f * t) * ease((0.40f - t) / 0.13f);
        const float strike = attack * std::exp(-37 * t);
        const float phase = 2 * pi * (99 * t - 55 * t * t);
        const float metal = std::sin(2 * pi * 163 * t) + 0.48f * std::sin(2 * pi * 271 * t);
        return body * (0.56f * std::sin(phase) + 0.13f * std::sin(2 * phase)) + 0.15f * strike * metal;
    });
}

}  // namespace sound
