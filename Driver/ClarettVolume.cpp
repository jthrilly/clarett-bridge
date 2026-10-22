// SPDX-License-Identifier: GPL-2.0-only
#include "ClarettVolume.h"
#include "ClarettAudio.h"
#include "Volume.hpp"
#include <DriverKit/IOLib.h>
#define super IOUserAudioLevelControl
struct ClarettVolume_IVars { ClarettAudio *owner; };
bool ClarettVolume::Configure(ClarettAudio *owner) {
    if (!super::init(owner, true, -float(Clarett::Volume::Range), {-float(Clarett::Volume::Range), 0},
        IOUserAudioObjectPropertyElementMain, IOUserAudioObjectPropertyScope::Output,
        IOUserAudioClassID::VolumeControl)) return false;
    ivars = IONewZero(ClarettVolume_IVars, 1);
    if (!ivars) return false;
    ivars->owner = owner;
    return true;
}
void ClarettVolume::free() {
    if (ivars) IODelete(ivars, ClarettVolume_IVars, 1);
    super::free();
}
kern_return_t ClarettVolume::HandleChangeDecibelValue(float value) {
    // Comparisons also reject NaN/infinity. The hardware has 1 dB steps;
    // round toward attenuation so quantization never exceeds a requested level.
    uint32_t attenuation = 0;
    if (!Clarett::Volume::attenuationFromDecibels(value, attenuation)) return kIOReturnBadArgument;
    return ivars->owner->QueueOutputAttenuation(attenuation);
}
kern_return_t ClarettVolume::HandleChangeScalarValue(float value) {
    if (!(value >= 0 && value <= 1)) return kIOReturnBadArgument;
    // Use AudioDriverKit's own scalar/dB transfer curve, rather than inventing
    // a different mapping for macOS volume keys. The asynchronous worker later
    // publishes the quantized, verified hardware-backed value.
    const auto previous = GetDecibelValue();
    auto ret = SetScalarValue(value);
    if (ret == kIOReturnSuccess) ret = HandleChangeDecibelValue(GetDecibelValue());
    if (ret != kIOReturnSuccess) SetDecibelValue(previous);
    return ret;
}
