// SPDX-License-Identifier: GPL-2.0-only
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSData.h>
#include <PCIDriverKit/PCIDriverKit.h>
#include <os/log.h>
#include "ClarettAudio.h"
#include "ClarettSettings.h"
#include "ClarettDevice.h"
#include "ClarettVolume.h"
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
    uint64_t timerInterval, ticksPerSecond, lastEvent, frames, events, overruns, readCallbacks, writeCallbacks;
    uint64_t inputNonzero, outputNonzero, callbacks, lastTimestamp;
    uint64_t lastOperation, lastCallbackFrames;
    uint64_t confirmedFrames, inputAheadCallbacks, inputAheadFrames, outputLateCallbacks;
    uint64_t pollGapMax, readTicksMax, eventGapMax, latePolls;
    uint64_t lastPoll;
    uint8_t counter;
    uint16_t sequence;
    uint32_t running;
    bool failed;
    IODispatchQueue *controlQueue, *periodQueue;
    ClarettSettings *settings;
    ClarettSettings *timingSettings, *volumeSettings;
    ClarettVolume *volumeControl;
    Clarett::Volume::State volume;
    uint32_t volumeEnabled, volumePending;
    uint64_t volumeRevision;
    uint32_t safetyFrames, periodFrames, timingPending, desiredSafety;
    uint32_t sampleRate, liveCapture[2];
    uint32_t mailboxBusy, closing, controlPending;
    bool monitorEnabled, monitorDirty;
    Clarett::Monitor::Plan monitor;


};

