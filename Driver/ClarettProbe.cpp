// SPDX-License-Identifier: GPL-2.0-only
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSBoolean.h>
#include <PCIDriverKit/PCIDriverKit.h>
#include <os/log.h>
#include "ClarettProbe.h"
#include "Protocol.hpp"
#define super IOService

struct ClarettProbe_IVars {
    IOPCIDevice *pci;
    IOBufferMemoryDescriptor *memory;
    IODMACommand *dma;
    volatile uint8_t *bytes;
    uint8_t bar;
    bool opened;
    bool mapped;
    bool busMaster;
    bool initialized;
};

namespace {
constexpr uint32_t BufferSize = 16384;

void number(OSDictionary *dict, const char *key, uint64_t value) {
    auto n = OSNumber::withNumber(value, 64);
    if (n) { dict->setObject(key, n); n->release(); }
}

bool commandBits(IOPCIDevice *pci, uint16_t set, uint16_t clear) {
    uint16_t command = UINT16_MAX;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
    if (command == UINT16_MAX) return false;
    const auto expected = uint16_t((command | set) & ~clear);
    pci->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, expected);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
    return command != UINT16_MAX && (command & (set | clear)) == (expected & (set | clear));
}

class PCITransport final : public Clarett::Transport {
public:
    PCITransport(IOPCIDevice *device, uint8_t index) : pci(device), bar(index) {}
    bool read(uint32_t offset, uint32_t &value) override {
        pci->MemoryRead32(bar, offset, &value);
        return value != UINT32_MAX;
    }
    void write(uint32_t offset, uint32_t value) override {
        pci->MemoryWrite32(bar, offset, value);
    }
    void pause() override { IOSleep(1); }
private:
    IOPCIDevice *pci;
    uint8_t bar;
};
}

bool ClarettProbe::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ClarettProbe_IVars, 1);
    return ivars != nullptr;
}

void ClarettProbe::free() {
    if (ivars) { IODelete(ivars, ClarettProbe_IVars, 1); }
    super::free();
}

void ClarettProbe::Publish(const char *state, uint64_t code) {
    auto props = OSDictionary::withCapacity(4);
    auto text = OSString::withCString(state);
    if (props && text) {
        props->setObject("ClarettProbeStatus", text);
        number(props, "ClarettProbeCode", code);
        SetProperties(props);
    }
    OSSafeReleaseNULL(text);
    OSSafeReleaseNULL(props);
    os_log(OS_LOG_DEFAULT, "ClarettProbe: %{public}s (0x%llx)", state, code);
}

kern_return_t IMPL(ClarettProbe, Start) {
    auto ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    ivars->pci = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->pci) return kIOReturnUnsupported;
    ret = ivars->pci->Open(this, 0);
    if (ret != kIOReturnSuccess) { Publish("pci-open-failed", ret); return ret; }
    ivars->opened = true;
    uint16_t vendor = 0, device = 0;
    ivars->pci->ConfigurationRead16(0, &vendor);
    ivars->pci->ConfigurationRead16(2, &device);
    if (vendor != 0x1cb5 || device != 0x0002) {
        Publish("wrong-pci-device");
        Quiesce();
        return kIOReturnUnsupported;
    }
    uint64_t barSize = 0;
    ret = ivars->pci->GetBARInfo(0, &ivars->bar, &barSize, nullptr);
    if (ret != kIOReturnSuccess || barSize < 65536) {
        Publish("invalid-bar0", ret);
        Quiesce();
        return kIOReturnUnsupported;
    }
    if (!commandBits(ivars->pci, kIOPCICommandMemorySpace, kIOPCICommandBusLead)) {
        Quiesce(); return kIOReturnIOError;
    }
    SetName("ClarettProbe");
    PCITransport io(ivars->pci, ivars->bar);
    uint32_t caps = 0;
    if (!io.read(0, caps)) { Publish("register-read-failed"); Quiesce(); return kIOReturnNoDevice; }
    auto props = OSDictionary::withCapacity(2);
    if (props) {
        number(props, "ClarettCapabilities", caps);
        number(props, "ClarettBARSize", barSize);
        SetProperties(props);
        props->release();
    }
    Publish("attached-register-read-ok");
    OSDictionary *properties = nullptr;
    bool mailbox = false;
    if (CopyProperties(&properties) == kIOReturnSuccess && properties) {
        auto setting = OSDynamicCast(OSString, properties->getObject("ClarettProbeMode"));
        mailbox = setting && setting->isEqualTo("mailbox");
        properties->release();
    }
    if (mailbox) {
        ret = ProbeMailbox();
        // Keep diagnostic properties visible even on a failed transaction.
        // Disable DMA before releasing mappings; Stop remains idempotent.
        Quiesce();
        if (ret != kIOReturnSuccess) {
            auto error = OSDictionary::withCapacity(1);
            if (error) {
                number(error, "ClarettProbeNativeError", uint32_t(ret));
                SetProperties(error);
                error->release();
            }
        }
    }
    return RegisterService();
}

