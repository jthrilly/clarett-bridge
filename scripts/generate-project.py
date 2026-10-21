#!/usr/bin/env python3
"""Generate a dependency-free Xcode project; no signing secrets are stored here."""
import sys
import hashlib
import plistlib
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
AUDIO = len(sys.argv) > 1 and sys.argv[1] == "audio"
CLASS = "ClarettAudio" if AUDIO else "ClarettProbe"
objects = {}

def ident(name):
    return hashlib.sha256(name.encode()).hexdigest()[:24].upper()

def obj(object_name, isa, **values):
    key = ident(object_name)
    objects[key] = dict(isa=isa, **values)
    return key

def plist(path, data):
    (ROOT / path).write_bytes(plistlib.dumps(data, sort_keys=False))

plist('Driver/Info.plist', {
    'CFBundleIdentifier': '$(PRODUCT_BUNDLE_IDENTIFIER)',
    'CFBundleExecutable': '$(EXECUTABLE_NAME)',
    'CFBundleName': '$(PRODUCT_NAME)',
    'CFBundlePackageType': 'DEXT',
    'CFBundleShortVersionString': '0.1',
    'CFBundleVersion': '$(CURRENT_PROJECT_VERSION)',
    'OSBundleUsageDescription': 'Identify a connected Clarett 2Pre Thunderbolt interface.',
    'IOKitPersonalities': {'ClarettProbe': {
        'CFBundleIdentifier': '$(PRODUCT_BUNDLE_IDENTIFIER)',
        'CFBundleIdentifierKernel': 'com.apple.kpi.iokit',
        'IOClass': 'IOUserService',
        'IOProviderClass': 'IOPCIDevice',
        'IOUserClass': CLASS,
        **({'IOUserAudioDriverUserClientProperties': {'IOClass': 'IOUserUserClient', 'IOUserClass': 'IOUserAudioDriverUserClient'}} if AUDIO else {}),
        'IOUserServerName': '$(PRODUCT_BUNDLE_IDENTIFIER)',
        'IOPCIPrimaryMatch': '0x00021cb5',
        'IOPCITunnelCompatible': True,
        'IOPCIPauseCompatible': True,
        'ClarettProbeMode': '$(CLARETT_PROBE_MODE)',
    }},
})
plist('Driver/ClarettProbe.entitlements', {
    'com.apple.developer.driverkit': True,
    **({'com.apple.developer.driverkit.family.audio': True} if AUDIO else {}),
    'com.apple.developer.driverkit.transport.pci': [{'IOPCIPrimaryMatch': '0xFFFFFFFF&0x00000000'}],
    'com.apple.security.app-sandbox': True,
})
plist('Host/Info.plist', {
    'CFBundleIdentifier': '$(PRODUCT_BUNDLE_IDENTIFIER)',
    'CFBundleExecutable': '$(EXECUTABLE_NAME)',
    'CFBundleName': 'Clarett Probe',
    'CFBundlePackageType': 'APPL',
    'CFBundleShortVersionString': '0.1',
    'CFBundleVersion': '$(CURRENT_PROJECT_VERSION)',
    'LSMinimumSystemVersion': '$(MACOSX_DEPLOYMENT_TARGET)',
    'NSPrincipalClass': 'NSApplication',
    'ClarettProbeMode': '$(CLARETT_PROBE_MODE)',
})
plist('Host/ClarettProbe.entitlements', {'com.apple.developer.system-extension.install': True})

def file(path, kind):
    return obj(path, 'PBXFileReference', path=path, sourceTree='<group>', lastKnownFileType=kind)

sources = [file('Host/ClarettProbeApp.swift', 'sourcecode.swift'),
           file(f'Driver/{CLASS}.cpp', 'sourcecode.cpp.cpp'),
           file(f'Driver/{CLASS}.iig', 'sourcecode.iig')]
if AUDIO:
    sources += [file('Driver/ClarettSettings.cpp', 'sourcecode.cpp.cpp'), file('Driver/ClarettSettings.iig', 'sourcecode.iig')]
others = [file('Shared/Protocol.hpp', 'sourcecode.cpp.h'),
          file('Shared/AudioRing.hpp', 'sourcecode.cpp.h'),
          file('Shared/Monitor.hpp', 'sourcecode.cpp.h'),
          file('Driver/Info.plist', 'text.plist.xml'),
          file('Host/Info.plist', 'text.plist.xml'),
          file('Driver/ClarettProbe.entitlements', 'text.plist.entitlements'),
          file('Host/ClarettProbe.entitlements', 'text.plist.entitlements')]
