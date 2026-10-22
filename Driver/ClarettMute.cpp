// SPDX-License-Identifier: GPL-2.0-only
#include "ClarettMute.h"
#include "ClarettAudio.h"
#include <DriverKit/IOLib.h>
#define super IOUserAudioBooleanControl
struct ClarettMute_IVars { ClarettAudio *owner; };
bool ClarettMute::Configure(ClarettAudio *owner) {
    if (!super::init(owner, true, false, IOUserAudioObjectPropertyElementMain,
        IOUserAudioObjectPropertyScope::Output, IOUserAudioClassID::MuteControl)) return false;
    ivars = IONewZero(ClarettMute_IVars, 1);
    if (!ivars) return false;
    ivars->owner = owner;
    return true;
}
void ClarettMute::free() {
    if (ivars) IODelete(ivars, ClarettMute_IVars, 1);
    super::free();
}
kern_return_t ClarettMute::HandleChangeControlValue(bool enabled) {
    // Completion updates this control only after hardware acknowledgement and
    // readback, outside this control's own work queue.
    return ivars->owner->QueueOutputMute(enabled);
}