kern_return_t ClarettProbe::ProbeMailbox() {
    Publish("allocating-response-buffer");
    auto ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, BufferSize,
                                               BufferSize, &ivars->memory);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->memory->SetLength(BufferSize);
    if (ret != kIOReturnSuccess) return ret;
    IOAddressSegment cpu = {};
    ret = ivars->memory->GetAddressRange(&cpu);
    if (ret != kIOReturnSuccess || cpu.length < BufferSize) return kIOReturnNoMemory;
    ivars->bytes = reinterpret_cast<volatile uint8_t *>(cpu.address);
    IODMACommandSpecification spec = {};
    spec.maxAddressBits = 32; // same conservative address width as snd-clarett
    Publish("creating-dma-command");
    ret = IODMACommand::Create(ivars->pci, 0, &spec, &ivars->dma);
    if (ret != kIOReturnSuccess) return ret;
    IOAddressSegment segments[32] = {};
    uint32_t count = 32;
    uint64_t flags = 0;
    Publish("mapping-response-buffer");
    ret = ivars->dma->PrepareForDMA(0, ivars->memory, 0, BufferSize, &flags, &count, segments);
    if (ret != kIOReturnSuccess) return ret;
    ivars->mapped = true;
    if (count != 1 || segments[0].length < BufferSize ||
        segments[0].address > UINT32_MAX - (BufferSize - 1) ||
        (segments[0].address & 4095) != 0 ||
        (flags & kIOMemoryDirectionIn) == 0) return kIOReturnUnsupported;
    for (uint32_t i = 0; i < BufferSize; ++i) ivars->bytes[i] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // Use the documented no-MSI polled fallback for this mailbox-only milestone.
    // No vectors are allocated and no interrupt callback consumes cause registers.
    // Disable legacy INTx delivery before enabling bus-master DMA.
    Publish("enabling-mailbox-dma");
    if (!commandBits(ivars->pci, kIOPCICommandBusLead | kIOPCICommandInterruptDisable, 0))
        return kIOReturnIOError;
    ivars->busMaster = true;
    PCITransport io(ivars->pci, ivars->bar);
    io.write(0x510, 8);
    io.write(0x500, 8);
    io.write(0x104, 0xf000003f);
    ivars->initialized = true;
    io.write(0x410, uint32_t(segments[0].address));
    io.write(0x414, uint32_t(segments[0].address >> 32));
    Publish("waiting-for-dma-address-ack");
    bool acknowledged = false;
    for (unsigned tick = 0; tick < 500; ++tick) {
        uint32_t phase = 0;
        if (!io.read(0x400, phase)) return kIOReturnNoDevice;
        if (phase & 1) { acknowledged = true; break; }
        IOSleep(1);
    }
    if (!acknowledged) { Publish("dma-address-ack-timeout"); return kIOReturnTimeout; }
    Publish("dma-address-ack-ok");
    Clarett::Session session(io, ivars->bytes, BufferSize);
    bool firmwareSeen = false;
    uint32_t applicationFirmware = 0, fpgaFirmware = 0;
    for (uint16_t sequence = 0; sequence < 6; ++sequence) {
        const auto request = (sequence % 2 == 0) ? Clarett::streamInfo(sequence)
                                                : Clarett::firmwareInfo(sequence);
        const auto result = session.transact(request);
        if (result != Clarett::Result::Success) {
            auto diagnostics = OSDictionary::withCapacity(10);
            if (diagnostics) {
                number(diagnostics, "ClarettRequestOpcode", request.opcode);
                number(diagnostics, "ClarettRequestSequence", request.sequence);
                number(diagnostics, "ClarettResponseEcho", Clarett::load32(ivars->bytes));
                number(diagnostics, "ClarettResponseLength", Clarett::load16(ivars->bytes + 4));
                number(diagnostics, "ClarettResponseSequence", Clarett::load16(ivars->bytes + 6));
                number(diagnostics, "ClarettResponseDeviceStatus", Clarett::load32(ivars->bytes + 8));
                number(diagnostics, "ClarettResponseValidation", uint64_t(session.lastResponse()));
                number(diagnostics, "ClarettObservedPhase", session.observedPhase());
                number(diagnostics, "ClarettObservedCause", session.observedCause());
                SetProperties(diagnostics);
                diagnostics->release();
            }
            Publish("mailbox-transaction-failed", uint64_t(result));
            return kIOReturnIOError;
        }
        const auto playback = Clarett::load16(ivars->bytes + 16);
        const auto capture = Clarett::load16(ivars->bytes + 18);
        const auto app = Clarett::load32(ivars->bytes + 16);
        const auto fpga = Clarett::load32(ivars->bytes + 20);
        if (request.opcode == Clarett::StreamInfo && (playback != 4 || capture != 14)) {
            Publish("unexpected-model-geometry");
            return kIOReturnUnsupported;
        }
        if (request.opcode == Clarett::GetData) {
            if (firmwareSeen && (applicationFirmware != app || fpgaFirmware != fpga)) {
                Publish("inconsistent-firmware-response");
                return kIOReturnIOError;
            }
            firmwareSeen = true;
            applicationFirmware = app;
            fpgaFirmware = fpga;
        }
        auto props = OSDictionary::withCapacity(4);
        if (props) {
            number(props, "ClarettCompletedTransactions", sequence + 1);
            if (request.opcode == Clarett::StreamInfo) {
                number(props, "ClarettTransportPlaybackSlots", playback);
                number(props, "ClarettTransportCaptureSlots", capture);
            } else {
                number(props, "ClarettApplicationFirmware", applicationFirmware);
                number(props, "ClarettFPGAFirmware", fpgaFirmware);
            }
            SetProperties(props);
            props->release();
        }
    }
    Publish("mailbox-six-transactions-ok");
    return kIOReturnSuccess;
}

