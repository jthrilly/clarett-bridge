#!/usr/bin/env python3
"""Check real signatures and embedded capabilities; never print profile secrets."""
import plistlib
import subprocess
import sys
import json
from datetime import datetime
from pathlib import Path

app = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('build/attach/Build/Products/Debug/Clarett Probe.app')
driver = app / 'Contents/Library/SystemExtensions/org.codaco.ClarettProbe.driver.dext'

def run(*args):
    return subprocess.run(args, check=True, capture_output=True).stdout

hardware = json.loads(run('system_profiler', 'SPHardwareDataType', '-json'))['SPHardwareDataType'][0]
machine_ids = [hardware.get('provisioning_UDID'), hardware.get('platform_UUID')]

for bundle, required in [
    (app, {'com.apple.developer.system-extension.install': True}),
    (driver, {'com.apple.developer.driverkit': True,
              'com.apple.developer.driverkit.transport.pci': [{'IOPCIPrimaryMatch': '0xFFFFFFFF&0x00000000'}]}),
]:
    run('codesign', '--verify', '--strict', '--verbose=2', str(bundle))
    signed = plistlib.loads(run('codesign', '-d', '--entitlements', '-', '--xml', str(bundle)))
    contents = bundle/'Contents' if (bundle/'Contents').is_dir() else bundle
    raw = run('security', 'cms', '-D', '-i', str(contents/'embedded.provisionprofile'))
    profile = plistlib.loads(raw)
    allowed = profile.get('Entitlements', {})
    if not profile.get('ProvisionedDevices'):
        raise SystemExit(f'{bundle.name}: expected a development device provisioning profile')
    if not any(value and value in profile['ProvisionedDevices'] for value in machine_ids):
        raise SystemExit(f'{bundle.name}: this Mac is missing from the development profile')
    if profile.get('ExpirationDate', datetime.min) <= datetime.utcnow():
        raise SystemExit(f'{bundle.name}: development profile has expired')
    for key, value in required.items():
        # Ordinary sandbox flags need a valid signature, not a profile grant.
        profile_required = key.startswith('com.apple.developer.')
        if signed.get(key) != value or (profile_required and allowed.get(key) != value):
            raise SystemExit(f'{bundle.name}: missing or mismatched signed/profile entitlement: {key}')
    team = signed.get('com.apple.developer.team-identifier')
    if team not in profile.get('TeamIdentifier', []):
        raise SystemExit(f'{bundle.name}: signature/profile team mismatch')
    print(f'PASS: {bundle.name}: valid signature, this Mac provisioned, required capabilities, team {team}')
    if bundle == driver:
        if signed.get('com.apple.developer.driverkit.allow-any-userclient-access'):
            raise SystemExit('Unexpected unrestricted user-client entitlement')
        print('NOTE: ordinary app-sandbox entitlement in final DriverKit signature:',
              signed.get('com.apple.security.app-sandbox', 'omitted by Xcode; not asserted by this check'))

info = plistlib.loads((driver/'Info.plist').read_bytes())
personality = info['IOKitPersonalities']['ClarettProbe']
assert personality['IOPCIPrimaryMatch'].lower() == '0x00021cb5'
assert personality['ClarettProbeMode'] in ['attach', 'mailbox']
print(f'PASS: PCI match restricted to 1cb5:0002, mode {personality["ClarettProbeMode"]}')