app = obj('app-product', 'PBXFileReference', path='Clarett Probe.app', sourceTree='BUILT_PRODUCTS_DIR', explicitFileType='wrapper.application')
dext = obj('dext-product', 'PBXFileReference', path='org.codaco.ClarettProbe.driver.dext', sourceTree='BUILT_PRODUCTS_DIR', explicitFileType='wrapper.driver-extension')
products = obj('products', 'PBXGroup', name='Products', children=[app, dext], sourceTree='<group>')
main = obj('main', 'PBXGroup', children=sources + others + [products], sourceTree='<group>')

common = {
    'CLANG_CXX_LANGUAGE_STANDARD': 'c++17', 'CLANG_ENABLE_MODULES': 'YES',
    'CLANG_WARN_DOCUMENTATION_COMMENTS': 'YES', 'GCC_WARN_64_TO_32_BIT_CONVERSION': 'YES',
    'GCC_WARN_UNDECLARED_SELECTOR': 'YES', 'GCC_WARN_UNINITIALIZED_AUTOS': 'YES_AGGRESSIVE',
    'ENABLE_USER_SCRIPT_SANDBOXING': 'YES', 'CODE_SIGN_STYLE': 'Automatic',
    'DEVELOPMENT_TEAM': '85EZ69PQHJ', 'CODE_SIGN_IDENTITY': 'Apple Development',
    'CLARETT_PROBE_MODE': 'audio' if AUDIO else 'attach', 'CURRENT_PROJECT_VERSION': '9' if AUDIO else '1',
}
def configs(name, extra):
    entries = []
    for config in ['Debug', 'Release']:
        settings = dict(extra)
        if config == 'Debug':
            settings.update(GCC_OPTIMIZATION_LEVEL='0', SWIFT_OPTIMIZATION_LEVEL='-Onone', DEBUG_INFORMATION_FORMAT='dwarf')
        else:
            settings.update(GCC_OPTIMIZATION_LEVEL='s', SWIFT_OPTIMIZATION_LEVEL='-O')
        entries.append(obj(name+config, 'XCBuildConfiguration', name=config, buildSettings=settings))
    return obj(name+'configs', 'XCConfigurationList', buildConfigurations=entries, defaultConfigurationIsVisible='0', defaultConfigurationName='Debug')

project_configs = configs('Project', common)
def phase(phase_name, isa, files=None, **extra):
    return obj(phase_name, isa, buildActionMask='2147483647', files=files or [], runOnlyForDeploymentPostprocessing='0', **extra)
def buildfile(name, ref):
    return obj(name, 'PBXBuildFile', fileRef=ref)

driver_sources = phase('driver-sources', 'PBXSourcesBuildPhase', [buildfile('driver-cpp', sources[1]), buildfile('driver-iig', sources[2])] + ([buildfile('settings-cpp', sources[3]), buildfile('settings-iig', sources[4])] if AUDIO else []))
driver_headers = phase('driver-headers', 'PBXHeadersBuildPhase')
driver_frameworks = phase('driver-frameworks', 'PBXFrameworksBuildPhase')
driver_resources = phase('driver-resources', 'PBXResourcesBuildPhase')
driver_configs = configs('Driver', {
    'SDKROOT': 'driverkit', 'SUPPORTED_PLATFORMS': 'driverkit',
    'DRIVERKIT_DEPLOYMENT_TARGET': '27.0', 'PRODUCT_NAME': '$(PRODUCT_BUNDLE_IDENTIFIER)',
    'PRODUCT_BUNDLE_IDENTIFIER': 'org.codaco.ClarettProbe.driver',
    'INFOPLIST_FILE': 'Driver/Info.plist', 'CODE_SIGN_ENTITLEMENTS': 'Driver/ClarettProbe.entitlements',
    'SKIP_INSTALL': 'YES', 'HEADER_SEARCH_PATHS': '$(SRCROOT)/Shared',
    'OTHER_LDFLAGS': ['-framework', 'DriverKit', '-framework', 'PCIDriverKit'] + (['-framework', 'AudioDriverKit'] if AUDIO else []),
    'GCC_ENABLE_CPP_EXCEPTIONS': 'NO', 'GCC_ENABLE_CPP_RTTI': 'NO',
    'ENABLE_APP_SANDBOX': 'YES',
})
driver = obj('driver-target', 'PBXNativeTarget', name='ClarettProbeDriver', productName='ClarettProbeDriver',
    buildConfigurationList=driver_configs, buildPhases=[driver_headers, driver_sources, driver_frameworks, driver_resources],
    buildRules=[], dependencies=[], productReference=dext, productType='com.apple.product-type.driver-extension')
