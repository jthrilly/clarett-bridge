// SPDX-License-Identifier: GPL-2.0-only
#include "ClarettSettings.h"
#include "ClarettAudio.h"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSBoolean.h>
#define super IOUserAudioCustomProperty
struct ClarettSettings_IVars { ClarettAudio *owner; uint32_t kind; };
bool ClarettSettings::Configure(ClarettAudio *owner, uint32_t kind) {
    const IOUserAudioObjectPropertyAddress address = {kind == 2 ? 0x636c766fu : kind == 1 ? 0x636c7470u : 0x636c6d63u, IOUserAudioObjectPropertyScope::Global, IOUserAudioObjectPropertyElementMain};
    if (!super::init(owner, address, true, IOUserAudioCustomPropertyDataType::None,
                     IOUserAudioCustomPropertyDataType::Dictionary)) return false;
    ivars = IONewZero(ClarettSettings_IVars, 1);
    if (!ivars) return false;
    ivars->owner = owner;
    ivars->kind = kind;
    auto initial = OSDictionary::withCapacity(4);
    if (!initial) return false;
    if (kind == 2) {
        initial->setObject("Enabled12", kOSBooleanFalse);
        initial->setObject("Enabled34", kOSBooleanFalse);
        const char *keys[] = {"Ceiling12", "Ceiling34"};
        for (auto key : keys) {
            auto value = OSNumber::withNumber(30, 32);
            if (!value) { initial->release(); return false; }
            initial->setObject(key, value); value->release();
        }
        auto ret = SetQualifierAndDataValue(nullptr, initial);
        initial->release(); return ret == kIOReturnSuccess;
    }
    if (kind == 1) {
        auto value = OSNumber::withNumber(512, 32);
        if (!value) { initial->release(); return false; }
        initial->setObject("SafetyFrames", value); value->release();
        auto ret = SetQualifierAndDataValue(nullptr, initial);
        initial->release(); return ret == kIOReturnSuccess;
    }
    initial->setObject("Enabled", kOSBooleanFalse);
    const char *keys[] = {"InputMode", "OutputPair", "Blend"};
    for (auto key : keys) {
        auto value = OSNumber::withNumber(!strcmp(key, "InputMode") ? 1 : !strcmp(key, "OutputPair") ? 2 : 50, 32);
        if (!value) { initial->release(); return false; }
        initial->setObject(key, value); value->release();
    }
    auto ret = SetQualifierAndDataValue(nullptr, initial);
    initial->release();
    return ret == kIOReturnSuccess;
}
void ClarettSettings::free() {
    if (ivars) IODelete(ivars, ClarettSettings_IVars, 1);
    super::free();
}
kern_return_t ClarettSettings::HandleChangeCustomPropertyDataValueWithQualifier(OSObject *qualifier, OSObject *data) {
    if (qualifier || !ivars || !ivars->owner) return kIOReturnBadArgument;
    auto settings = OSDynamicCast(OSDictionary, data);
    if (!settings) return kIOReturnBadArgument;
    auto ret = ivars->kind == 2 ? ivars->owner->QueueVolumeConfiguration(settings) : ivars->kind == 1 ? ivars->owner->QueueTimingConfiguration(settings) : ivars->owner->QueueMonitorConfiguration(settings);
    // This property records an accepted request. Actual applied/readback state
    // and any failure are published separately, never inferred from this value.
    return ret == kIOReturnSuccess ? super::HandleChangeCustomPropertyDataValueWithQualifier(qualifier, data) : ret;
}
