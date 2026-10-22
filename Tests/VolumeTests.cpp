// SPDX-License-Identifier: GPL-2.0-only
#include "Volume.hpp"
#include "Protocol.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
#include <limits>
namespace V = Clarett::Volume;
struct Wire final : V::Wire {
    struct Event { unsigned kind, pair, value; };
    std::vector<Event> events;
    unsigned knobValue = 40, calls = 0, failAt = ~0u;
    unsigned hardware[2]{127,127}; bool enabled[2]{};
    bool step() { return calls++ != failAt; }
    bool knob(unsigned &a) override { a = knobValue; events.push_back({0,0,a}); return step(); }
    bool gains(unsigned p, unsigned a) override {
        events.push_back({1,p,a}); if (!step()) return false;
        assert(p < 2 && a <= 127); hardware[p] = a; return true;
    }
    bool software(unsigned p, bool e) override {
        events.push_back({2,p,unsigned(e)}); if (!step()) return false;
        if (e) assert(hardware[p] == 127); // floor must precede takeover
        enabled[p] = e; return true;
    }
};
int main() {
    uint32_t attenuation = 0;
    assert(V::attenuationFromDecibels(-0.1f, attenuation) && attenuation == 1);
    assert(V::attenuationFromDecibels(0, attenuation) && attenuation == 0);
    assert(V::attenuationFromDecibels(-48, attenuation) && attenuation == 127);
    assert(V::attenuationFromDecibels(-47.1f, attenuation) && attenuation == 127);
    assert(V::attenuationFromDecibels(-47, attenuation) && attenuation == 47);
    assert(V::attenuationFromDecibels(-24, attenuation) && attenuation == 24);
    assert(V::controlDecibels(127) == -48 && V::controlDecibels(74) == -48);
    assert(V::controlDecibels(24) == -24 && V::controlDecibels(0) == 0);
    unsigned previous = V::Floor;
    for (unsigned percent = 0; percent <= 100; ++percent) {
        const float db = -float(V::ControlRange) * (1 - float(percent) / 100);
        unsigned current = 0;
        assert(V::attenuationFromDecibels(db, current));
        assert(current <= previous); previous = current;
        for (unsigned cap = 0; cap <= 60; ++cap) {
            V::Settings point{{true,true},{cap,cap},current};
            assert(V::gain(point,0) >= cap);
        }
    }
    assert(previous == 0);
    for (auto invalid : {0.1f, -48.1f, -127.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        assert(!V::attenuationFromDecibels(invalid, attenuation));
    for (unsigned cap = 0; cap <= 60; ++cap) for (unsigned a = 0; a <= 127; ++a) {
        V::Settings s{{true,true},{cap,cap},a};
        assert(V::valid(s) && V::gain(s,0) >= cap && V::gain(s,0) <= 127);
        if (!a) assert(V::gain(s,0) == cap); // full software volume == ceiling
    }
    V::State state; Wire w;
    auto request = state.settings; request.enabled[0] = request.enabled[1] = true;
    assert(V::apply(state, request, w) == V::Result::Verified);
    assert(w.hardware[0] == 127 && w.hardware[1] == 127);
    assert(w.events[1].kind == 1 && w.events[2].kind == 1); // both staged before any ownership commit
    request.attenuation = 0;
    assert(V::apply(state, request, w) == V::Result::Verified);
    assert(w.hardware[0] == 30 && w.hardware[1] == 30);
    request.ceiling[0] = 10; // raise ceiling -30 -> -10 dB; must NOT turn up
    assert(V::apply(state, request, w) == V::Result::Verified);
    assert(state.settings.attenuation == 20 && w.hardware[0] == 30 && w.hardware[1] == 50);
    request = state.settings; request.attenuation = 0;
    assert(V::apply(state, request, w) == V::Result::Verified);
    assert(w.hardware[0] == 10 && w.hardware[1] == 30);
    request.ceiling[0] = 50;
    assert(V::apply(state, request, w) == V::Result::Verified && w.hardware[0] == 50);
    request.enabled[0] = false; w.knobValue = 0;
    const auto count = w.events.size();
    assert(V::apply(state, request, w) == V::Result::TurnKnobDown);
    assert(w.events.size() == count + 1 && state.settings.enabled[0]);
    w.knobValue = 127;
    assert(V::apply(state, request, w) == V::Result::Verified && !w.enabled[0]);
    // At full-down knob, takeover still succeeds quietly (no unrepresentable minimum).
    V::State down; Wire dw; dw.knobValue = 127;
    auto ds = down.settings; ds.enabled[0] = true; ds.ceiling[0] = 0; ds.attenuation = 0;
    assert(V::apply(down, ds, dw) == V::Result::Verified && dw.hardware[0] == 127);
    for (unsigned point = 1; point < 7; ++point) {
        V::State fail; Wire fw; fw.failAt = point;
        auto fs = fail.settings; fs.enabled[0] = fs.enabled[1] = true;
        assert(V::apply(fail, fs, fw) == V::Result::Failed && fail.uncertain);
        const auto n = fw.events.size();
        assert(V::apply(fail, fs, fw) == V::Result::Uncertain && fw.events.size() == n);
        assert(fw.hardware[0] == 127 && fw.hardware[1] == 127);
    }
    auto invalid = state.settings; invalid.ceiling[0] = 61;
    assert(V::apply(state, invalid, w) == V::Result::Invalid);
    invalid = state.settings; invalid.attenuation = 128;
    assert(V::apply(state, invalid, w) == V::Result::Invalid);
    // Protocol: only exact stereo gain bytes and gain-owner flags, no flash or knob writes.
    for (unsigned off = 0; off < 256; ++off) {
        const auto read = Clarett::volumeRead(4, off, off == 32 || off == 36 ? 2 : 1);
        const bool allowed = off == 24 || off == 32 || off == 36 || off == 52 || off == 56 || off == 72 || off == 112;
        assert(Clarett::permitted(read, true, true) == allowed);
        auto write = Clarett::volumeWrite(4, off, off == 24 ? 1 : off == 72 ? 0x8f : 3, 3);
        assert(Clarett::permitted(write, true, true) == (allowed && off != 112));
        assert(!Clarett::permitted(write, true, false));
    }
    for (unsigned cmd = 0; cmd < 10; ++cmd) {
        auto r = Clarett::volumeCommit(4, false); Clarett::put32(r.payload, cmd);
        assert(Clarett::permitted(r, true, true) == (cmd == 1 || cmd == 2 || cmd == 3));
    }
    auto bad = Clarett::volumeWrite(4, 32, 128, 0);
    assert(!Clarett::permitted(bad,true,true));
    bad = Clarett::volumeWrite(4,52,4);
    assert(!Clarett::permitted(bad,true,true));
    bad = Clarett::volumeRead(4,32,4);
    assert(!Clarett::permitted(bad,true,true));
    std::puts("Volume safety and protocol tests passed");
}