namespace {
constexpr uint32_t BufferSize = 16384;

void number(OSDictionary *dict, const char *key, uint64_t value) {
    auto n = OSNumber::withNumber(value, 64);
    if (n) { dict->setObject(key, n); n->release(); }
}

void atomicMax(uint64_t *target, uint64_t value) {
    auto previous = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > previous && !__atomic_compare_exchange_n(target, &previous, value, true,
                                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
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
        OSSafeReleaseNULL(ivars->controlQueue);
        OSSafeReleaseNULL(ivars->periodQueue);
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
    // Double-speed changes the live capture tail, not the DMA frame stride.
    const auto highBand = Clarett::streamInfo(6, 1);
    if (session.transact(highBand) != Clarett::Result::Success) return kIOReturnIOError;
    if (Clarett::load16(ivars->bytes + 16) != A::TxChannels ||
        Clarett::load16(ivars->bytes + 18) != 10) {
        Publish("unexpected-double-speed-geometry"); return kIOReturnUnsupported;
    }
    ivars->liveCapture[0] = A::RxChannels;
    ivars->liveCapture[1] = 10;
    ivars->sequence = 7;
    auto geometry = OSDictionary::withCapacity(3);
    if (geometry) {
        number(geometry, "ClarettCompletedTransactions", 7);
        number(geometry, "ClarettCaptureSingleSpeed", A::RxChannels);
        number(geometry, "ClarettCaptureDoubleSpeed", 10);
        SetProperties(geometry); geometry->release();
    }
    Publish("mailbox-rate-bands-verified");
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
    __atomic_store_n(&ivars->closing, 1, __ATOMIC_RELEASE);
    // A framework rate callback cannot synchronously enter controlQueue (the
    // device's setter queue may already be held). Let its bounded exchange
    // retire before disabling PCI or releasing shared DMA memory.
    while (__atomic_load_n(&ivars->timingPending, __ATOMIC_ACQUIRE) == 2) IOSleep(1);
    StopAudio();
    if (ivars->periodQueue) ivars->periodQueue->DispatchSync(^{});
    if (ivars->controlQueue) ivars->controlQueue->DispatchSync(^{
        if (ivars->monitorEnabled && !ivars->failed) DisableMonitor();
    });
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
    OSSafeReleaseNULL(ivars->settings);
    OSSafeReleaseNULL(ivars->timingSettings);
    OSSafeReleaseNULL(ivars->volumeSettings);
    OSSafeReleaseNULL(ivars->volumeControl);
    // Queues remain alive until free: an in-flight property setter may enqueue
    // a closing-only completion after Stop has drained earlier requests.
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
    ivars->sampleRate = A::DefaultRate;
    ivars->safetyFrames = A::Safety;
    ivars->periodFrames = A::Period;
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
    auto device = OSSharedPtr(OSTypeAlloc(ClarettDevice), OSNoRetain);
    if (!device || !device->Configure(this, uid.get(), model.get(), manufacturer.get(), A::Frames)) return kIOReturnNoMemory;
    ivars->device = device.get(); ivars->device->retain();
    device->SetName(name.get());
    SetName(name.get());
    device->SetTransportType(IOUserAudioTransportType::Thunderbolt);
    double rates[A::RateCount] = {};
    for (size_t i = 0; i < A::RateCount; ++i) rates[i] = A::Rates[i];
    ret = device->SetAvailableSampleRates(rates, A::RateCount);
    if (ret != kIOReturnSuccess) return ret;
    ret = device->SetSampleRate(A::DefaultRate);
    if (ret != kIOReturnSuccess) return ret;
    device->SetClockAlgorithm(IOUserAudioClockAlgorithm::SimpleIIR);
    device->SetClockDomain(0);
    device->SetInputSafetyOffset(A::Safety);
    device->SetOutputSafetyOffset(A::Safety);
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
        IOUserAudioStreamBasicDescription format = { double(A::DefaultRate), IOUserAudioFormatID::LinearPCM,
            static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger |
                IOUserAudioFormatFlags::FormatFlagIsPacked | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
            channels * 4, 1, channels * 4, channels, 32 };
        IOUserAudioStreamBasicDescription formats[A::RateCount] = {};
        for (size_t i = 0; i < A::RateCount; ++i) {
            formats[i] = format; formats[i].mSampleRate = A::Rates[i];
        }
        ret = stream->SetAvailableStreamFormats(formats, A::RateCount);
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
        const auto completed = __atomic_load_n(&ivars->confirmedFrames, __ATOMIC_ACQUIRE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (operation == IOUserAudioIOOperationWriteEnd) {
            if (int64_t(sample - completed) < 0)
                __atomic_fetch_add(&ivars->outputLateCallbacks, 1, __ATOMIC_RELAXED);
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
            const auto rate = __atomic_load_n(&ivars->sampleRate, __ATOMIC_ACQUIRE);
            const auto liveChannels = ivars->liveCapture[A::speedBand(rate)];
            const auto ahead = int64_t(sample + frames - completed);
            if (ahead > 0) {
                __atomic_fetch_add(&ivars->inputAheadCallbacks, 1, __ATOMIC_RELAXED);
                atomicMax(&ivars->inputAheadFrames, uint64_t(ahead));
            }
            for (uint32_t f = 0; f < frames; ++f) {
                const auto pos = (sample + f) % A::Frames;
                for (uint32_t ch = 0; ch < A::RxChannels; ++ch) {
                    const auto p = reinterpret_cast<volatile uint32_t *>(ivars->audioBytes + A::sampleOffset(true, sample + f, ch));
                    const auto value = A::captureSample(*p, ch, liveChannels);
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
    ret = IODispatchQueue::Create("ClarettControls", 0, 0, &ivars->controlQueue);
    if (ret != kIOReturnSuccess) return ret;
    ret = IODispatchQueue::Create("ClarettPeriods", 0, 0, &ivars->periodQueue);
    if (ret != kIOReturnSuccess) return ret;
    ret = IOTimerDispatchSource::Create(ivars->periodQueue, &ivars->timer);
    if (ret != kIOReturnSuccess) return ret;
    ret = CreateActionTick(0, &ivars->timerAction);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->timer->SetHandler(ivars->timerAction);
    if (ret != kIOReturnSuccess) return ret;
    struct mach_timebase_info timebase = {};
    mach_timebase_info(&timebase);
    ivars->ticksPerSecond = 1000000000ull * timebase.denom / timebase.numer;
    ivars->timerInterval = A::pollTicks(ivars->ticksPerSecond, ivars->sampleRate, ivars->safetyFrames);
    ivars->settings = OSTypeAlloc(ClarettSettings);
    if (!ivars->settings || !ivars->settings->Configure(this)) return kIOReturnNoMemory;
    ret = device->AddCustomProperty(ivars->settings);
    if (ret != kIOReturnSuccess) return ret;
    ivars->timingSettings = OSTypeAlloc(ClarettSettings);
    if (!ivars->timingSettings || !ivars->timingSettings->Configure(this, true)) return kIOReturnNoMemory;
    ret = device->AddCustomProperty(ivars->timingSettings);
    if (ret != kIOReturnSuccess) return ret;
    ivars->volume = Clarett::Volume::State{};
    ivars->volumeSettings = OSTypeAlloc(ClarettSettings);
    if (!ivars->volumeSettings || !ivars->volumeSettings->Configure(this, 2)) return kIOReturnNoMemory;
    ret = device->AddCustomProperty(ivars->volumeSettings);
    if (ret != kIOReturnSuccess) return ret;
    ivars->volumeControl = OSTypeAlloc(ClarettVolume);
    if (!ivars->volumeControl || !ivars->volumeControl->Configure(this)) return kIOReturnNoMemory;
    ret = device->AddControl(ivars->volumeControl);
    if (ret != kIOReturnSuccess) return ret;
    VolumeStatus("unchanged");
    MonitorStatus("off");
    TimingStatus("stable");
    ret = ChangeClock(ivars->sampleRate);
    if (ret != kIOReturnSuccess) return ret;
    RateStatus("stable", ivars->sampleRate);
    ret = AddObject(device.get());
    if (ret != kIOReturnSuccess) return ret;
    auto props = OSDictionary::withCapacity(4);
    if (!props) return kIOReturnNoMemory;
    number(props, "ClarettAudioSampleRate", ivars->sampleRate);
    number(props, "ClarettAudioRingFrames", A::Frames);
    number(props, "ClarettAudioPeriodFrames", A::Period);
    number(props, "ClarettAudioSafetyFrames", A::Safety);
    number(props, "ClarettAudioPlaybackChannels", A::TxChannels);
    number(props, "ClarettAudioCaptureChannels", A::RxChannels);
    SetProperties(props); props->release();
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::Command(uint32_t opcode, uint32_t rate) {
    const auto request = opcode == 0x006003 ? Clarett::audioClock(0, rate) : Clarett::audioSync(opcode, 0);
    uint8_t response[4] = {};
    const auto ret = Exchange(opcode, request.payload, request.length, request.expectedLength,
                              response, request.expectedLength >= 4 ? 4 : 0);
    if (ret != kIOReturnSuccess) return ret;
    if ((opcode == 0x006005 && Clarett::load32(response) != rate) ||
        (opcode == 0x006004 && (Clarett::load32(response) & 1) == 0)) return kIOReturnNotReady;
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::ChangeClock(uint32_t rate) {
    if (!A::rateValid(rate)) return kIOReturnBadArgument;
    auto ret = Command(0x006003, rate);
    if (ret != kIOReturnSuccess) return ret;
    // Preserve the reference SET_CLOCK / sync / commit / rate handshake.
    ret = Command(0x006004, rate);
    if (ret != kIOReturnSuccess && ret != kIOReturnNotReady) return ret;
    ret = Command(0x006002, rate);
    if (ret != kIOReturnSuccess) return ret;
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        if (__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnAborted;
        const auto locked = Command(0x006004, rate);
        if (locked != kIOReturnSuccess && locked != kIOReturnNotReady) return locked;
        const auto readback = Command(0x006005, rate);
        if (readback != kIOReturnSuccess && readback != kIOReturnNotReady) return readback;
        if (locked == kIOReturnSuccess && readback == kIOReturnSuccess) {
            auto props = OSDictionary::withCapacity(1);
            if (props) {
                number(props, "ClarettHardwareSampleRate", rate);
                SetProperties(props); props->release();
            }
            return kIOReturnSuccess;
        }
        IOSleep(5); // allow the PLL to settle; never run in the realtime callback
    }
    return kIOReturnTimeout;
}

kern_return_t ClarettAudio::StartAudio() {
    if (__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) return kIOReturnSuccess;
    if (ivars->failed || !ivars->opened || __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    const auto rate = __atomic_load_n(&ivars->sampleRate, __ATOMIC_ACQUIRE);
    const auto clockResult = ChangeClock(rate);
    if (clockResult != kIOReturnSuccess) return clockResult;
    if (!A::build(ivars->audioBytes, A::Bytes, ivars->audioAddress, ivars->periodFrames)) return kIOReturnUnsupported;
    memset(ivars->inputBytes, 0, A::Frames * A::RxChannels * 4);
    memset(ivars->outputBytes, 0, A::Frames * A::TxChannels * 4);
    ivars->frames = ivars->events = ivars->overruns = ivars->lastTimestamp = 0;
    __atomic_store_n(&ivars->confirmedFrames, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ivars->inputAheadCallbacks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->inputAheadFrames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->outputLateCallbacks, 0, __ATOMIC_RELAXED);
    ivars->pollGapMax = ivars->readTicksMax = ivars->eventGapMax = ivars->latePolls = 0;
    ivars->counter = 0;
    ivars->lastEvent = mach_absolute_time();
    ivars->lastPoll = ivars->lastEvent;
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
    ivars->controlQueue->DispatchSync(^{
        ivars->periodQueue->DispatchSync(^{});
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
    ivars->controlQueue->DispatchSync(^{
        StopAudio();
        ivars->periodQueue->DispatchSync(^{});
        ret = super::StopDevice(object, flags);
        Publish("audio-device-ready");
    });
    return ret;
}

void IMPL(ClarettAudio, Tick) {
    if (!__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) return;
    const auto now = mach_absolute_time();
    const auto gap = now - ivars->lastPoll;
    if (gap > ivars->pollGapMax) ivars->pollGapMax = gap;
    ivars->latePolls += gap > A::ticksForFrames(ivars->ticksPerSecond, ivars->periodFrames, ivars->sampleRate);
    ivars->lastPoll = now;
    PCITransport io(ivars->pci, ivars->bar);
    uint32_t unused = 0, cause = 0;
    const bool mailbox = __atomic_load_n(&ivars->mailboxBusy, __ATOMIC_ACQUIRE) != 0;
    const bool connected = (mailbox || io.read(0x100, unused)) && io.read(0x300, cause) && io.read(0x200, unused) &&
        (mailbox || (io.read(0x400, unused) && io.read(0x500, unused)));
    const auto readTicks = mach_absolute_time() - now;
    if (readTicks > ivars->readTicksMax) ivars->readTicksMax = readTicks;
    if (!connected) {
        StopAudio();
        ivars->controlQueue->DispatchAsync(^{
            if (!__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) ivars->device->SetDeviceIsAlive(false);
        });
        Publish("audio-device-disconnected"); return;
    }
    if (now - ivars->lastEvent > A::ticksForFrames(ivars->ticksPerSecond, A::Frames * 7 / 8, ivars->sampleRate)) {
        StopAudio();
        ivars->controlQueue->DispatchAsync(^{
            if (!__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) ivars->device->SetDeviceIsAlive(false);
        });
        Publish("audio-clock-stalled"); return;
    }
    uint32_t delta = 0;
    if (A::counterDelta(cause, ivars->counter, delta)) {
        const auto eventGap = now - ivars->lastEvent;
        if (eventGap > ivars->eventGapMax) ivars->eventGapMax = eventGap;
        ivars->counter = uint8_t(cause);
        // Retire consumed playback samples so an input-only client cannot
        // cause the last output block to repeat on each ring wrap.
        for (uint32_t f = 0; f < delta; ++f)
            for (uint32_t ch = 0; ch < A::TxChannels; ++ch)
                *reinterpret_cast<volatile uint32_t *>(ivars->audioBytes + A::sampleOffset(false, ivars->frames + f, ch)) = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        ivars->frames += delta;
        __atomic_store_n(&ivars->confirmedFrames, ivars->frames, __ATOMIC_RELEASE);
        ++ivars->events;
        ivars->overruns += (cause & 0x40000000) != 0;
        ivars->lastEvent = now;
        const auto timestampFrame = A::timestampFrame(ivars->frames);
        if (timestampFrame > ivars->lastTimestamp) {
            // The HAL treats the zero-timestamp period as its ring-buffer size.
            // Publish one timestamp per complete shared ring, not per hardware
            // hardware IRQ. Interpolate a late poll back to the ring boundary.
            const auto framesPastBoundary = ivars->frames - timestampFrame;
            const auto ticksPastBoundary = A::ticksForFrames(ivars->ticksPerSecond, framesPastBoundary, ivars->sampleRate);
            ivars->device->UpdateCurrentZeroTimestamp(timestampFrame, now - ticksPastBoundary);
            ivars->lastTimestamp = timestampFrame;
        }
        if (ivars->events % (ivars->sampleRate / ivars->periodFrames) == 0) {
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
            number(props, "ClarettAudioInputAheadCallbacks", __atomic_load_n(&ivars->inputAheadCallbacks, __ATOMIC_RELAXED));
            number(props, "ClarettAudioInputAheadFramesMax", __atomic_load_n(&ivars->inputAheadFrames, __ATOMIC_RELAXED));
            number(props, "ClarettAudioOutputLateCallbacks", __atomic_load_n(&ivars->outputLateCallbacks, __ATOMIC_RELAXED));
            number(props, "ClarettAudioPollGapMaxUs", ivars->pollGapMax * 1000000 / ivars->ticksPerSecond);
            number(props, "ClarettAudioReadMaxUs", ivars->readTicksMax * 1000000 / ivars->ticksPerSecond);
            number(props, "ClarettAudioEventGapMaxUs", ivars->eventGapMax * 1000000 / ivars->ticksPerSecond);
            number(props, "ClarettAudioLatePolls", ivars->latePolls);
            SetProperties(props); props->release();
            }
        }
    }
    if (__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE))
        ivars->timer->WakeAtTime(kIOTimerClockMachAbsoluteTime, mach_absolute_time() + ivars->timerInterval, 0);
}

kern_return_t ClarettAudio::Exchange(uint32_t opcode, const uint8_t *payload, uint16_t length,
                                     uint16_t minimum, uint8_t *response, uint16_t responseSize) {
    if (ivars->failed || !ivars->opened) return kIOReturnNotReady;
    Clarett::Request request = {opcode, ivars->sequence, length, minimum, {}};
    if (length > sizeof(request.payload) || responseSize > minimum || (length && !payload)) return kIOReturnBadArgument;
    if (length) memcpy(request.payload, payload, length);
    if (!Clarett::permitted(request, true, true)) return kIOReturnNotPermitted;
    // Transfer cause-register ownership before submitting. Drain any period
    // callback which observed the old flag; subsequent callbacks touch only
    // 0x300/0x200, while mailbox polling owns 0x100/0x400/0x500.
    __atomic_store_n(&ivars->mailboxBusy, 1, __ATOMIC_RELEASE);
    ivars->periodQueue->DispatchSync(^{});
    PCITransport io(ivars->pci, ivars->bar);
    Clarett::Session session(io, ivars->bytes, BufferSize, true, true);
    const auto result = session.transact(request);
    if (result == Clarett::Result::Success) {
        ++ivars->sequence;
        if (response && responseSize)
            for (unsigned i = 0; i < responseSize; ++i) response[i] = ivars->bytes[16 + i];
    } else {
        ivars->failed = true;
        auto props = OSDictionary::withCapacity(7);
        if (props) {
            number(props, "ClarettFailedOpcode", opcode);
            number(props, "ClarettResponseLength", Clarett::load16(ivars->bytes + 4));
            number(props, "ClarettResponseDeviceStatus", Clarett::load32(ivars->bytes + 8));
            number(props, "ClarettResponseSequence", Clarett::load16(ivars->bytes + 6));
            number(props, "ClarettRequestSequence", request.sequence);
            number(props, "ClarettResponseEcho", Clarett::load32(ivars->bytes));
            number(props, "ClarettResponseValidation", uint64_t(session.lastResponse()));
            SetProperties(props); props->release();
        }
    }
    __atomic_store_n(&ivars->mailboxBusy, 0, __ATOMIC_RELEASE);
    return result == Clarett::Result::Success ? kIOReturnSuccess : kIOReturnIOError;
}

kern_return_t ClarettAudio::ReadRoutes(uint32_t *entries) {
    const auto band = A::speedBand(ivars->sampleRate);
    const auto routes = Clarett::Monitor::routeCount(band);
    for (unsigned offset = 0; offset < routes; offset += 28) {
        const auto count = uint8_t(routes - offset < 28 ? routes - offset : 28);
        const auto request = Clarett::monitorMuxRead(0, uint8_t(offset), count, band);
        uint8_t reply[112] = {};
        auto ret = Exchange(request.opcode, request.payload, request.length, request.expectedLength, reply, request.expectedLength);
        if (ret != kIOReturnSuccess) return ret;
        for (unsigned i = 0; i < count; ++i) entries[offset + i] = Clarett::load32(reply + i * 4);
    }
    if (!Clarett::Monitor::routesValid(entries, band)) {
        auto props = OSDictionary::withCapacity(1);
        auto data = OSData::withBytes(entries, Clarett::Monitor::Routes * sizeof(uint32_t));
        if (props && data) { props->setObject("ClarettMonitorUnexpectedRoutes", data); SetProperties(props); }
        OSSafeReleaseNULL(data); OSSafeReleaseNULL(props);
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}
kern_return_t ClarettAudio::ReadRow(uint32_t bus, uint16_t *row) {
    auto request = Clarett::monitorMixRead(0, uint16_t(bus));
    uint8_t reply[60] = {};
    const auto ret = Exchange(request.opcode, request.payload, request.length, request.expectedLength, reply, 60);
    if (ret == kIOReturnSuccess)
        for (unsigned i = 0; i < Clarett::Monitor::Inputs; ++i) row[i] = Clarett::load16(reply + i * 2);
    return ret;
}
kern_return_t ClarettAudio::WriteRoutes(const uint32_t *entries) {
    const auto request = Clarett::monitorMuxWrite(0, entries, A::speedBand(ivars->sampleRate));
    return Exchange(request.opcode, request.payload, request.length, 0, nullptr, 0);
}
kern_return_t ClarettAudio::WriteRow(uint32_t bus, const uint16_t *row) {
    const auto request = Clarett::monitorMixWrite(0, bus, row);
    return Exchange(request.opcode, request.payload, request.length, 0, nullptr, 0);
}

kern_return_t ClarettAudio::VerifyMonitor() {
    uint32_t routes[Clarett::Monitor::Routes] = {};
    auto ret = ReadRoutes(routes);
    if (ret != kIOReturnSuccess) return ret;
    if (!Clarett::Monitor::sameRoutes(routes, ivars->monitor.active, ivars->monitor.before.band)) return kIOReturnExclusiveAccess;
    for (unsigned b = 0; b < 2; ++b) {
        uint16_t row[Clarett::Monitor::Inputs] = {};
        ret = ReadRow(ivars->monitor.buses[b], row);
        if (ret != kIOReturnSuccess) return ret;
        if (memcmp(row, ivars->monitor.rows[b], sizeof(row))) return kIOReturnExclusiveAccess;
    }
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::DisableMonitor() {
    if (!ivars->monitorEnabled && !ivars->monitorDirty) return kIOReturnSuccess;
    if (!ivars->monitorDirty) {
        const auto ret = VerifyMonitor();
        if (ret != kIOReturnSuccess) return ret; // do not overwrite outside routing changes
    }
    ivars->monitorDirty = true;
    auto ret = WriteRoutes(ivars->monitor.before.routes);
    if (ret != kIOReturnSuccess) return ret;
    for (unsigned b = 0; b < 2; ++b) {
        ret = WriteRow(ivars->monitor.buses[b], ivars->monitor.before.rows[ivars->monitor.buses[b]]);
        if (ret != kIOReturnSuccess) return ret;
    }
    uint32_t routes[Clarett::Monitor::Routes] = {};
    ret = ReadRoutes(routes);
    if (ret != kIOReturnSuccess) return ret;
    if (!Clarett::Monitor::sameRoutes(routes, ivars->monitor.before.routes, ivars->monitor.before.band)) return kIOReturnIOError;
    for (unsigned b = 0; b < 2; ++b) {
        uint16_t row[Clarett::Monitor::Inputs] = {};
        ret = ReadRow(ivars->monitor.buses[b], row);
        if (ret != kIOReturnSuccess) return ret;
        if (memcmp(row, ivars->monitor.before.rows[ivars->monitor.buses[b]], sizeof(row))) return kIOReturnIOError;
    }
    ivars->monitorEnabled = false;
    ivars->monitorDirty = false;
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::ApplyMonitor(bool enabled, uint32_t inputMode, uint32_t outputPair, uint32_t blend) {
    Clarett::Monitor::Options options{enabled, inputMode, outputPair, blend};
    if (!Clarett::Monitor::valid(options)) return kIOReturnBadArgument;
    auto ret = DisableMonitor();
    if (ret != kIOReturnSuccess || !enabled) return ret;
    if (ivars->failed || __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    Clarett::Monitor::Snapshot original = {};
    original.band = A::speedBand(ivars->sampleRate);
    ret = ReadRoutes(original.routes);
    if (ret != kIOReturnSuccess) return ret;
    for (unsigned bus = 0; bus < Clarett::Monitor::Buses; ++bus) {
        ret = ReadRow(bus, original.rows[bus]);
        if (ret != kIOReturnSuccess) return ret;
    }
    if (!Clarett::Monitor::build(original, options, ivars->monitor)) return kIOReturnNoResources;
    // These buses have no router references. Mute them first, configure their
    // input slots without changing live original mixes, then switch only the
    // requested analogue outputs after the complete new rows are ready.
    ivars->monitorDirty = true;
    uint16_t silent[Clarett::Monitor::Inputs] = {};
    for (unsigned b = 0; b < 2 && ret == kIOReturnSuccess; ++b) ret = WriteRow(ivars->monitor.buses[b], silent);
    if (ret == kIOReturnSuccess) ret = WriteRoutes(ivars->monitor.setup);
    for (unsigned b = 0; b < 2 && ret == kIOReturnSuccess; ++b) ret = WriteRow(ivars->monitor.buses[b], ivars->monitor.rows[b]);
    if (ret == kIOReturnSuccess) ret = WriteRoutes(ivars->monitor.active);
    if (ret == kIOReturnSuccess) ret = VerifyMonitor();
    if (ret == kIOReturnSuccess) {
        ivars->monitorEnabled = true;
        ivars->monitorDirty = false;
    } else if (!ivars->failed) {
        // A known, completed operation may be rolled back. An uncertain or
        // timed-out mailbox session is terminal and is never reused here.
        DisableMonitor();
    }
    return ret;
}

void ClarettAudio::MonitorStatus(const char *state, uint32_t error) {
    auto props = OSDictionary::withCapacity(8);
    auto label = OSString::withCString(state);
    if (props && label) {
        props->setObject("ClarettMonitorStatus", label);
        number(props, "ClarettMonitorError", error);
        number(props, "ClarettMonitorEnabled", ivars->monitorEnabled);
        number(props, "ClarettMonitorUncertain", ivars->monitorDirty);
        number(props, "ClarettControlPending", __atomic_load_n(&ivars->controlPending, __ATOMIC_ACQUIRE));
        if (ivars->monitorEnabled) {
            number(props, "ClarettMonitorInputMode", ivars->monitor.options.inputMode);
            number(props, "ClarettMonitorOutputPair", ivars->monitor.options.outputPair);
            number(props, "ClarettMonitorBlend", ivars->monitor.options.blend);
        }
        SetProperties(props);
    }
    OSSafeReleaseNULL(label); OSSafeReleaseNULL(props);
}

kern_return_t ClarettAudio::QueueMonitorConfiguration(OSDictionary *settings) {
    if (__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) || !ivars->controlQueue) return kIOReturnNotReady;
    if (settings->getCount() != 4) return kIOReturnBadArgument;
    const auto enabled = OSDynamicCast(OSBoolean, settings->getObject("Enabled"));
    const auto input = OSDynamicCast(OSNumber, settings->getObject("InputMode"));
    const auto output = OSDynamicCast(OSNumber, settings->getObject("OutputPair"));
    const auto blend = OSDynamicCast(OSNumber, settings->getObject("Blend"));
    if (!enabled || !input || !output || !blend || input->unsigned64BitValue() > 3 ||
        output->unsigned64BitValue() > 3 || blend->unsigned64BitValue() > 100) return kIOReturnBadArgument;
    Clarett::Monitor::Options options{enabled == kOSBooleanTrue, input->unsigned32BitValue(), output->unsigned32BitValue(), blend->unsigned32BitValue()};
    if (!Clarett::Monitor::valid(options)) return kIOReturnBadArgument;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&ivars->controlPending, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return kIOReturnBusy;
    retain(); // keep this object alive until even a late closing request retires
    ivars->controlQueue->DispatchAsync(^{
        MonitorStatus("applying");
        auto ret = __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) ? kIOReturnNoDevice :
            ApplyMonitor(options.enabled, options.inputMode, options.outputPair, options.blend);
        __atomic_store_n(&ivars->controlPending, 0, __ATOMIC_RELEASE);
        MonitorStatus(ret == kIOReturnSuccess ? (ivars->monitorEnabled ? "on-verified" : "off-restored") :
                      (ivars->monitorDirty ? "state-uncertain" : "request-failed"), uint32_t(ret));
        release();
    });
    return kIOReturnSuccess;
}

void ClarettAudio::TimingStatus(const char *state, uint32_t error) {
    auto props = OSDictionary::withCapacity(6);
    auto label = OSString::withCString(state);
    if (props && label) {
        props->setObject("ClarettTimingStatus", label);
        number(props, "ClarettTimingError", error);
        number(props, "ClarettTimingPending", __atomic_load_n(&ivars->timingPending, __ATOMIC_ACQUIRE));
        number(props, "ClarettAudioSafetyFrames", __atomic_load_n(&ivars->safetyFrames, __ATOMIC_ACQUIRE));
        number(props, "ClarettAudioPeriodFrames", __atomic_load_n(&ivars->periodFrames, __ATOMIC_ACQUIRE));
        SetProperties(props);
    }
    OSSafeReleaseNULL(label); OSSafeReleaseNULL(props);
}

kern_return_t ClarettAudio::QueueTimingConfiguration(OSDictionary *settings) {
    if (__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) || !ivars->controlQueue) return kIOReturnNotReady;
    if (settings->getCount() != 1) return kIOReturnBadArgument;
    auto safety = OSDynamicCast(OSNumber, settings->getObject("SafetyFrames"));
    if (!safety || !A::safetyValid(safety->unsigned64BitValue())) return kIOReturnBadArgument;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&ivars->timingPending, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return kIOReturnBusy;
    const auto requested = safety->unsigned32BitValue();
    __atomic_store_n(&ivars->desiredSafety, requested, __ATOMIC_RELEASE);
    retain();
    ivars->controlQueue->DispatchAsync(^{
        TimingStatus("applying");
        // The host stops I/O and invokes ClarettDevice's configuration callback.
        // Never change safety offsets or rebuild active DMA descriptors here.
        const auto ret = __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) ? kIOReturnNoDevice :
            ivars->device->RequestDeviceConfigurationChange(A::TimingAction | requested, nullptr);
        if (ret != kIOReturnSuccess) {
            __atomic_store_n(&ivars->timingPending, 0, __ATOMIC_RELEASE);
            TimingStatus("request-failed", uint32_t(ret));
        }
        release();
    });
    return kIOReturnSuccess;
}

kern_return_t ClarettAudio::PerformTimingChange(uint32_t safety) {
    if (!A::safetyValid(safety) || __atomic_load_n(&ivars->timingPending, __ATOMIC_ACQUIRE) != 1 ||
        safety != __atomic_load_n(&ivars->desiredSafety, __ATOMIC_ACQUIRE)) return kIOReturnBadArgument;
    kern_return_t ret = kIOReturnNotReady;
    // Called by the host only after StopDevice has stopped DMA and drained the
    // period queue. Do not synchronously enter controlQueue from this callback:
    // setters synchronize on the audio device's own work queue.
    if (!__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE)) {
        const auto previous = __atomic_load_n(&ivars->safetyFrames, __ATOMIC_ACQUIRE);
        ret = ivars->device->SetInputSafetyOffset(safety);
        if (ret == kIOReturnSuccess) {
            ret = ivars->device->SetOutputSafetyOffset(safety);
            if (ret != kIOReturnSuccess) {
                const auto rollback = ivars->device->SetInputSafetyOffset(previous);
                if (rollback != kIOReturnSuccess) {
                    ivars->failed = true;
                    ivars->device->SetDeviceIsAlive(false);
                }
            }
        }
        if (ret == kIOReturnSuccess) {
            __atomic_store_n(&ivars->safetyFrames, safety, __ATOMIC_RELEASE);
            __atomic_store_n(&ivars->periodFrames, A::periodForSafety(safety), __ATOMIC_RELEASE);
            ivars->timerInterval = A::pollTicks(ivars->ticksPerSecond, ivars->sampleRate, safety);
        }
    }
    __atomic_store_n(&ivars->timingPending, 0, __ATOMIC_RELEASE);
    TimingStatus(ret == kIOReturnSuccess ? "stable" : "request-failed", uint32_t(ret));
    return ret;
}

void ClarettAudio::AbortTimingChange(uint32_t safety) {
    if (__atomic_load_n(&ivars->timingPending, __ATOMIC_ACQUIRE) != 1 ||
        safety != __atomic_load_n(&ivars->desiredSafety, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&ivars->timingPending, 0, __ATOMIC_RELEASE);
    TimingStatus("request-aborted", uint32_t(kIOReturnAborted));
}

void ClarettAudio::RateStatus(const char *state, uint32_t requested, uint32_t error) {
    auto props = OSDictionary::withCapacity(6);
    auto label = OSString::withCString(state);
    if (props && label) {
        props->setObject("ClarettRateStatus", label);
        number(props, "ClarettRateError", error);
        number(props, "ClarettRequestedSampleRate", requested);
        const auto rate = __atomic_load_n(&ivars->sampleRate, __ATOMIC_ACQUIRE);
        number(props, "ClarettAudioSampleRate", rate);
        number(props, "ClarettAudioLiveCaptureChannels", ivars->liveCapture[A::speedBand(rate)]);
        SetProperties(props);
    }
    OSSafeReleaseNULL(label); OSSafeReleaseNULL(props);
}

kern_return_t ClarettAudio::PerformSampleRateChange(double rate) {
    if (!A::rateValid(rate)) return kIOReturnBadArgument;
    if (__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    const auto requested = uint32_t(rate);
    const auto previous = __atomic_load_n(&ivars->sampleRate, __ATOMIC_ACQUIRE);
    if (requested == previous) return kIOReturnSuccess;
    // 1 is an outstanding safety-margin change; 2 owns a rate transition.
    // This also excludes monitor commands which could touch a different band.
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&ivars->timingPending, &idle, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return kIOReturnBusy;
    idle = 0;
    if (!__atomic_compare_exchange_n(&ivars->controlPending, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&ivars->timingPending, 0, __ATOMIC_RELEASE);
        return kIOReturnBusy;
    }
    RateStatus("applying", requested);
    kern_return_t ret = kIOReturnBusy;
    if (!__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) && !ivars->failed &&
        !ivars->monitorEnabled && !ivars->monitorDirty) {
        // No synchronous dispatch to controlQueue here: Core Audio owns the
        // stopped-I/O configuration callback and may hold the device work queue.
        auto metadata = [&](uint32_t value) {
            const auto device = ivars->device->SetSampleRate(value);
            const auto input = ivars->input->DeviceSampleRateChanged(value);
            const auto output = ivars->output->DeviceSampleRateChanged(value);
            return device != kIOReturnSuccess ? device : input != kIOReturnSuccess ? input : output;
        };
        ret = ChangeClock(requested);
        if (ret == kIOReturnSuccess) ret = metadata(requested);
        if (ret == kIOReturnSuccess) {
            __atomic_store_n(&ivars->sampleRate, requested, __ATOMIC_RELEASE);
            ivars->timerInterval = A::pollTicks(ivars->ticksPerSecond, requested, ivars->safetyFrames);
        } else {
            // An uncertain mailbox is terminal. Only completed, known exchanges
            // may be followed by a rollback; never send into a poisoned session.
            bool restored = false;
            if (!ivars->failed && !__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) {
                restored = ChangeClock(previous) == kIOReturnSuccess;
                if (restored) restored = metadata(previous) == kIOReturnSuccess;
            }
            if (!restored) {
                ivars->failed = true;
                ivars->device->SetDeviceIsAlive(false);
            }
        }
    }
    RateStatus(ret == kIOReturnSuccess ? "stable" : "request-failed", requested, uint32_t(ret));
    __atomic_store_n(&ivars->controlPending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ivars->timingPending, 0, __ATOMIC_RELEASE);
    return ret;
}

// Output control shares the serialized mailbox queue with monitoring. No config
// transactions run in the PCM callback or overlap a sample-rate handshake.
kern_return_t ClarettAudio::VolumeRead(uint32_t offset, uint32_t length, uint8_t *data) {
    if (ivars->failed || __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    const auto r = Clarett::volumeRead(0, offset, length);
    return Exchange(r.opcode, r.payload, r.length, r.expectedLength, data, uint16_t(length));
}
kern_return_t ClarettAudio::VolumeWrite(uint32_t offset, uint8_t first, uint8_t second, bool ownership) {
    if (ivars->failed || __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    auto r = Clarett::volumeWrite(0, offset, first, second);
    auto ret = Exchange(r.opcode, r.payload, r.length, 0, nullptr, 0);
    if (ret != kIOReturnSuccess) return ret;
    r = Clarett::volumeCommit(0, ownership);
    ret = Exchange(r.opcode, r.payload, r.length, 0, nullptr, 0);
    if (ret != kIOReturnSuccess) return ret;
    uint8_t data[2] = {};
    const unsigned length = ownership ? 1 : 2;
    ret = VolumeRead(offset, length, data);
    if (ret != kIOReturnSuccess) return ret;
    return data[0] == first && (ownership || data[1] == second) ? kIOReturnSuccess : kIOReturnIOError;
}
namespace {
class VolumeWire final : public Clarett::Volume::Wire {
    ClarettAudio *owner;
public:
    kern_return_t error = kIOReturnSuccess;
    explicit VolumeWire(ClarettAudio *driver) : owner(driver) {}
    bool knob(unsigned &attenuation) override {
        uint8_t byte = 0;
        error = owner->VolumeRead(112, 1, &byte); attenuation = byte;
        return error == kIOReturnSuccess && byte <= 127;
    }
    bool gains(unsigned pair, unsigned attenuation) override {
        error = owner->VolumeWrite(pair ? 36 : 32, uint8_t(attenuation), uint8_t(attenuation), false);
        return error == kIOReturnSuccess;
    }
    bool software(unsigned pair, bool enabled) override {
        // These bytes contain exactly the two gain-owner bits for this pair.
        // Reject unknown/reserved bits instead of overwriting unrelated state.
        uint8_t flags = 0;
        const auto offset = pair ? 56 : 52;
        error = owner->VolumeRead(offset, 1, &flags);
        if (error != kIOReturnSuccess || flags > 3) { if (!error) error = kIOReturnBadArgument; return false; }
        error = owner->VolumeWrite(offset, enabled ? 0 : 3, 0, true);
        return error == kIOReturnSuccess;
    }
};
}
void ClarettAudio::VolumeStatus(const char *state, uint32_t error) {
    auto props = OSDictionary::withCapacity(16);
    auto label = OSString::withCString(state);
    if (props && label) {
        props->setObject("ClarettVolumeStatus", label);
        number(props, "ClarettVolumeError", error);
        number(props, "ClarettVolumeRevision", ivars->volumeRevision);
        number(props, "ClarettVolumeRangeDB", Clarett::Volume::ControlRange);
        number(props, "ClarettVolumePending", __atomic_load_n(&ivars->volumePending, __ATOMIC_ACQUIRE));
        number(props, "ClarettVolumeUncertain", ivars->volume.uncertain);
        number(props, "ClarettVolumeAttenuation", ivars->volume.settings.attenuation);
        const char *enabled[] = {"ClarettVolumeEnabled12", "ClarettVolumeEnabled34"};
        const char *known[] = {"ClarettVolumeKnown12", "ClarettVolumeKnown34"};
        const char *ceiling[] = {"ClarettVolumeCeiling12", "ClarettVolumeCeiling34"};
        const char *attenuation[] = {"ClarettVolumeHardware12", "ClarettVolumeHardware34"};
        for (unsigned p = 0; p < 2; ++p) {
            number(props, enabled[p], ivars->volume.settings.enabled[p]);
            number(props, known[p], ivars->volume.known[p]);
            number(props, ceiling[p], ivars->volume.settings.ceiling[p]);
            // Meaningful only with software enabled and no uncertainty.
            number(props, attenuation[p], Clarett::Volume::gain(ivars->volume.settings, p));
        }
        SetProperties(props);
    }
    OSSafeReleaseNULL(label); OSSafeReleaseNULL(props);
}
kern_return_t ClarettAudio::QueueOutputAttenuation(uint32_t attenuation) {
    if (!__atomic_load_n(&ivars->volumeEnabled, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    return QueueVolume(false, false, false, 0, 0, attenuation);
}
kern_return_t ClarettAudio::QueueVolumeConfiguration(OSDictionary *settings) {
    if (settings->getCount() != 4) return kIOReturnBadArgument;
    auto a = OSDynamicCast(OSBoolean, settings->getObject("Enabled12"));
    auto b = OSDynamicCast(OSBoolean, settings->getObject("Enabled34"));
    auto c = OSDynamicCast(OSNumber, settings->getObject("Ceiling12"));
    auto d = OSDynamicCast(OSNumber, settings->getObject("Ceiling34"));
    if (!a || !b || !c || !d || c->unsigned64BitValue() > Clarett::Volume::MaximumCeiling ||
        d->unsigned64BitValue() > Clarett::Volume::MaximumCeiling) return kIOReturnBadArgument;
    return QueueVolume(true, a == kOSBooleanTrue, b == kOSBooleanTrue, c->unsigned32BitValue(),
                       d->unsigned32BitValue(), 0);
}
kern_return_t ClarettAudio::QueueVolume(bool custom, bool enabled12, bool enabled34, uint32_t ceiling12, uint32_t ceiling34, uint32_t attenuation) {
    if (attenuation > Clarett::Volume::Range || (custom && (ceiling12 > 60 || ceiling34 > 60))) return kIOReturnBadArgument;
    if (__atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) || !ivars->controlQueue) return kIOReturnNotReady;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&ivars->controlPending, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return kIOReturnBusy;
    __atomic_store_n(&ivars->volumePending, 1, __ATOMIC_RELEASE);
    retain();
    ivars->controlQueue->DispatchAsync(^{
        VolumeStatus("applying");
        auto next = ivars->volume.settings;
        if (custom) {
            next.enabled[0] = enabled12; next.enabled[1] = enabled34;
            next.ceiling[0] = ceiling12; next.ceiling[1] = ceiling34;
        }
        // Configuration never supplies a stale logical volume. Only the
        // standard level control changes it, serialized with this request.
        if (!custom) next.attenuation = attenuation;
        VolumeWire wire(this);
        const auto result = __atomic_load_n(&ivars->closing, __ATOMIC_ACQUIRE) || ivars->failed ?
            Clarett::Volume::Result::Failed : Clarett::Volume::apply(ivars->volume, next, wire);
        __atomic_store_n(&ivars->volumeEnabled, !ivars->volume.uncertain &&
            (ivars->volume.settings.enabled[0] || ivars->volume.settings.enabled[1]), __ATOMIC_RELEASE);
        // This completion runs after the property handler returns. Never wait
        // for the mailbox from a level-control callback: SetDecibelValue uses
        // that control's work queue and would otherwise deadlock.
        if (ivars->volumeControl) ivars->volumeControl->SetDecibelValue(Clarett::Volume::controlDecibels(ivars->volume.settings.attenuation));
        ++ivars->volumeRevision;
        __atomic_store_n(&ivars->volumePending, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&ivars->controlPending, 0, __ATOMIC_RELEASE);
        VolumeStatus(result == Clarett::Volume::Result::Verified ? "verified" :
                     result == Clarett::Volume::Result::TurnKnobDown ? "turn-knob-down" : "request-failed",
                     uint32_t(wire.error != kIOReturnSuccess ? wire.error : result == Clarett::Volume::Result::Verified ? kIOReturnSuccess : kIOReturnNotReady));
        release();
    });
    return kIOReturnSuccess;
}
