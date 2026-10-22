// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace Clarett::Volume {
constexpr unsigned Pairs = 2, Floor = 127, Range = 127, MaximumCeiling = 60, DefaultCeiling = 30;
struct Settings { bool enabled[Pairs]; unsigned ceiling[Pairs]; unsigned attenuation; };
struct State {
    Settings settings{{false, false}, {DefaultCeiling, DefaultCeiling}, Range};
    bool known[Pairs]{};
    bool uncertain = false;
};
inline bool attenuationFromDecibels(float db, uint32_t &attenuation) {
    if (!(db >= -float(Range) && db <= 0)) return false;
    attenuation = uint32_t(-db);
    if (float(attenuation) < -db) ++attenuation;
    return true;
}
inline bool valid(const Settings &s) {
    return s.attenuation <= Range && s.ceiling[0] <= MaximumCeiling && s.ceiling[1] <= MaximumCeiling;
}
inline unsigned gain(const Settings &s, unsigned pair) {
    const auto value = s.ceiling[pair] + s.attenuation;
    return value > Floor ? Floor : value;
}
// Every setter includes command acknowledgement and config-byte readback. Boot
// config is not an authoritative snapshot of the physical output ownership.
struct Wire {
    virtual ~Wire() = default;
    virtual bool knob(unsigned &attenuation) = 0;
    virtual bool gains(unsigned pair, unsigned attenuation) = 0;
    virtual bool software(unsigned pair, bool enabled) = 0;
};
enum class Result { Verified, Invalid, Uncertain, TurnKnobDown, Failed };
inline Result apply(State &state, Settings next, Wire &wire) {
    if (!valid(next)) return Result::Invalid;
    if (state.uncertain) return Result::Uncertain;
    const auto before = state.settings;
    // A ceiling change is configuration, not an instruction to turn up. Keep
    // every already-controlled pair at least as quiet when raising its limit.
    for (unsigned p = 0; p < Pairs; ++p) if (before.enabled[p] && next.enabled[p] && next.ceiling[p] < before.ceiling[p]) {
        const auto oldGain = gain(before, p);
        const auto preserve = oldGain > next.ceiling[p] ? oldGain - next.ceiling[p] : 0;
        if (next.attenuation < preserve) next.attenuation = preserve;
    }
    if (next.attenuation > Range) next.attenuation = Range;
    bool ownershipChange = false;
    for (unsigned p = 0; p < Pairs; ++p) ownershipChange |= next.enabled[p] != before.enabled[p];
    unsigned knob = Floor;
    if (ownershipChange && (!wire.knob(knob) || knob > Floor)) return Result::Failed;
    // Refuse a handover that could jump up. The ceiling only governs software
    // ownership; hardware ownership deliberately restores the physical knob.
    for (unsigned p = 0; p < Pairs; ++p)
        if (before.enabled[p] && !next.enabled[p] && knob < gain(before, p)) return Result::TurnKnobDown;
    // Enabling starts no louder than the live knob, even for a raw property
    // request with a high logical level. The next explicit level request may rise.
    for (unsigned p = 0; p < Pairs; ++p) if (!before.enabled[p] && next.enabled[p]) {
        const auto quiet = knob > next.ceiling[p] ? knob - next.ceiling[p] : 0;
        if (next.attenuation < quiet) next.attenuation = quiet > Range ? Range : quiet;
    }
    bool ok = true;
    for (unsigned p = 0; p < Pairs && ok; ++p)
        if (!before.enabled[p] && next.enabled[p]) ok = wire.gains(p, Floor);
    for (unsigned p = 0; p < Pairs && ok; ++p)
        if (next.enabled[p] != before.enabled[p]) ok = wire.software(p, next.enabled[p]);
    for (unsigned p = 0; p < Pairs && ok; ++p) if (next.enabled[p]) {
        auto target = gain(next, p);
        ok = wire.gains(p, target);
    }
    if (!ok) {
        // A failed readback never becomes a reported successful level. Best
        // effort attenuation is allowed only while the mailbox remains usable;
        // the transport implementation refuses reuse after a poisoned exchange.
        for (unsigned p = 0; p < Pairs; ++p)
            if (before.enabled[p] || next.enabled[p]) wire.gains(p, Floor);
        state.uncertain = true;
        return Result::Failed;
    }
    state.settings = next;
    for (unsigned p = 0; p < Pairs; ++p) if (next.enabled[p] != before.enabled[p]) state.known[p] = true;
    return Result::Verified;
}
} // namespace Clarett::Volume