void ClarettProbe::Quiesce() {
    if (!ivars || !ivars->pci) return;
    if (ivars->opened) {
        if (ivars->initialized) {
            ivars->pci->MemoryWrite32(ivars->bar, 0x104, 0);
        }
        if (ivars->busMaster) {
            // Disable new DMA and flush the config write before clearing addresses.
            commandBits(ivars->pci, 0, kIOPCICommandBusLead);
            ivars->busMaster = false;
        }
        if (ivars->initialized) {
            ivars->pci->MemoryWrite32(ivars->bar, 0x410, 0);
            ivars->pci->MemoryWrite32(ivars->bar, 0x414, 0);
            ivars->initialized = false;
        }
        // DriverKit Close also disables Bus Lead and Memory Space. It must
        // precede releasing any DMA mapping, including error/unplug paths.
        ivars->pci->Close(this, 0);
        ivars->opened = false;
    }
    if (ivars->mapped) { ivars->dma->CompleteDMA(0); ivars->mapped = false; }
    OSSafeReleaseNULL(ivars->dma);
    OSSafeReleaseNULL(ivars->memory);
    ivars->bytes = nullptr;
}

kern_return_t IMPL(ClarettProbe, Stop) {
    Quiesce();
    return Stop(provider, SUPERDISPATCH);
}
