// SPDX-License-Identifier: GPL-2.0-only
#include "Monitor.hpp"
#include "Protocol.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
namespace M = Clarett::Monitor;
static M::Snapshot fixture(unsigned band = 0) {
    M::Snapshot s = {};
    s.band = band;
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i) s.routes[n++] = ((0x300 + (i & 1)) << 12) | (0x400 + i);
    for (unsigned i = 0; i < 30; ++i) s.routes[n++] = 0x300 + i;
    for (unsigned i = 0; i < (band == 0 ? 14u : 10u); ++i) s.routes[n++] = (0x400u << 12) | (0x600 + i);
    // Active original rows use slots 10/11; slots 0..3 are free to configure.
    s.routes[M::routeIndex(s.routes, 0x30a)] = (0x600u << 12) | 0x30a;
    s.routes[M::routeIndex(s.routes, 0x30b)] = (0x601u << 12) | 0x30b;
    s.rows[0][10] = s.rows[1][11] = 8192;
    for (unsigned b = 2; b < 16; ++b) for (unsigned i = 0; i < 30; ++i) s.rows[b][i] = uint16_t(b * 30 + i);
    assert(n == M::routeCount(band) && M::routesValid(s.routes, band));
    return s;
}
struct Wire final : Clarett::Transport {
    uint8_t *reply;
    uint32_t echo = 0, seq = 0;
    unsigned submits = 0, acks = 0;
    std::vector<uint32_t> payload;
    explicit Wire(uint8_t *response) : reply(response) {}
    bool read(uint32_t offset, uint32_t &value) override {
        if (offset == 0x100) value = Clarett::Done;
        else if (offset == 0x400) {
            Clarett::put32(reply, echo); Clarett::put32(reply + 4, seq << 16);
            Clarett::put32(reply + 8, 0); value = 2;
        } else value = 0;
        return true;
    }
    void write(uint32_t offset, uint32_t value) override {
        if (offset == Clarett::Mailbox) echo = value;
        if (offset == Clarett::Mailbox + 4) seq = value >> 16;
        if (offset >= Clarett::Mailbox + 16 && offset < Clarett::Mailbox + 16 + 512) payload.push_back(value);
        if (offset == Clarett::Doorbell && value == 1) ++submits;
        if (offset == Clarett::Doorbell && value == 2) ++acks;
    }
    void pause() override {}
};
int main() {
    const auto original = fixture();
    for (unsigned mode = 1; mode <= 3; ++mode) for (unsigned pair = 1; pair <= 3; ++pair)
        for (unsigned blend : {0u, 25u, 50u, 100u}) {
            M::Plan p;
            assert(M::build(original, {true, mode, pair, blend}, p));
            assert(p.buses[0] == 15 && p.buses[1] == 14);
            assert(!std::memcmp(&p.before, &original, sizeof(original)));
            assert(M::routesValid(p.setup) && M::routesValid(p.active));
            assert((p.setup[M::routeIndex(p.setup, 0x300 + p.slots[1])] >> 12) == 0x402); // 2Pre pin gap
            for (unsigned i = 0; i < M::Routes; ++i) {
                const auto dst = original.routes[i] & 4095;
                if (dst >= 0x600 && dst <= 0x60d) assert(original.routes[i] == p.active[i]);
                if (dst >= 0x400 && dst <= 0x403) {
                    assert(p.setup[i] == original.routes[i]); // configure inputs before switching outputs
                    const unsigned output = dst - 0x400;
                    bool selected = pair == 3 || (pair == 1 && output < 2) || (pair == 2 && output >= 2);
                    assert(selected ? p.active[i] != original.routes[i] : p.active[i] == original.routes[i]);
                }
            }
            for (unsigned b = 0; b < 2; ++b) {
                unsigned sum = 0;
                for (auto gain : p.rows[b]) sum += gain;
                assert(sum == 8192); // complementary PCM/live gains cannot increase worst-case peak
            }
            auto write = Clarett::monitorMuxWrite(0, p.active);
            assert(!Clarett::permitted(write, true));
            assert(Clarett::permitted(write, true, true));
            write.payload[4 + 48*4] = 1; assert(!Clarett::permitted(write, true, true));
            assert(Clarett::permitted(Clarett::monitorMixWrite(0, p.buses[0], p.rows[0]), true, true));
        }
    M::Plan wired;
    assert(M::build(original, {true, 1, 2, 50}, wired));
    uint8_t response[512] = {};
    Wire wire(response);
    auto mux = Clarett::monitorMuxWrite(9, wired.active);
    Clarett::Session unprivileged(wire, response, sizeof(response), true);
    assert(unprivileged.transact(mux) == Clarett::Result::NotPermitted && wire.submits == 0);
    Clarett::Session controls(wire, response, sizeof(response), true, true);
    assert(controls.transact(mux) == Clarett::Result::Success && wire.submits == 1 && wire.acks == 1);
    assert(wire.payload.size() == 65 && wire.payload[0] == 0);
    for (unsigned i = 0; i < M::Routes; ++i) assert(wire.payload[1+i] == wired.active[i]);
    for (unsigned i = 49; i < 65; ++i) assert(wire.payload[i] == 0);
    auto bad = original;
    bad.routes[1] = bad.routes[0]; assert(!M::routesValid(bad.routes));
    bad = original; bad.routes[0] |= 0xff000000; assert(!M::routesValid(bad.routes));
    bad = original; bad.routes[0] = (0x401u << 12) | 0x400; assert(!M::routesValid(bad.routes));
    M::Plan plan;
    assert(!M::build(original, {true, 0, 2, 50}, plan));
    assert(!M::build(original, {true, 1, 4, 50}, plan));
    assert(!M::build(original, {true, 1, 2, 101}, plan));
    auto full = original;
    for (unsigned b = 0; b < 16; ++b) full.routes[4 + b] = (0x300 + b) << 12 | (0x300 + b);
    assert(!M::build(full, {true, 1, 2, 50}, plan)); // never take an active bus
    full = original;
    for (unsigned b = 0; b < 2; ++b) for (auto &gain : full.rows[b]) gain = 1;
    assert(!M::build(full, {true, 1, 2, 50}, plan)); // no inaudible slots available
    assert(Clarett::permitted(Clarett::monitorMuxRead(0, 0, 28), true, true));
    assert(Clarett::permitted(Clarett::monitorMuxRead(0, 28, 20), true, true));
    assert(!Clarett::permitted(Clarett::monitorMuxRead(0, 28, 28), true, true));
    assert(!Clarett::permitted(Clarett::monitorMixRead(0, 16), true, true));
    auto flash = Clarett::monitorMixRead(0, 0); flash.opcode = 0x800002;
    assert(!Clarett::permitted(flash, true, true));
    const auto high = fixture(1);
    for (unsigned mode = 1; mode <= 3; ++mode) for (unsigned pair = 1; pair <= 3; ++pair)
        for (unsigned blend : {0u, 50u, 100u}) {
            M::Plan p;
            assert(M::build(high, {true, mode, pair, blend}, p));
            assert(p.before.band == 1 && M::sameRoutes(p.before.routes, high.routes, 1));
            assert(M::routesValid(p.setup, 1) && M::routesValid(p.active, 1));
            for (unsigned i = 0; i < M::routeCount(1); ++i) {
                const auto dst = high.routes[i] & 4095;
                if (dst >= 0x600) assert(p.active[i] == high.routes[i]);
            }
            auto command = Clarett::monitorMuxWrite(3, p.active, 1);
            assert(command.length == 244 && Clarett::load32(command.payload) == 0x10000);
            assert(Clarett::permitted(command, true, true));
            command.payload[4 + 44*4] = 1; assert(!Clarett::permitted(command, true, true));
        }
    auto highBad = high;
    highBad.routes[0] = (0x204u << 12) | 0x400; // unavailable ADAT source at double speed
    assert(!M::routesValid(highBad.routes, 1));
    highBad = high; highBad.routes[43] = 0x60a; // missing ADAT destination
    assert(!M::routesValid(highBad.routes, 1));
    auto wrongBand = Clarett::monitorMuxWrite(0, original.routes);
    Clarett::put32(wrongBand.payload, 0x10000);
    assert(!Clarett::permitted(wrongBand, true, true)); // cannot send base geometry to band 1
    assert(Clarett::permitted(Clarett::monitorMuxRead(0, 28, 16, 1), true, true));
    assert(!Clarett::permitted(Clarett::monitorMuxRead(0, 28, 17, 1), true, true));
    assert(!Clarett::permitted(Clarett::monitorMuxRead(0, 0, 28, 2), true, true));
    assert(!M::routesValid(high.routes, 2));
    puts("PASS: single/double-speed monitor routing preservation, unused buses/slots, 2Pre pins, mono/stereo, blend headroom, snapshot retention and control allowlist");
}
