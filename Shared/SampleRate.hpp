// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Clarett::Audio {
constexpr uint32_t DefaultRate = 48000;
constexpr uint32_t Rates[] = {44100, 48000, 88200, 96000};
constexpr size_t RateCount = sizeof(Rates) / sizeof(Rates[0]);
inline bool rateValid(double rate) {
    for (auto supported : Rates) if (rate == supported) return true;
    return false; // rejects fractions, NaN, infinities and unimplemented rates
}
inline uint8_t speedBand(uint32_t rate) { return rate > 48000 ? 1 : 0; }
inline uint32_t pollFrames(uint32_t safety) { return safety == 512 ? 24 : 12; }
inline uint64_t pollTicks(uint64_t ticksPerSecond, uint32_t rate, uint32_t safety) {
    return ticksPerSecond * pollFrames(safety) / rate;
}
inline uint64_t ticksForFrames(uint64_t ticksPerSecond, uint64_t frames, uint32_t rate) {
    return frames * ticksPerSecond / rate;
}
inline int32_t captureSample(uint32_t raw, uint32_t channel, uint32_t liveChannels) {
    // The hardware writes impulse residue into ADAT slots removed by S/MUX.
    return channel < liveChannels ? int32_t(raw & 0xffffff00u) : 0;
}
} // namespace Clarett::Audio
