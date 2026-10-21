// SPDX-License-Identifier: GPL-2.0-only
// Read Core Audio properties only: never changes defaults, starts I/O or records.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <string>
#include <vector>
static bool get(AudioObjectID device, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope,
                void *data, UInt32 &size) {
    AudioObjectPropertyAddress p{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, data) == noErr;
}
static std::string name(AudioObjectID device) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (!get(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &value, size) || !value) return {};
    char text[1024] = {};
    CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(value);
    return text;
}
static UInt32 channels(AudioObjectID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress p{kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(device, &p, 0, nullptr, &bytes) != noErr || bytes < sizeof(AudioBufferList)) return 0;
    std::vector<uint8_t> data(bytes);
    if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &bytes, data.data()) != noErr) return 0;
    auto list = reinterpret_cast<AudioBufferList *>(data.data());
    UInt32 total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) total += list->mBuffers[i].mNumberChannels;
    return total;
}
static void clients(AudioObjectID device) {
    const auto global = kAudioObjectPropertyScopeGlobal;
    AudioObjectPropertyAddress p{kAudioHardwarePropertyProcessObjectList, global, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &p, 0, nullptr, &bytes) != noErr) return;
    std::vector<AudioObjectID> processes(bytes / sizeof(AudioObjectID));
    if (processes.empty() || !get(kAudioObjectSystemObject, p.mSelector, global, processes.data(), bytes)) return;
    for (auto process : processes) {
        UInt32 running = 0, size = sizeof(running);
        if (!get(process, kAudioProcessPropertyIsRunning, global, &running, size) || !running) continue;
        bool usesDevice = false;
        for (auto scope : {kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput}) {
            p = {kAudioProcessPropertyDevices, scope, kAudioObjectPropertyElementMain};
            size = 0;
            if (AudioObjectGetPropertyDataSize(process, &p, 0, nullptr, &size) != noErr) continue;
            std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
            if (devices.empty() || !get(process, p.mSelector, scope, devices.data(), size)) continue;
            for (auto value : devices) usesDevice |= value == device;
        }
        if (!usesDevice) continue;
        pid_t pid = 0; size = sizeof(pid);
        get(process, kAudioProcessPropertyPID, global, &pid, size);
        CFStringRef bundle = nullptr; size = sizeof(bundle);
        char label[256] = {};
        if (get(process, kAudioProcessPropertyBundleID, global, &bundle, size) && bundle) {
            CFStringGetCString(bundle, label, sizeof(label), kCFStringEncodingUTF8); CFRelease(bundle);
        }
        std::printf("  Active client: %s (PID %d)\n", label[0] ? label : "unknown bundle", int(pid));
    }
}
int main() {
    AudioObjectPropertyAddress p{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &p, 0, nullptr, &bytes) != noErr) return 2;
    std::vector<AudioObjectID> devices(bytes / sizeof(AudioObjectID));
    if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, devices.data(), bytes)) return 2;
    bool found = false;
    for (auto device : devices) {
        auto text = name(device);
        if (text.find("Clarett") == std::string::npos) continue;
        found = true;
        double rate = 0; UInt32 rateBytes = sizeof(rate);
        UInt32 running = 0, alive = 0, frames = 0, size = sizeof(UInt32);
        const auto global = kAudioObjectPropertyScopeGlobal;
        get(device, kAudioDevicePropertyNominalSampleRate, global, &rate, rateBytes);
        get(device, kAudioDevicePropertyDeviceIsRunningSomewhere, global, &running, size);
        size = sizeof(UInt32); get(device, kAudioDevicePropertyDeviceIsAlive, global, &alive, size);
        size = sizeof(UInt32); get(device, kAudioDevicePropertyBufferFrameSize, global, &frames, size);
        AudioValueRange range{}; UInt32 rangeSize = sizeof(range);
        get(device, kAudioDevicePropertyBufferFrameSizeRange, global, &range, rangeSize);
        UInt32 inputSafety = 0, outputSafety = 0;
        size = sizeof(UInt32); get(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, &inputSafety, size);
        size = sizeof(UInt32); get(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &outputSafety, size);
        std::printf("  Supported buffer range: %.0f–%.0f; safety: input %u / output %u frames\n", range.mMinimum, range.mMaximum, inputSafety, outputSafety);
        std::printf("%s\n  Device ID: %u; alive: %u; running: %u\n  Rate: %.0f Hz; buffer: %u frames\n  Channels: %u input, %u output\n",
                    text.c_str(), device, alive, running, rate, frames,
                    channels(device, kAudioObjectPropertyScopeInput), channels(device, kAudioObjectPropertyScopeOutput));
        p = {kAudioDevicePropertyAvailableNominalSampleRates, global, kAudioObjectPropertyElementMain};
        UInt32 rateSize = 0;
        if (AudioObjectGetPropertyDataSize(device, &p, 0, nullptr, &rateSize) == noErr && rateSize && rateSize % sizeof(AudioValueRange) == 0) {
            std::vector<AudioValueRange> rates(rateSize / sizeof(AudioValueRange));
            if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &rateSize, rates.data()) == noErr) {
                std::printf("  Available rates (Hz):");
                for (const auto &rate : rates) std::printf(" %.0f–%.0f", rate.mMinimum, rate.mMaximum);
                std::putchar('\n');
            }
        }
        for (auto scope : {kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput}) {
            p = {kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
            UInt32 streamBytes = 0;
            if (AudioObjectGetPropertyDataSize(device, &p, 0, nullptr, &streamBytes) != noErr || !streamBytes) continue;
            std::vector<AudioStreamID> streams(streamBytes / sizeof(AudioStreamID));
            if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &streamBytes, streams.data()) != noErr) continue;
            for (auto stream : streams) {
                AudioStreamBasicDescription format{}; UInt32 formatSize = sizeof(format);
                if (get(stream, kAudioStreamPropertyPhysicalFormat, global, &format, formatSize))
                    std::printf("  %s stream: %.0f Hz, %u channels\n", scope == kAudioObjectPropertyScopeInput ? "Input" : "Output", format.mSampleRate, format.mChannelsPerFrame);
            }
        }
        clients(device);
    }
    if (!found) std::puts("No Clarett Core Audio device found.");
    return found ? 0 : 1;
}
