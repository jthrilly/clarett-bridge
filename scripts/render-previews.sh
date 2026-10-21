#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mkdir -p build/tools
xcrun swiftc -parse-as-library -module-cache-path build/ModuleCache \
  Host/BridgeController.swift Host/BridgeView.swift Tools/RenderAssets.swift \
  -o build/tools/render-assets
build/tools/render-assets --icon build/ClarettBridge.iconset
iconutil -c icns build/ClarettBridge.iconset -o Host/Resources/ClarettBridge.icns
build/tools/render-assets --previews docs/images
