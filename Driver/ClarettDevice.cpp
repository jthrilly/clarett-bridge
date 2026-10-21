// SPDX-License-Identifier: GPL-2.0-only
#include "ClarettDevice.h"
#include "ClarettAudio.h"
#include "AudioRing.hpp"
#include <DriverKit/IOLib.h>
#define super IOUserAudioDevice
struct ClarettDevice_IVars { ClarettAudio *owner; };
bool ClarettDevice::Configure(ClarettAudio *owner, OSString *uid, OSString *model, OSString *manufacturer, uint32_t ringFrames) {
    if (!super::init(owner, false, uid, model, manufacturer, ringFrames)) return false;
    ivars = IONewZero(ClarettDevice_IVars, 1);
    if (!ivars) return false;
    ivars->owner = owner;
    return true;
}
void ClarettDevice::free() {
    if (ivars) IODelete(ivars, ClarettDevice_IVars, 1);
    super::free();
}
kern_return_t ClarettDevice::PerformDeviceConfigurationChange(uint64_t action, OSObject *info) {
    if ((action & 0xffffffff00000000ull) == Clarett::Audio::TimingAction)
        return info ? kIOReturnBadArgument : ivars->owner->PerformTimingChange(uint32_t(action));
    return super::PerformDeviceConfigurationChange(action, info);
}
kern_return_t ClarettDevice::AbortDeviceConfigurationChange(uint64_t action, OSObject *info) {
    if ((action & 0xffffffff00000000ull) == Clarett::Audio::TimingAction)
        ivars->owner->AbortTimingChange(uint32_t(action));
    return super::AbortDeviceConfigurationChange(action, info);
}
