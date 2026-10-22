// SPDX-License-Identifier: GPL-2.0-only
#include "Mute.hpp"
#include "Protocol.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace M = Clarett::Mute;
namespace V = Clarett::Volume;
struct Wire final : M::Wire {
    struct Event { unsigned kind, pair, value; };
    std::vector<Event> events;
    unsigned calls = 0, failAt = ~0u;
    unsigned gain[2]{127,127}; bool owns[2]{true,true};
    uint8_t assignment = 0x80; bool muted = false, ambiguousRelease = false;
    bool step(unsigned kind, unsigned pair = 0, unsigned value = 0) {
        events.push_back({kind,pair,value}); return calls++ != failAt;
    }
    bool masterMute(bool value) override {
        if (!step(0,0,value)) return false;
        muted = value; return value || !ambiguousRelease;
    }
    bool assignMute() override {
        assert(muted); // assignment can never precede master mute assertion
        if (!step(1)) return false;
        assignment |= M::AnalogueMask; return true;
    }
    bool muteAssigned() override { return step(2) && (assignment & M::AnalogueMask) == M::AnalogueMask; }
    bool softwareOwned(unsigned p) override { return step(3,p) && owns[p]; }
    bool gains(unsigned p, unsigned a) override {
        assert(muted); // capped gain restoration must precede mute release
        if (!step(4,p,a)) return false;
        gain[p] = a; return true;
    }
};
int main() {
    V::State volume; M::State mute; Wire wire;
    // Initial false restores from macOS cannot unmute an unknown device.
    wire.muted = true;
    assert(M::apply(mute,volume,false,wire) == V::Result::Verified);
    assert(wire.events.empty() && wire.muted && !mute.known);
    assert(M::apply(mute,volume,true,wire) == V::Result::Verified);
    assert(mute.enabled && mute.known && wire.assignment == 0x8f);
    assert(wire.events[0].kind == 0 && wire.events[0].value == 1 && wire.events[1].kind == 1);
    volume.settings = {{true,true},{30,40},12};
    assert(M::apply(mute,volume,false,wire) == V::Result::Verified);
    assert(!mute.enabled && mute.known && !wire.muted);
    assert(wire.gain[0] == 42 && wire.gain[1] == 52);
    assert(volume.settings.attenuation == 12); // mute never overwrites volume
    const auto completed = wire.events.size();
    assert(M::apply(mute,volume,false,wire) == V::Result::Verified && wire.events.size() == completed);
    // Volume/ceiling changes while muted are applied safely before release.
    assert(M::apply(mute,volume,true,wire) == V::Result::Verified);
    volume.settings.ceiling[0] = 60; volume.settings.attenuation = 0;
    assert(M::apply(mute,volume,false,wire) == V::Result::Verified);
    assert(wire.gain[0] == 60 && wire.gain[1] == 40);
    // Mute covers the analogue outputs even in hardware-knob mode. Unmute
    // never changes gain ownership or writes gain for an uncontrolled pair.
    V::State hardware; M::State hm; Wire hw;
    hw.owns[0] = hw.owns[1] = false;
    assert(M::apply(hm,hardware,true,hw) == V::Result::Verified);
    assert(M::apply(hm,hardware,false,hw) == V::Result::Verified);
    for (const auto &event : hw.events) assert(event.kind != 3 && event.kind != 4);
    for (unsigned point = 0; point < 2; ++point) {
        V::State v; M::State m; Wire w; w.failAt = point;
        assert(M::apply(m,v,true,w) == V::Result::Failed && v.uncertain);
        const auto count = w.events.size();
        assert(M::apply(m,v,false,w) == V::Result::Uncertain && w.events.size() == count);
        for (const auto &e : w.events) assert(e.kind != 0 || e.value == 1);
    }
    // Every failure before mute release must leave master mute asserted.
    for (unsigned point = 0; point < 6; ++point) {
        V::State v; M::State m; Wire w;
        v.settings = {{true,true},{30,40},12};
        assert(M::apply(m,v,true,w) == V::Result::Verified);
        w.calls = 0; w.events.clear(); w.failAt = point;
        assert(M::apply(m,v,false,w) == V::Result::Failed && v.uncertain && w.muted);
        const auto count = w.events.size();
        assert(M::apply(m,v,false,w) == V::Result::Uncertain && w.events.size() == count);
    }
    {
        V::State v; M::State m; Wire w;
        v.settings = {{true,true},{30,40},12};
        assert(M::apply(m,v,true,w) == V::Result::Verified);
        w.ambiguousRelease = true; // hardware changed, acknowledgement lost
        assert(M::apply(m,v,false,w) == V::Result::Failed && v.uncertain && !m.known);
        assert(!w.muted && w.gain[0] == 42 && w.gain[1] == 52); // caps were verified first
        const auto n = w.events.size();
        assert(M::apply(m,v,false,w) == V::Result::Uncertain && w.events.size() == n);
    }
    for (bool ownershipConflict : {false,true}) {
        V::State v; M::State m; Wire w;
        v.settings.enabled[0] = true;
        assert(M::apply(m,v,true,w) == V::Result::Verified);
        if (ownershipConflict) w.owns[0] = false; else w.assignment = 0;
        assert(M::apply(m,v,false,w) == V::Result::Failed && v.uncertain && w.muted);
    }
    // New config access remains narrowly limited to master mute and adding
    // the four mute assignments. Dim, knob, unrelated bytes and flash are denied.
    assert(Clarett::permitted(Clarett::volumeWrite(0,24,1),true,true));
    assert(Clarett::permitted(Clarett::volumeWrite(0,24,0),true,true));
    assert(!Clarett::permitted(Clarett::volumeWrite(0,24,2),true,true));
    assert(Clarett::permitted(Clarett::volumeWrite(0,72,0x8f),true,true));
    assert(!Clarett::permitted(Clarett::volumeWrite(0,72,0x8e),true,true));
    assert(!Clarett::permitted(Clarett::volumeWrite(0,28,1),true,true));
    assert(!Clarett::permitted(Clarett::volumeWrite(0,112,0),true,true));
    assert(Clarett::permitted(Clarett::muteCommit(0,false),true,true));
    assert(!Clarett::permitted(Clarett::muteCommit(0,false),true,false));
    std::puts("PASS: master mute order, capped unmute, unchanged volume, hardware ownership, conflicts, failures and narrow command scope");
}
