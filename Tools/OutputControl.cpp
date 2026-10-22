// SPDX-License-Identifier: GPL-2.0-only
// Explicit control requests only. Never opens audio I/O or changes default devices.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Volume.hpp"
static AudioObjectPropertyAddress property(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
static AudioDeviceID findDevice() {
    auto p = property(kAudioHardwarePropertyDevices); UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&p,0,nullptr,&size) != noErr) return 0;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (devices.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject,&p,0,nullptr,&size,devices.data()) != noErr) return 0;
    p = property(kAudioDevicePropertyDeviceUID);
    for (auto device : devices) {
        CFStringRef uid = nullptr; size = sizeof(uid);
        if (AudioObjectGetPropertyData(device,&p,0,nullptr,&size,&uid) != noErr || !uid) continue;
        const bool match = CFEqual(uid, CFSTR("org.codaco.clarett.2pre.experimental")); CFRelease(uid);
        if (match) return device;
    }
    return 0;
}
static bool integer(const char *text, int low, int high, int &out) {
    char *end = nullptr; errno = 0;
    const auto n = std::strtol(text, &end, 10);
    if (!text[0] || errno || *end || n < low || n > high) return false;
    out = int(n); return true;
}
static void status(AudioDeviceID device) {
    auto p = property(kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeOutput);
    Float32 db = 0; UInt32 size = sizeof(db);
    if (AudioObjectGetPropertyData(device,&p,0,nullptr,&size,&db) == noErr)
        std::printf("Core Audio logical level: %.0f dB (relative to each enabled pair's ceiling)\n", db);
    p = property(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput);
    Float32 scalar = 0; size = sizeof(scalar);
    if (AudioObjectGetPropertyData(device,&p,0,nullptr,&size,&scalar) == noErr)
        std::printf("Core Audio scalar: %.6f\n", scalar);
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceNameMatching("ClarettProbe"),&iterator) != KERN_SUCCESS) return;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        CFMutableDictionaryRef values = nullptr;
        if (IORegistryEntryCreateCFProperties(service,&values,kCFAllocatorDefault,0) == KERN_SUCCESS && values) {
            const char *keys[] = {"Status","Error","Revision","RangeDB","Pending","Uncertain","Attenuation","Enabled12","Enabled34","Known12","Known34","Ceiling12","Ceiling34","Hardware12","Hardware34"};
            for (auto key : keys) {
                char full[80]; std::snprintf(full,sizeof(full),"ClarettVolume%s",key);
                auto name = CFStringCreateWithCString(kCFAllocatorDefault,full,kCFStringEncodingUTF8);
                auto value = CFDictionaryGetValue(values,name); CFRelease(name);
                if (!value) continue;
                if (CFGetTypeID(value) == CFNumberGetTypeID()) {
                    int64_t n = 0; CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&n);
                    std::printf("%s: %lld\n",full,static_cast<long long>(n));
                } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
                    char text[128]{}; CFStringGetCString(static_cast<CFStringRef>(value),text,sizeof(text),kCFStringEncodingUTF8);
                    std::printf("%s: %s\n",full,text);
                }
            }
            CFRelease(values);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
}
int main(int argc, char **argv) {
    const bool report = argc == 2 && !std::strcmp(argv[1],"status");
    int level = 0, mask = 0, cap12 = 0, cap34 = 0;
    const bool db = argc == 3 && !std::strcmp(argv[1],"level") && integer(argv[2],-127,0,level);
    const bool scalar = argc == 3 && !std::strcmp(argv[1],"scalar") && integer(argv[2],0,100,level);
    const bool configure = argc == 5 && !std::strcmp(argv[1],"configure") &&
        integer(argv[2],0,3,mask) && integer(argv[3],0,60,cap12) && integer(argv[4],0,60,cap34);
    if (!report && !db && !scalar && !configure) {
        std::fputs("Usage: output-control status | level DB | scalar 0..100 | configure mask cap12 cap34\n"
            "mask: 0=unchanged/return controlled pairs to knob, 1=1–2, 2=3–4, 3=both software.\n"
            "Caps are positive attenuation codes (0..60); level 0/scalar 100 means the caps, not hardware unity.\n"
            "Level must be inside the active driver's advertised range (driver 16: -48..0 dB; minimum selects -127 dB hardware floor).\n"
            "Set physical levels down first. Requests are asynchronous: inspect status separately.\n",stderr); return 2;
    }
    const auto device = findDevice();
    if (!device) { std::fputs("Clarett Core Audio device unavailable.\n",stderr); return 1; }
    if (report) { status(device); return 0; }
    OSStatus result;
    if (configure) {
        auto p = property(0x636c766f);
        if (!AudioObjectHasProperty(device,&p)) { std::fputs("Output control requires driver 15.\n",stderr); return 1; }
        const void *keys[] = {CFSTR("Enabled12"),CFSTR("Enabled34"),CFSTR("Ceiling12"),CFSTR("Ceiling34")};
        auto a = CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&cap12);
        auto b = CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&cap34);
        const void *values[] = {(mask & 1) ? kCFBooleanTrue : kCFBooleanFalse,(mask & 2) ? kCFBooleanTrue : kCFBooleanFalse,a,b};
        auto request = CFDictionaryCreate(kCFAllocatorDefault,keys,values,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        result = AudioObjectSetPropertyData(device,&p,0,nullptr,sizeof(request),&request);
        CFRelease(request); CFRelease(a); CFRelease(b);
    } else {
        auto p = property(db ? kAudioDevicePropertyVolumeDecibels : kAudioDevicePropertyVolumeScalar,kAudioObjectPropertyScopeOutput);
        if (db) {
            auto rangeProperty = property(kAudioDevicePropertyVolumeRangeDecibels,kAudioObjectPropertyScopeOutput);
            AudioValueRange range{}; UInt32 bytes = sizeof(range);
            if (AudioObjectGetPropertyData(device,&rangeProperty,0,nullptr,&bytes,&range) != noErr ||
                level < range.mMinimum || level > range.mMaximum) {
                std::fputs("Level is outside the active driver's advertised dB range. Use scalar 0 for the quiet endpoint.\n",stderr);
                return 2;
            }
        }
        Float32 value = db ? Float32(level) : Float32(level) / 100;
        result = AudioObjectSetPropertyData(device,&p,0,nullptr,sizeof(value),&value);
    }
    if (result != noErr) { std::fprintf(stderr,"Core Audio rejected request: %d\n",int(result)); return 1; }
    std::puts("Request accepted. Check status for a newer revision, pending=0, uncertainty=0 and verified gain/ownership before listening.");
    return 0;
}
