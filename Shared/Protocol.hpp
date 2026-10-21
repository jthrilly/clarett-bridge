// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Clarett {
constexpr uint32_t Execute = 0x80000000;
constexpr uint32_t Done = 0x20000000;
constexpr uint32_t GetData = 0x800000;
constexpr uint32_t StreamInfo = 0x007001;
constexpr uint32_t Mailbox = 0x8020;
constexpr uint32_t Doorbell = 0x408;
constexpr size_t HeaderSize = 16;

inline uint16_t load16(const volatile uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
inline uint32_t load32(const volatile uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct Request {
    uint32_t opcode;
    uint16_t sequence;
    uint16_t length;
    uint16_t expectedLength;
    uint8_t payload[8];
};

inline Request streamInfo(uint16_t sequence) {
    return { StreamInfo, sequence, 1, 4, { 0 } }; // base sample-rate band
}
inline Request firmwareInfo(uint16_t sequence) {
    // GET_DATA offset 8, length 8: application/FPGA identification words only.
    return { GetData, sequence, 8, 8, { 8, 0, 0, 0, 8, 0, 0, 0 } };
}
inline Request audioClock(uint16_t sequence) {
    return { 0x006003, sequence, 8, 0, { 0x80, 0xbb, 0, 0, 24, 0, 0, 0 } }; // 48 kHz, internal
}
inline Request audioSync(uint32_t opcode, uint16_t sequence) {
    return { opcode, sequence, 0, uint16_t(opcode == 0x006002 ? 0 : 4), {} };
}
inline bool permitted(const Request &r, bool audio = false) {
    if (audio) {
        const auto clock = audioClock(r.sequence);
        bool same = r.opcode == clock.opcode && r.length == clock.length && r.expectedLength == 0;
        for (size_t i = 0; i < 8; ++i) same &= r.payload[i] == clock.payload[i];
        if (same) return true;
        if ((r.opcode == 0x006004 || r.opcode == 0x006002 || r.opcode == 0x006005) &&
            r.length == 0 && r.expectedLength == (r.opcode == 0x006002 ? 0 : 4)) return true;
    }
    if (r.opcode == StreamInfo && r.length == 1 && r.expectedLength == 4 && r.payload[0] == 0)
        return true;
    const auto f = firmwareInfo(r.sequence);
    if (r.opcode != f.opcode || r.length != f.length || r.expectedLength != f.expectedLength)
        return false;
    for (size_t i = 0; i < sizeof(r.payload); ++i)
        if (r.payload[i] != f.payload[i]) return false;
    return true;
}

enum class Response { Pending, WrongSequence, DeviceError, InvalidLength, Complete };
inline Response validate(const Request &r, const volatile uint8_t *buffer,
                         size_t capacity, bool landed) {
    // A header echo alone does not prove that the payload DMA has completed.
    if (!landed || capacity < HeaderSize || load32(buffer) != (Execute | r.opcode))
        return Response::Pending;
    if (load16(buffer + 6) != r.sequence) return Response::WrongSequence;
    if (load32(buffer + 8) != 0) return Response::DeviceError;
    const auto length = load16(buffer + 4);
    // STREAM_INFO includes firmware-dependent trailing fields (24 bytes on the
    // test 2Pre). Consume only its four-byte geometry prefix. GET_DATA must
    // return exactly the requested eight identification bytes.
    if (length < r.expectedLength || length > capacity - HeaderSize ||
        (r.opcode == GetData && length != r.expectedLength))
        return Response::InvalidLength;
    return Response::Complete;
}

// All polling is bounded. Once a command times out, the session is terminal:
// no next command, no buffer reuse, and no premature doorbell acknowledgement.
struct Transport {
    virtual ~Transport() = default;
    virtual bool read(uint32_t offset, uint32_t &value) = 0;
    virtual void write(uint32_t offset, uint32_t value) = 0;
    virtual void pause() = 0;
};
enum class Result { Success, NotPermitted, Disconnected, Timeout, BadResponse, Poisoned };

class Session {
public:
    Session(Transport &transport, volatile uint8_t *response, size_t capacity, bool audio = false)
        : io(transport), buffer(response), size(capacity), allowAudio(audio) {}

    Response lastResponse() const { return responseState; }
    uint32_t observedPhase() const { return phaseBits; }
    uint32_t observedCause() const { return causeBits; }

    Result transact(const Request &r) {
        if (poisoned) return Result::Poisoned;
        if (!permitted(r, allowAudio) || size < HeaderSize + r.expectedLength) return Result::NotPermitted;
        responseState = Response::Pending;
        phaseBits = causeBits = 0;
        // Volatile stores are intentional: memory is shared with PCI DMA.
        for (size_t i = 0; i < size; ++i) buffer[i] = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        io.write(Mailbox, Execute | r.opcode);
        io.write(Mailbox + 4, uint32_t(r.sequence) << 16 | r.length);
        io.write(Mailbox + 8, 0);
        io.write(Mailbox + 12, 0);
        for (size_t i = 0; i < r.length; i += 4) {
            uint32_t word = 0;
            for (size_t j = 0; j < 4 && i + j < r.length; ++j)
                word |= uint32_t(r.payload[i + j]) << (8 * j);
            io.write(Mailbox + 16 + uint32_t(i), word);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        io.write(Doorbell, 1);
        bool done = false, landed = false;
        for (unsigned tick = 0; tick < 500; ++tick) {
            uint32_t cause = 0;
            if (!io.read(0x100, cause)) return fail(Result::Disconnected);
            causeBits |= cause;
            done |= (cause & Done) != 0;
            // Do not consume the read-to-clear phase register before DONE.
            if (done) {
                if (!io.read(0x400, cause)) return fail(Result::Disconnected);
                phaseBits |= cause;
                landed |= (cause & 2) != 0;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                const auto state = validate(r, buffer, size, landed);
                responseState = state;
                if (state == Response::Complete) {
                    io.write(Doorbell, 2);
                    return Result::Success;
                }
                if (state != Response::Pending) return fail(Result::BadResponse);
            }
            io.pause();
        }
        return fail(Result::Timeout);
    }
private:
    Result fail(Result result) { poisoned = true; return result; }
    Transport &io;
    volatile uint8_t *buffer;
    size_t size;
    bool poisoned = false;
    bool allowAudio = false;
    Response responseState = Response::Pending;
    uint32_t phaseBits = 0, causeBits = 0;
};
} // namespace Clarett
