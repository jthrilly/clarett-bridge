// SPDX-License-Identifier: GPL-2.0-only
// Configure only this prototype's Core Audio settings. Never starts I/O,
// changes default devices, plays a tone, records, or changes boot security.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "SampleRate.hpp"
static AudioObjectPropertyAddress property(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}
static AudioDeviceID findDevice() {
    auto p = property(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &p, 0, nullptr, &size) != noErr) return 0;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (devices.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject, &p, 0, nullptr, &size, devices.data()) != noErr) return 0;
    p = property(kAudioDevicePropertyDeviceUID);
    for (auto device : devices) {
        CFStringRef uid = nullptr; size = sizeof(uid);
        if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, &uid) != noErr || !uid) continue;
        const bool match = CFEqual(uid, CFSTR("org.codaco.clarett.2pre.experimental"));
        CFRelease(uid);
        if (match) return device;
    }
    return 0;
}
int main(int argc, char **argv) {
    const bool margin = argc == 3 && !std::strcmp(argv[1], "margin");
    const bool buffer = argc == 3 && !std::strcmp(argv[1], "buffer");
    const bool rate = argc == 3 && !std::strcmp(argv[1], "rate");
    UInt32 frames = 0;
    const UInt32 choices[] = {64, 128, 256, 512, 1024, 2048};
    for (auto value : choices) {
        char word[16]; std::snprintf(word, sizeof(word), "%u", value);
        if (argc == 3 && !std::strcmp(argv[2], word) && (buffer || (margin && value >= 128 && value <= 512))) frames = value;
    }
    if (rate) for (auto value : Clarett::Audio::Rates) {
        char word[16]; std::snprintf(word, sizeof(word), "%u", value);
        if (!std::strcmp(argv[2], word)) frames = value;
    }
    if ((!margin && !buffer && !rate) || !frames) {
        std::fputs("Usage: audio-configure margin 128|256|512 OR buffer 64|128|256|512|1024|2048 OR rate 44100|48000|88200|96000\n", stderr);
        return 2;
    }
    const auto device = findDevice();
    if (!device) { std::fputs("Prototype Clarett Core Audio device unavailable.\n", stderr); return 1; }
    OSStatus status;
    if (margin) {
        auto p = property(0x636c7470);
        if (!AudioObjectHasProperty(device, &p)) { std::fputs("Runtime margin control requires driver version 12.\n", stderr); return 1; }
        const auto number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &frames);
        const void *keys[] = {CFSTR("SafetyFrames")}, *values[] = {number};
        const auto request = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        status = AudioObjectSetPropertyData(device, &p, 0, nullptr, sizeof(request), &request);
        CFRelease(request); CFRelease(number);
    } else if (rate) {
        auto p = property(kAudioDevicePropertyAvailableNominalSampleRates);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device, &p, 0, nullptr, &size) != noErr || !size || size % sizeof(AudioValueRange)) return 1;
        std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
        if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, ranges.data()) != noErr) return 1;
        bool available = false;
        for (const auto &range : ranges) available |= frames >= range.mMinimum && frames <= range.mMaximum;
        if (!available) { std::fputs("Requested rate is not advertised by the active driver.\n", stderr); return 1; }
        double requested = frames;
        p = property(kAudioDevicePropertyNominalSampleRate);
        status = AudioObjectSetPropertyData(device, &p, 0, nullptr, sizeof(requested), &requested);
    } else {
        auto p = property(kAudioDevicePropertyBufferFrameSizeRange);
        AudioValueRange range{}; UInt32 size = sizeof(range);
        if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, &range) != noErr || frames < range.mMinimum || frames > range.mMaximum) {
            std::fputs("Requested buffer is outside the live supported range.\n", stderr); return 1;
        }
        p = property(kAudioDevicePropertyBufferFrameSize);
        status = AudioObjectSetPropertyData(device, &p, 0, nullptr, sizeof(frames), &frames);
    }
    if (status != noErr) { std::fprintf(stderr, "Core Audio rejected request: %d\n", int(status)); return 1; }
    std::printf("Accepted %s request: %u %s. Verify applied state separately with audio-status and driver diagnostics.\n", argv[1], frames, rate ? "Hz" : "frames");
    return 0;
}
