// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Volume.hpp"

namespace Clarett::Mute {
constexpr uint8_t AnalogueMask = 0x0f; // original 2Pre: four analogue outputs
struct State { bool enabled = false, known = false; };
struct Wire {
    virtual ~Wire() = default;
    virtual bool masterMute(bool enabled) = 0;
    virtual bool assignMute() = 0;
    virtual bool muteAssigned() = 0;
    virtual bool softwareOwned(unsigned pair) = 0;
    virtual bool gains(unsigned pair, unsigned attenuation) = 0;
};
inline Volume::Result apply(State &state, Volume::State &volume, bool enabled, Wire &wire) {
    if (volume.uncertain) return Volume::Result::Uncertain;
    // Core Audio can restore an initial false value. Never unmute an unknown
    // device at attach; an explicit mute establishes this session's ownership.
    if (!enabled && (!state.known || !state.enabled)) return Volume::Result::Verified;
    bool ok;
    if (enabled) {
        // Assert the master flag BEFORE adding output assignments. Neither
        // operation can release a previously muted output or raise its gain.
        ok = wire.masterMute(true) && wire.assignMute();
    } else {
        ok = wire.muteAssigned();
        // Reverify gain ownership and apply capped gains BEFORE releasing mute.
        // Gain changes while muted remain intentional and obey the same caps.
        for (unsigned p = 0; p < Volume::Pairs && ok; ++p)
            if (volume.settings.enabled[p])
                ok = wire.softwareOwned(p) && wire.gains(p, Volume::gain(volume.settings, p));
        if (ok) ok = wire.masterMute(false);
    }
    if (!ok) {
        // No further command, including unmute, may use uncertain state. The
        // driver transport also refuses any exchange after a mailbox failure.
        volume.uncertain = true;
        state.known = false;
        return Volume::Result::Failed;
    }
    state.enabled = enabled; state.known = true;
    return Volume::Result::Verified;
}
} // namespace Clarett::Mute
