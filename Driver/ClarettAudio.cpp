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
#include "ClarettAudio.h"
#include "Protocol.hpp"
#include "AudioRing.hpp"
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>

#define super IOUserAudioDriver
using namespace AudioDriverKit;
namespace A = Clarett::Audio;

struct ClarettAudio_IVars {
    IOPCIDevice *pci;
    IOBufferMemoryDescriptor *memory;
    IODMACommand *dma;
    volatile uint8_t *bytes;
    uint8_t bar;
    bool opened;
    bool mapped;
    bool busMaster;
    bool initialized;
    IOUserAudioDevice *device;
    IOUserAudioStream *input, *output;
    IOBufferMemoryDescriptor *inputMemory, *outputMemory, *audioMemory;
    IODMACommand *audioDMA;
    volatile uint8_t *audioBytes;
    int32_t *inputBytes, *outputBytes;
    uint64_t audioAddress;
    bool audioMapped;
    IOTimerDispatchSource *timer;
    OSAction *timerAction;
    uint64_t timerInterval, lastEvent, frames, events, overruns, readCallbacks, writeCallbacks;
    uint64_t inputNonzero, outputNonzero, callbacks, lastTimestamp;
    uint64_t lastOperation, lastCallbackFrames;
    uint8_t counter;
    uint16_t sequence;
    uint32_t running;
    bool failed;

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

bool ClarettAudio::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ClarettAudio_IVars, 1);
    return ivars != nullptr;
}

void ClarettAudio::free() {
    if (ivars) {
        ReleaseAudioObjects();
        Quiesce();
        IODelete(ivars, ClarettAudio_IVars, 1);
    }
    super::free();
}

void ClarettAudio::Publish(const char *state, uint64_t code) {
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

kern_return_t IMPL(ClarettAudio, Start) {
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
    IOService::SetName("ClarettProbe");
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
    ret = ProbeMailbox();
    if (ret == kIOReturnSuccess) ret = ConfigureAudio();
    if (ret != kIOReturnSuccess) {
        Publish("audio-initialization-failed", uint32_t(ret));
        ReleaseAudioObjects();
        Quiesce();
        return ret;
    }
    Publish("audio-device-ready");
    return RegisterService();
}

kern_return_t ClarettAudio::ProbeMailbox() {
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
    ivars->sequence = 6;
    Publish("mailbox-six-transactions-ok");
    return kIOReturnSuccess;
}

void ClarettAudio::Quiesce() {
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
    if (ivars->audioMapped) { ivars->audioDMA->CompleteDMA(0); ivars->audioMapped = false; }
    OSSafeReleaseNULL(ivars->audioDMA);
    OSSafeReleaseNULL(ivars->audioMemory);
    ivars->audioBytes = nullptr;
    if (ivars->mapped) { ivars->dma->CompleteDMA(0); ivars->mapped = false; }
    OSSafeReleaseNULL(ivars->dma);
    OSSafeReleaseNULL(ivars->memory);
    ivars->bytes = nullptr;
}

kern_return_t IMPL(ClarettAudio, Stop) {
    StopAudio();
    if (ivars->device) ivars->device->SetDeviceIsAlive(false);
    auto ret = Stop(provider, SUPERDISPATCH);
    // The framework stops its I/O threads before shared buffers are released.
    ReleaseAudioObjects();
    Quiesce();
    return ret;
}

void ClarettAudio::ReleaseAudioObjects() {
    if (ivars->device) ivars->device->SetIOOperationHandler(nullptr);
    if (ivars->timer) ivars->timer->Cancel(nullptr);
    OSSafeReleaseNULL(ivars->timer);
    OSSafeReleaseNULL(ivars->timerAction);
    OSSafeReleaseNULL(ivars->input);
    OSSafeReleaseNULL(ivars->output);
    OSSafeReleaseNULL(ivars->device);
    OSSafeReleaseNULL(ivars->inputMemory);
    OSSafeReleaseNULL(ivars->outputMemory);
}

namespace {
kern_return_t audioBuffer(uint64_t size, IOBufferMemoryDescriptor **descriptor, void **address) {
    auto ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, size, 16384, descriptor);
    if (ret != kIOReturnSuccess) return ret;
    ret = (*descriptor)->SetLength(size);
    if (ret != kIOReturnSuccess) return ret;
    IOAddressSegment cpu = {};
    ret = (*descriptor)->GetAddressRange(&cpu);
    if (ret != kIOReturnSuccess || cpu.length < size) return kIOReturnNoMemory;
    *address = reinterpret_cast<void *>(cpu.address);
    memset(*address, 0, size);
    return kIOReturnSuccess;
}
}

