// SPDX-License-Identifier: GPL-2.0-only
#include "Protocol.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace Clarett;

struct FakeTransport final : Transport {
    enum Scenario { Success, ExtendedStreamInfo, HeaderBeforePayload, Timeout, WrongSequence, BadLength, ShortLength, DeviceError, Unplug } scenario;
    uint8_t *buffer;
    unsigned ticks = 0, acks = 0, submits = 0;
    uint32_t command = 0, sequenceLength = 0;
    bool doneSeen = false, earlyPhaseRead = false;
    explicit FakeTransport(Scenario s, uint8_t *p) : scenario(s), buffer(p) {}
    void word(unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) buffer[offset + i] = uint8_t(value >> (8 * i));
    }
    bool read(uint32_t offset, uint32_t &value) override {
        if (scenario == Unplug && ticks > 1) return false;
        if (offset == 0x100) { value = Done; doneSeen = true; return true; }
        if (offset == 0x400) {
            if (!doneSeen) earlyPhaseRead = true;
            const bool ready = scenario != Timeout && scenario != Unplug && (scenario != HeaderBeforePayload || ticks >= 3);
            word(0, command);
            const uint16_t sequence = uint16_t(sequenceLength >> 16) + (scenario == WrongSequence ? 1 : 0);
            const uint16_t length = command == (Execute | GetData) ? 8 : 4;
            const uint16_t reported = scenario == BadLength ? 65535 : scenario == ShortLength ? 3 :
                scenario == ExtendedStreamInfo && command == (Execute | StreamInfo) ? 24 : length;
            word(4, uint32_t(sequence) << 16 | reported);
            word(8, scenario == DeviceError ? 3 : 0);
            if (ready) { buffer[16] = 4; buffer[18] = 14; }
            value = ready ? 2 : 0;
            return true;
        }
        value = 0; return true;
    }
    void write(uint32_t offset, uint32_t value) override {
        if (offset == Mailbox) command = value;
        if (offset == Mailbox + 4) sequenceLength = value;
        if (offset == Doorbell && value == 1) ++submits;
        if (offset == Doorbell && value == 2) {
            assert(scenario == Success || scenario == ExtendedStreamInfo || (scenario == HeaderBeforePayload && ticks >= 3));
            ++acks;
        }
    }
    void pause() override { ++ticks; }
};

int main() {
    for (auto scenario : {FakeTransport::Success, FakeTransport::ExtendedStreamInfo, FakeTransport::HeaderBeforePayload,
                          FakeTransport::Timeout, FakeTransport::WrongSequence,
                          FakeTransport::BadLength, FakeTransport::ShortLength, FakeTransport::DeviceError, FakeTransport::Unplug}) {
        uint8_t response[64] = {};
        FakeTransport io(scenario, response);
        Session session(io, response, sizeof(response));
        auto result = session.transact(streamInfo(7));
        if (scenario == FakeTransport::Success || scenario == FakeTransport::ExtendedStreamInfo || scenario == FakeTransport::HeaderBeforePayload) {
            assert(result == Result::Success && io.acks == 1);
            assert(load16(response + 16) == 4 && load16(response + 18) == 14);
        } else {
            assert(result != Result::Success && io.acks == 0);
            assert(session.transact(streamInfo(8)) == Result::Poisoned);
            assert(io.submits == 1);
        }
        assert(!io.earlyPhaseRead);
    }
    uint8_t response[64] = {};
    FakeTransport io(FakeTransport::Success, response);
    Session session(io, response, sizeof(response));
    for (uint16_t sequence = 0; sequence < 6; ++sequence) {
        assert(session.transact(sequence % 2 == 0 ? streamInfo(sequence) : firmwareInfo(sequence)) == Result::Success);
    }
    assert(io.submits == 6 && io.acks == 6);
    auto forbidden = firmwareInfo(0);
    forbidden.payload[0] = 24;
    assert(session.transact(forbidden) == Result::NotPermitted && io.submits == 6);
    forbidden = firmwareInfo(0); forbidden.opcode = 0x800001; // SET_DATA is never allowed
    assert(session.transact(forbidden) == Result::NotPermitted && io.submits == 6);
    assert(validate(streamInfo(0), response, 3, true) == Response::Pending);
    // A longer geometry response is valid, but GET_DATA retains an exact size.
    io.word(0, Execute | GetData); io.word(4, 24); io.word(8, 0);
    assert(validate(firmwareInfo(0), response, sizeof(response), true) == Response::InvalidLength);
    puts("PASS: completion ordering, extended geometry, short/oversize rejection, stale sequence, device errors, timeout, unplug, terminal failures and command allowlist");
}
