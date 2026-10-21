// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace Clarett::Monitor {
constexpr unsigned Routes = 48, Buses = 16, Inputs = 30;
struct Options {
    bool enabled = false;
    uint32_t inputMode = 1; // 1: input 1 mono; 2: input 2 mono; 3: 1/2 stereo
    uint32_t outputPair = 2; // 1: lines 1/2; 2: lines 3/4 (headphones); 3: both
    uint32_t blend = 50; // live input percentage; complementary PCM gain
};
inline unsigned routeCount(unsigned band) { return band == 0 ? Routes : band == 1 ? 44 : 0; }
struct Snapshot { uint32_t routes[Routes]; uint16_t rows[Buses][Inputs]; unsigned band = 0; };
struct Plan {
    Snapshot before;
    uint32_t setup[Routes], active[Routes];
    uint16_t rows[2][Inputs];
    unsigned buses[2], slots[4];
    Options options;
};
inline bool valid(const Options &o) {
    return o.inputMode >= 1 && o.inputMode <= 3 && o.outputPair >= 1 && o.outputPair <= 3 && o.blend <= 100;
}
inline bool source(uint32_t pin, unsigned band = 0) {
    return pin == 0 || pin == 0x400 || pin == 0x402 || pin == 0x186 || pin == 0x187 ||
        (pin >= 0x200 && pin <= (band == 0 ? 0x207u : 0x203u)) || (pin >= 0x300 && pin <= 0x30f) || (pin >= 0x600 && pin <= 0x603);
}
inline bool destination(uint32_t pin, unsigned band = 0) {
    return (pin >= 0x400 && pin <= 0x403) || (pin >= 0x300 && pin <= 0x31d) || (pin >= 0x600 && pin <= (band == 0 ? 0x60du : 0x609u));
}
inline bool routesValid(const uint32_t *entries, unsigned band = 0) {
    const auto count = routeCount(band);
    if (!entries || !count) return false;
    for (unsigned i = 0; i < count; ++i) {
        if ((entries[i] & 0xff000000) || !source(entries[i] >> 12, band) || !destination(entries[i] & 4095, band)) return false;
        for (unsigned j = 0; j < i; ++j)
            if ((entries[i] & 4095) == (entries[j] & 4095)) return false;
    }
    return true;
}
inline int routeIndex(const uint32_t *entries, uint32_t destinationPin, unsigned band = 0) {
    for (unsigned i = 0; i < routeCount(band); ++i) if ((entries[i] & 4095) == destinationPin) return int(i);
    return -1;
}
inline bool build(const Snapshot &snapshot, const Options &options, Plan &plan) {
    if (!valid(options) || !routesValid(snapshot.routes, snapshot.band)) return false;
    plan = {};
    plan.before = snapshot;
    plan.options = options;
    bool active[Buses] = {}, reserved[Inputs] = {};
    for (unsigned i = 0; i < routeCount(snapshot.band); ++i) {
        plan.setup[i] = plan.active[i] = snapshot.routes[i];
        const auto src = snapshot.routes[i] >> 12;
        if (src >= 0x300 && src <= 0x30f) active[src - 0x300] = true;
    }
    unsigned found = 0;
    for (int bus = Buses - 1; bus >= 0 && found < 2; --bus)
        if (!active[bus]) plan.buses[found++] = unsigned(bus);
    if (found != 2) return false; // never take over an already-routed mixer bus
    const uint32_t sources[] = {0x400, 0x402, 0x600, 0x601};
    for (unsigned s = 0; s < 4; ++s) {
        int slot = -1;
        for (unsigned k = 0; k < Inputs; ++k) {
            const auto idx = routeIndex(snapshot.routes, 0x300 + k, snapshot.band);
            if (!reserved[k] && (snapshot.routes[idx] >> 12) == sources[s]) { slot = int(k); break; }
        }
        if (slot < 0) {
            // A slot may be reassigned only if it is inaudible in EVERY active
            // original bus. Prefer unrouted slots, then otherwise unused slots.
            for (unsigned pass = 0; pass < 2 && slot < 0; ++pass)
                for (unsigned k = 0; k < Inputs; ++k) {
                    if (reserved[k]) continue;
                    const auto idx = routeIndex(snapshot.routes, 0x300 + k, snapshot.band);
                    if (pass == 0 && (snapshot.routes[idx] >> 12) != 0) continue;
                    bool silent = true;
                    for (unsigned b = 0; b < Buses; ++b) if (active[b] && snapshot.rows[b][k] != 0) silent = false;
                    if (silent) { slot = int(k); break; }
                }
        }
        if (slot < 0) return false;
        plan.slots[s] = unsigned(slot); reserved[slot] = true;
        const auto idx = routeIndex(snapshot.routes, 0x300 + unsigned(slot), snapshot.band);
        plan.setup[idx] = plan.active[idx] = sources[s] << 12 | (0x300 + unsigned(slot));
    }
    const auto live = uint16_t(8192u * options.blend / 100);
    const auto pcm = uint16_t(8192u - live);
    plan.rows[0][plan.slots[2]] = pcm;
    plan.rows[1][plan.slots[3]] = pcm;
    if (options.inputMode == 1) plan.rows[0][plan.slots[0]] = plan.rows[1][plan.slots[0]] = live;
    if (options.inputMode == 2) plan.rows[0][plan.slots[1]] = plan.rows[1][plan.slots[1]] = live;
    if (options.inputMode == 3) { plan.rows[0][plan.slots[0]] = live; plan.rows[1][plan.slots[1]] = live; }
    for (unsigned output = 0; output < 4; ++output) {
        if ((options.outputPair == 1 && output >= 2) || (options.outputPair == 2 && output < 2)) continue;
        const auto idx = routeIndex(snapshot.routes, 0x400 + output, snapshot.band);
        plan.active[idx] = (0x300 + plan.buses[output & 1]) << 12 | (0x400 + output);
    }
    return true;
}
inline bool sameRoutes(const uint32_t *a, const uint32_t *b, unsigned band = 0) {
    for (unsigned i = 0; i < routeCount(band); ++i) if (a[i] != b[i]) return false;
    return true;
}
} // namespace Clarett::Monitor
