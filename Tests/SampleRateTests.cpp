// SPDX-License-Identifier: GPL-2.0-only
#include "SampleRate.hpp"
#include "AudioRing.hpp"
#include "Protocol.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>
namespace A = Clarett::Audio;
int main() {
    constexpr uint64_t ticks = 1000000000;
    for (auto rate : {44100u, 48000u, 88200u, 96000u}) {
        assert(A::rateValid(rate));
        auto command = Clarett::audioClock(7, rate);
        assert(Clarett::load32(command.payload) == rate);
        assert(Clarett::load32(command.payload + 4) == 24);
        assert(!Clarett::permitted(command));
        assert(Clarett::permitted(command, true));
        // An otherwise valid rate must never grant an external clock source.
        command.payload[4] = 0;
        assert(!Clarett::permitted(command, true));
        command = Clarett::audioClock(7, rate); command.length = 9;
        assert(!Clarett::permitted(command, true));
        const auto band = A::speedBand(rate);
        assert(band == (rate >= 88200));
        assert(Clarett::permitted(Clarett::streamInfo(9, band)));
        // S/MUX removes a live tail, without changing DMA stride or the HAL ring.
        const auto live = band == 0 ? 14u : 10u;
        for (uint32_t ch = 0; ch < A::RxChannels; ++ch) {
            assert(A::captureSample(0x123456ff, ch, live) == (ch < live ? 0x12345600 : 0));
            assert(A::captureSample(0xffffff55, ch, live) == (ch < live ? -256 : 0));
            assert(A::sampleOffset(true, 1, ch) == A::RxSamples + 14 * 4 + ch * 4);
        }
        for (auto margin : {128u, 256u, 512u}) {
            const auto interval = A::pollTicks(ticks, rate, margin);
            const auto periodTicks = A::ticksForFrames(ticks, margin / 2, rate);
            assert(interval > 0 && interval * 4 <= periodTicks);
            assert(std::abs(double(interval) / ticks - double(margin == 512 ? 24 : 12) / rate) < 1e-9);
        }
        const auto ringTicks = A::ticksForFrames(ticks, A::Frames, rate);
        const auto watchdog = A::ticksForFrames(ticks, A::Frames * 7 / 8, rate);
        assert(watchdog < ringTicks && watchdog > ringTicks * 3 / 4);
        assert(A::timestampFrame(A::Frames + 64) == A::Frames);
    }
    for (auto invalid : {0u, 1u, 32000u, 48001u, 64000u, 176400u, 192000u, UINT32_MAX}) {
        assert(!A::rateValid(invalid));
        assert(!Clarett::permitted(Clarett::audioClock(0, invalid), true));
    }
    assert(!A::rateValid(48000.5));
    assert(!A::rateValid(std::numeric_limits<double>::infinity()));
    assert(!A::rateValid(std::numeric_limits<double>::quiet_NaN()));
    assert(!Clarett::permitted(Clarett::streamInfo(0, 2)));
    assert(A::pollTicks(ticks, 96000, 512) == 250000);
    assert(A::pollTicks(ticks, 48000, 512) == 500000);
    assert(A::ticksForFrames(ticks, A::Frames * 7 / 8, 96000) < 40000000);
    puts("PASS: four-rate clock allowlist, speed bands, S/MUX residue suppression, rate-scaled polling, wrap watchdog and timestamps");
}