kern_return_t ClarettAudio::ConfigureAudio() {
    Publish("configuring-audio-device");
    void *address = nullptr;
    auto ret = audioBuffer(A::Bytes, &ivars->audioMemory, &address);
    if (ret != kIOReturnSuccess) return ret;
    ivars->audioBytes = static_cast<volatile uint8_t *>(address);
    IODMACommandSpecification spec = {};
    spec.maxAddressBits = 32;
    ret = IODMACommand::Create(ivars->pci, 0, &spec, &ivars->audioDMA);
    if (ret != kIOReturnSuccess) return ret;
    IOAddressSegment segments[32] = {};
    uint32_t count = 32;
    uint64_t flags = 0;
    ret = ivars->audioDMA->PrepareForDMA(0, ivars->audioMemory, 0, A::Bytes, &flags, &count, segments);
    if (ret != kIOReturnSuccess) return ret;
    ivars->audioMapped = true;
    if (count != 1 || segments[0].length < A::Bytes || (flags & kIOMemoryDirectionOutIn) != kIOMemoryDirectionOutIn ||
        !A::build(ivars->audioBytes, A::Bytes, segments[0].address)) return kIOReturnUnsupported;
    ivars->audioAddress = segments[0].address;

    ret = audioBuffer(A::Frames * A::RxChannels * 4, &ivars->inputMemory, &address);
    if (ret != kIOReturnSuccess) return ret;
    ivars->inputBytes = static_cast<int32_t *>(address);
    ret = audioBuffer(A::Frames * A::TxChannels * 4, &ivars->outputMemory, &address);
    if (ret != kIOReturnSuccess) return ret;
    ivars->outputBytes = static_cast<int32_t *>(address);

    auto uid = OSSharedPtr(OSString::withCString("org.codaco.clarett.2pre.experimental"), OSNoRetain);
    auto model = OSSharedPtr(OSString::withCString("Clarett2PreThunderbolt"), OSNoRetain);
    auto manufacturer = OSSharedPtr(OSString::withCString("Focusrite / experimental driver"), OSNoRetain);
    auto name = OSSharedPtr(OSString::withCString("Clarett 2Pre Thunderbolt (Experimental)"), OSNoRetain);
    auto device = IOUserAudioDevice::Create(this, false, uid.get(), model.get(), manufacturer.get(), A::Frames);
    if (!device) return kIOReturnNoMemory;
    ivars->device = device.get(); ivars->device->retain();
    device->SetName(name.get());
    SetName(name.get());
    device->SetTransportType(IOUserAudioTransportType::Thunderbolt);
    double rate = A::Rate;
    device->SetAvailableSampleRates(&rate, 1);
    device->SetSampleRate(rate);
    device->SetClockAlgorithm(IOUserAudioClockAlgorithm::SimpleIIR);
    device->SetClockDomain(0);
    device->SetInputSafetyOffset(512);
    device->SetOutputSafetyOffset(512);
    device->SetInputLatency(32);
    device->SetOutputLatency(32);
    device->SetCanBeDefaultInputDevice(true);
    device->SetCanBeDefaultOutputDevice(true);
    device->SetCanBeDefaultSystemOutputDevice(false);

    auto input = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Input, ivars->inputMemory);
    auto output = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Output, ivars->outputMemory);
    if (!input || !output) return kIOReturnNoMemory;
    ivars->input = input.get(); input->retain();
    ivars->output = output.get(); output->retain();
    for (unsigned direction = 0; direction < 2; ++direction) {
        auto stream = direction == 0 ? input.get() : output.get();
        const auto channels = direction == 0 ? A::RxChannels : A::TxChannels;
        IOUserAudioStreamBasicDescription format = { double(A::Rate), IOUserAudioFormatID::LinearPCM,
            static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger |
                IOUserAudioFormatFlags::FormatFlagIsPacked | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
            channels * 4, 1, channels * 4, channels, 32 };
        ret = stream->SetAvailableStreamFormats(&format, 1);
        if (ret != kIOReturnSuccess) return ret;
        ret = stream->SetCurrentStreamFormat(&format);
        if (ret != kIOReturnSuccess) return ret;
        ret = device->AddStream(stream);
        if (ret != kIOReturnSuccess) return ret;
    }
    // The HAL operation callback copies between its packed ring and page-contained
    // hardware fragments. No MMIO, allocation, locks or mailbox calls occur here.
    ret = device->SetIOOperationHandler(^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation operation,
                                                     uint32_t frames, uint64_t sample, uint64_t) {
        __atomic_fetch_add(&ivars->callbacks, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&ivars->lastOperation, operation, __ATOMIC_RELAXED);
        __atomic_store_n(&ivars->lastCallbackFrames, frames, __ATOMIC_RELAXED);
        if (frames > A::Frames) return kIOReturnBadArgument;
        if (!__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        uint64_t nonzero = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (operation == IOUserAudioIOOperationWriteEnd) {
            for (uint32_t f = 0; f < frames; ++f) {
                const auto pos = (sample + f) % A::Frames;
                for (uint32_t ch = 0; ch < A::TxChannels; ++ch) {
                    auto value = ivars->outputBytes[pos * A::TxChannels + ch];
                    auto p = reinterpret_cast<volatile uint32_t *>(ivars->audioBytes + A::sampleOffset(false, sample + f, ch));
                    *p = uint32_t(value) & 0xffffff00u;
                    nonzero += value != 0;
                }
            }
            __atomic_fetch_add(&ivars->writeCallbacks, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&ivars->outputNonzero, nonzero, __ATOMIC_RELAXED);
        } else if (operation == IOUserAudioIOOperationBeginRead) {
            for (uint32_t f = 0; f < frames; ++f) {
                const auto pos = (sample + f) % A::Frames;
                for (uint32_t ch = 0; ch < A::RxChannels; ++ch) {
                    const auto p = reinterpret_cast<volatile uint32_t *>(ivars->audioBytes + A::sampleOffset(true, sample + f, ch));
                    const auto value = int32_t(*p & 0xffffff00u);
                    ivars->inputBytes[pos * A::RxChannels + ch] = value;
                    nonzero += value != 0;
                }
            }
            __atomic_fetch_add(&ivars->readCallbacks, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&ivars->inputNonzero, nonzero, __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return kIOReturnSuccess;
    });
    if (ret != kIOReturnSuccess) return ret;
    auto queue = GetWorkQueue();
    ret = IOTimerDispatchSource::Create(queue.get(), &ivars->timer);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionTick(0, &ivars->timerAction);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->timer->SetHandler(ivars->timerAction);
    if (ret != kIOReturnSuccess) return ret;
    struct mach_timebase_info timebase = {};
    mach_timebase_info(&timebase);
    ivars->timerInterval = 500000ull * timebase.denom / timebase.numer;
    ret = AddObject(device.get());
    if (ret != kIOReturnSuccess) return ret;
    auto props = OSDictionary::withCapacity(4);
    if (!props) return kIOReturnNoMemory;
    number(props, "ClarettAudioSampleRate", A::Rate);
    number(props, "ClarettAudioRingFrames", A::Frames);
    number(props, "ClarettAudioPlaybackChannels", A::TxChannels);
    number(props, "ClarettAudioCaptureChannels", A::RxChannels);
    SetProperties(props); props->release();
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::Command(uint32_t opcode) {
    if (ivars->failed) return kIOReturnNotReady;
    PCITransport io(ivars->pci, ivars->bar);
    Clarett::Session session(io, ivars->bytes, BufferSize, true);
    const auto request = opcode == 0x006003 ? Clarett::audioClock(ivars->sequence) : Clarett::audioSync(opcode, ivars->sequence);
    const auto result = session.transact(request);
    if (result != Clarett::Result::Success) {
        ivars->failed = true;
        auto props = OSDictionary::withCapacity(5);
        if (props) {
        number(props, "ClarettFailedOpcode", opcode);
        number(props, "ClarettResponseLength", Clarett::load16(ivars->bytes + 4));
        number(props, "ClarettResponseDeviceStatus", Clarett::load32(ivars->bytes + 8));
        number(props, "ClarettResponseSequence", Clarett::load16(ivars->bytes + 6));
        number(props, "ClarettRequestSequence", request.sequence);
        SetProperties(props); props->release();
        }
        Publish("audio-clock-command-failed", uint64_t(result));
        return kIOReturnIOError;
    }
    ++ivars->sequence;
    if ((opcode == 0x006005 && Clarett::load32(ivars->bytes + 16) != A::Rate) ||
        (opcode == 0x006004 && Clarett::load32(ivars->bytes + 16) == 0)) return kIOReturnNotReady;
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::StartAudio() {
    if (__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) return kIOReturnSuccess;
    if (ivars->failed || !ivars->opened) return kIOReturnNotReady;
    const uint32_t opcodes[] = {0x006003u, 0x006004u, 0x006002u, 0x006005u};
    for (auto opcode : opcodes) {
        const auto ret = Command(opcode);
        if (ret != kIOReturnSuccess) return ret;
    }
    if (!A::build(ivars->audioBytes, A::Bytes, ivars->audioAddress)) return kIOReturnUnsupported;
    memset(ivars->inputBytes, 0, A::Frames * A::RxChannels * 4);
    memset(ivars->outputBytes, 0, A::Frames * A::TxChannels * 4);
    ivars->frames = ivars->events = ivars->overruns = ivars->lastTimestamp = 0;
    ivars->counter = 0;
    ivars->lastEvent = mach_absolute_time();
    ivars->device->UpdateCurrentZeroTimestamp(0, ivars->lastEvent);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    PCITransport io(ivars->pci, ivars->bar);
    io.write(0x100, 0xf);
    io.write(0x108, 0x10);
    // Reference engine sequence: global enable precedes geometry; final arm
    // remains last, after both descriptor bases have been programmed.
    io.write(0x20c, 1);
    io.write(0x204, A::TxChannels);
    io.write(0x208, A::TxChannels * 16);
    io.write(0x214, uint32_t(ivars->audioAddress >> 32));
    io.write(0x210, uint32_t(ivars->audioAddress));
    const auto rx = ivars->audioAddress + A::RxTable;
    io.write(0x304, A::RxChannels);
    io.write(0x308, A::RxChannels * 16);
    io.write(0x314, uint32_t(rx >> 32));
    io.write(0x310, uint32_t(rx));
    io.write(0x10c, 0x1e70700);
    __atomic_store_n(&ivars->running, 1, __ATOMIC_RELEASE);
    io.write(0x110, 7);
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + ivars->timerInterval, 0);
    ivars->timer->SetEnable(true);
    Publish("audio-streaming");
    return kIOReturnSuccess;
}

void ClarettAudio::StopAudio() {
    __atomic_store_n(&ivars->running, 0, __ATOMIC_RELEASE);
    if (ivars->timer) ivars->timer->SetEnable(false);
    if (ivars->opened) {
        PCITransport io(ivars->pci, ivars->bar);
        io.write(0x20c, 0); io.write(0x30c, 0);
        io.write(0x110, 0); io.write(0x108, 0);
        uint32_t flush = 0; io.read(0x20c, flush);
    }
}

kern_return_t ClarettAudio::StartDevice(IOUserAudioObjectID object, IOUserAudioStartStopFlags flags) {
    if (!ivars->device || object != ivars->device->GetObjectID()) return kIOReturnBadArgument;
    __block kern_return_t ret = kIOReturnNotReady;
    GetWorkQueue()->DispatchSync(^{
        ret = StartAudio();
        if (ret == kIOReturnSuccess) {
            ret = super::StartDevice(object, flags);
            if (ret != kIOReturnSuccess) StopAudio();
        }
    });
    return ret;
}

kern_return_t ClarettAudio::StopDevice(IOUserAudioObjectID object, IOUserAudioStartStopFlags flags) {
    if (!ivars->device || object != ivars->device->GetObjectID()) return kIOReturnBadArgument;
    __block kern_return_t ret;
    GetWorkQueue()->DispatchSync(^{
        StopAudio();
        ret = super::StopDevice(object, flags);
        Publish("audio-device-ready");
    });
    return ret;
}

void IMPL(ClarettAudio, Tick) {
    if (!__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) return;
    const auto now = mach_absolute_time();
    PCITransport io(ivars->pci, ivars->bar);
    uint32_t unused = 0, cause = 0;
    if (!io.read(0x100, unused) || !io.read(0x300, cause) || !io.read(0x200, unused) ||
        !io.read(0x400, unused) || !io.read(0x500, unused)) {
        StopAudio();
        ivars->device->SetDeviceIsAlive(false);
        Publish("audio-device-disconnected"); return;
    }
    if (now - ivars->lastEvent > ivars->timerInterval * 150) {
        StopAudio();
        ivars->device->SetDeviceIsAlive(false);
        Publish("audio-clock-stalled"); return;
    }
    uint32_t delta = 0;
    if (A::counterDelta(cause, ivars->counter, delta)) {
        ivars->counter = uint8_t(cause);
        // Retire consumed playback samples so an input-only client cannot
        // cause the last output block to repeat on each ring wrap.
        for (uint32_t f = 0; f < delta; ++f)
            for (uint32_t ch = 0; ch < A::TxChannels; ++ch)
                *reinterpret_cast<volatile uint32_t *>(ivars->audioBytes + A::sampleOffset(false, ivars->frames + f, ch)) = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        ivars->frames += delta;
        ++ivars->events;
        ivars->overruns += (cause & 0x40000000) != 0;
        ivars->lastEvent = now;
        const auto timestampFrame = A::timestampFrame(ivars->frames);
        if (timestampFrame > ivars->lastTimestamp) {
            // The HAL treats the zero-timestamp period as its ring-buffer size.
            // Publish one timestamp per complete shared ring, not per 256-frame
            // hardware IRQ. Interpolate a late poll back to the ring boundary.
            const auto framesPastBoundary = ivars->frames - timestampFrame;
            const auto ticksPastBoundary = framesPastBoundary * ivars->timerInterval * 2000 / A::Rate;
            ivars->device->UpdateCurrentZeroTimestamp(timestampFrame, now - ticksPastBoundary);
            ivars->lastTimestamp = timestampFrame;
        }
        if (ivars->events % 188 == 0) {
            auto props = OSDictionary::withCapacity(7);
            if (props) {
            number(props, "ClarettAudioCallbacks", __atomic_load_n(&ivars->callbacks, __ATOMIC_RELAXED));
            number(props, "ClarettAudioLastOperation", __atomic_load_n(&ivars->lastOperation, __ATOMIC_RELAXED));
            number(props, "ClarettAudioLastCallbackFrames", __atomic_load_n(&ivars->lastCallbackFrames, __ATOMIC_RELAXED));
            number(props, "ClarettAudioFrames", ivars->frames);
            number(props, "ClarettAudioEvents", ivars->events);
            number(props, "ClarettAudioOverruns", ivars->overruns);
            number(props, "ClarettAudioReadCallbacks", __atomic_load_n(&ivars->readCallbacks, __ATOMIC_RELAXED));
            number(props, "ClarettAudioWriteCallbacks", __atomic_load_n(&ivars->writeCallbacks, __ATOMIC_RELAXED));
            number(props, "ClarettAudioInputNonzero", __atomic_load_n(&ivars->inputNonzero, __ATOMIC_RELAXED));
            number(props, "ClarettAudioOutputNonzero", __atomic_load_n(&ivars->outputNonzero, __ATOMIC_RELAXED));
            SetProperties(props); props->release();
            }
        }
    }
    ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + ivars->timerInterval, 0);
}
