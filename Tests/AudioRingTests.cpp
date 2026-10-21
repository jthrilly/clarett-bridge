// SPDX-License-Identifier: GPL-2.0-only
#include "AudioRing.hpp"
#include "Protocol.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace Clarett;
namespace A = Clarett::Audio;
static uint64_t load64(const uint8_t *p) {
    uint64_t word = 0;
    for (unsigned i = 0; i < 8; ++i) word |= uint64_t(p[i]) << (8*i);
    return word;
}
int main() {
    std::vector<uint8_t> memory(A::Bytes + 16, 0xa5);
    constexpr uint64_t base = 0x100000;
    assert(!A::build(memory.data(), A::Bytes - 1, base));
    assert(!A::build(memory.data(), A::Bytes, base + 1));
    assert(!A::build(memory.data(), A::Bytes, 0xffffc000));
    assert(A::build(memory.data(), A::Bytes, base));
    for (uint32_t i = 0; i < A::Descriptors; ++i) {
        auto tx = load64(memory.data() + i * 8);
        auto rx = load64(memory.data() + A::RxTable + i * 8);
        assert((tx & ~3ull) == base + A::TxSamples + i * A::TxSlot);
        assert((rx & ~3ull) == base + A::RxSamples + i * A::RxSlot);
        assert((tx & 1) == (i == A::Descriptors - 1));
        assert((rx & 1) == (i == A::Descriptors - 1));
        assert(bool(rx & 2) == ((i + 1) % 16 == 0));
        assert((tx & 4095) + A::FragmentFrames * A::TxChannels * 4 - (tx & 3) <= 4096);
        assert((rx & 4095) + A::FragmentFrames * A::RxChannels * 4 - (rx & 3) <= 4096);
    }
    for (unsigned i = 0; i < 16; ++i) assert(memory[A::Bytes+i] == 0xa5);
    // Packed HAL frames are scattered over 14-channel fragments with padding.
    for (uint64_t f = 0; f < 2 * A::Frames; ++f) {
        for (uint32_t ch = 0; ch < A::RxChannels; ++ch) {
            auto off = A::sampleOffset(true, f, ch);
            assert(off >= A::RxSamples && off + 4 <= A::Bytes);
            assert(off == A::sampleOffset(true, f + A::Frames, ch));
            assert((off & 1023) < A::FragmentFrames * A::RxChannels * 4);
        }
    }
    assert(A::timestampFrame(A::Frames - 1) == 0);
    assert(A::timestampFrame(A::Frames) == A::Frames);
    assert(A::timestampFrame(A::Frames + A::Period) == A::Frames);
    assert(A::timestampFrame(2 * A::Frames) == 2 * A::Frames);
    uint32_t frames = 0;
    assert(A::counterDelta(0x80000010, 0, frames) && frames == 256);
    assert(A::counterDelta(0xc0000010, 0xf0, frames) && frames == 512); // overrun + wrap remains valid
    assert(!A::counterDelta(0xffffffff, 0, frames));
    assert(!A::counterDelta(0x80000110, 0, frames));
    assert(!A::counterDelta(0x80000010, 0x10, frames)); // ambiguous entire wrap
    assert(!A::counterDelta(0x10, 0, frames));
    assert(!permitted(audioClock(0)) && permitted(audioClock(0), true));
    auto clock = audioClock(0); clock.payload[0] = 0x44;
    assert(!permitted(clock, true)); // rate is fixed; no arbitrary clock writes
    for (auto op : {0x6004u, 0x6002u, 0x6005u}) {
        assert(permitted(audioSync(op, 1), true));
        assert(!permitted(audioSync(op, 1)));
    }
    assert(!permitted(audioSync(0x800001, 1), true)); // no configuration writes
    puts("PASS: audio descriptor bounds, 4KB page containment, padded capture, wrapping, hardware counters and fixed clock command scope");
}
