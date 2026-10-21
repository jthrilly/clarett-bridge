// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace Clarett::Audio {
constexpr uint32_t Rate = 48000, Descriptors = 256, FragmentFrames = 16;
constexpr uint32_t Frames = Descriptors * FragmentFrames;
constexpr uint32_t TxChannels = 4, RxChannels = 14;
constexpr size_t TableBytes = Descriptors * 8;
constexpr size_t TxSlot = 256, RxSlot = 1024;
constexpr size_t TxSamples = TableBytes;
constexpr size_t RxTable = TableBytes + Descriptors * TxSlot;
constexpr size_t RxSamples = (RxTable + TableBytes + 4095) & ~size_t(4095);
constexpr size_t Bytes = (RxSamples + Descriptors * RxSlot + 16383) & ~size_t(16383);
// Restore the v6 hardware-tested margin while diagnosing the low-latency
// regression. Core Audio's selectable client buffer is a separate setting.
constexpr uint32_t Safety = 512;
constexpr uint32_t Period = 256; // hardware period, distinct from the HAL ring size
inline uint64_t timestampFrame(uint64_t frames) { return frames / Frames * Frames; }
inline size_t sampleOffset(bool input, uint64_t frame, uint32_t channel) {
    const auto pos = uint32_t(frame % Frames);
    return (input ? RxSamples : TxSamples) + (pos / FragmentFrames) * (input ? RxSlot : TxSlot)
        + ((pos % FragmentFrames) * (input ? RxChannels : TxChannels) + channel) * 4;
}
inline void store64(volatile uint8_t *p, uint64_t word) {
    for (unsigned j = 0; j < 8; ++j) p[j] = uint8_t(word >> (8 * j));
}
inline bool build(volatile uint8_t *p, size_t size, uint64_t address) {
    if (!p || size < Bytes || (address & 16383) || address > UINT32_MAX - (Bytes - 1)) return false;
    for (size_t i = 0; i < Bytes; ++i) p[i] = 0;
    for (uint32_t i = 0; i < Descriptors; ++i) {
        const uint64_t wrap = i == Descriptors - 1 ? 1 : 0;
        store64(p + i * 8, address + TxSamples + i * TxSlot + wrap);
        store64(p + RxTable + i * 8, address + RxSamples + i * RxSlot + wrap
                + ((i + 1) % (Period / FragmentFrames) == 0 ? 2 : 0));
    }
    return true;
}
inline bool counterDelta(uint32_t cause, uint8_t previous, uint32_t &frames) {
    if (cause == UINT32_MAX || !(cause & 0x80000000) || (cause & ~0xc00000ffu)) return false;
    const uint32_t delta = (cause - previous) & 255;
    if (!delta) return false; // ambiguous full wrap: do not invent elapsed frames
    frames = delta * FragmentFrames;
    return true;
}
} // namespace Clarett::Audio
