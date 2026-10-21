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
        std::printf("%s\n  Device ID: %u; alive: %u; running: %u\n  Rate: %.0f Hz; buffer: %u frames\n  Channels: %u input, %u output\n",
                    text.c_str(), device, alive, running, rate, frames,
                    channels(device, kAudioObjectPropertyScopeInput), channels(device, kAudioObjectPropertyScopeOutput));
    }
    if (!found) std::puts("No Clarett Core Audio device found.");
    return found ? 0 : 1;
}