proxy = obj('driver-proxy', 'PBXContainerItemProxy', containerPortal=ident('project'), proxyType='1', remoteGlobalIDString=driver, remoteInfo='ClarettProbeDriver')
dependency = obj('driver-dependency', 'PBXTargetDependency', target=driver, targetProxy=proxy)
embed_file = buildfile('embed-dext', dext)
objects[embed_file]['settings'] = {'ATTRIBUTES': ['CodeSignOnCopy', 'RemoveHeadersOnCopy']}
embed = phase('embed', 'PBXCopyFilesBuildPhase', [embed_file], name='Embed System Extensions',
              dstPath='$(CONTENTS_FOLDER_PATH)/Library/SystemExtensions', dstSubfolderSpec='16')
host_configs = configs('Host', {
    'SDKROOT': 'macosx', 'SUPPORTED_PLATFORMS': 'macosx', 'MACOSX_DEPLOYMENT_TARGET': '14.0',
    'PRODUCT_NAME': 'Clarett Probe', 'PRODUCT_BUNDLE_IDENTIFIER': 'org.codaco.ClarettProbe',
    'INFOPLIST_FILE': 'Host/Info.plist', 'CODE_SIGN_ENTITLEMENTS': 'Host/ClarettProbe.entitlements',
    'SWIFT_VERSION': '5.0', 'ENABLE_HARDENED_RUNTIME': 'YES',
    'OTHER_LDFLAGS': ['-framework', 'SystemExtensions', '-framework', 'IOKit', '-framework', 'CoreAudio'],
    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/../Frameworks'],
    'SWIFT_EMIT_LOC_STRINGS': 'NO',
})
host = obj('host-target', 'PBXNativeTarget', name='ClarettProbe', productName='Clarett Probe',
    buildConfigurationList=host_configs,
    buildPhases=[phase('host-sources', 'PBXSourcesBuildPhase', [buildfile('host-swift', sources[0])]),
                 phase('host-frameworks', 'PBXFrameworksBuildPhase'), phase('host-resources', 'PBXResourcesBuildPhase'), embed],
    buildRules=[], dependencies=[dependency], productReference=app, productType='com.apple.product-type.application')
obj('project', 'PBXProject', buildConfigurationList=project_configs, compatibilityVersion='Xcode 14.0',
    developmentRegion='en', hasScannedForEncodings='0', knownRegions=['en', 'Base'], mainGroup=main,
    productRefGroup=products, projectDirPath='', projectRoot='', targets=[host, driver],
    attributes={'LastUpgradeCheck': '2700', 'BuildIndependentTargetsInParallel': 'YES',
                'TargetAttributes': {host: {'CreatedOnToolsVersion': '27.0'}, driver: {'CreatedOnToolsVersion': '27.0'}}})

def serialize(value, indent=0):
    if isinstance(value, dict):
        return '{\n' + ''.join('\t'*(indent+1)+serialize(k)+' = '+serialize(v,indent+1)+';\n' for k,v in value.items()) + '\t'*indent + '}'
    if isinstance(value, list):
        return '(' + ', '.join(serialize(v, indent+1) for v in value) + ')'
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'

project = dict(archiveVersion='1', classes={}, objectVersion='56', objects=objects, rootObject=ident('project'))
(ROOT/'ClarettProbe.xcodeproj/project.pbxproj').write_text('// !$*UTF8*$!\n'+serialize(project)+'\n')
(ROOT/'ClarettProbe.xcodeproj/xcshareddata/xcschemes/ClarettProbe.xcscheme').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2700" version="1.3">
 <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries>
 <BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">
 <BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{host}" BuildableName="Clarett Probe.app" BlueprintName="ClarettProbe" ReferencedContainer="container:ClarettProbe.xcodeproj"/>
 </BuildActionEntry></BuildActionEntries></BuildAction>
 <LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="NO">
 <BuildableProductRunnable runnableDebuggingMode="0"><BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{host}" BuildableName="Clarett Probe.app" BlueprintName="ClarettProbe" ReferencedContainer="container:ClarettProbe.xcodeproj"/></BuildableProductRunnable>
 </LaunchAction>
 <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
''')
print('Generated ClarettProbe.xcodeproj')
