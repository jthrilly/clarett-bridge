# Contributing

Start with the [architecture](docs/architecture.md), [build guide](docs/building.md) and [validation boundaries](docs/validation.md). Keep protocol/DMA changes separate from host presentation changes when possible. Preserve the distinction between request acceptance and verified applied state.

Run `bash scripts/test.sh`, type-check/build the host, and check generated project consistency with `python3 scripts/generate-project.py audio`. Hardware-affecting changes also require an appropriate signed build and a described manual test; CI is not hardware evidence.

Do not commit profiles, signing identities, private keys, personal device identifiers, local configuration or build products. Use your own developer team and App IDs. Never require reduced startup security, disabled SIP or unrestricted user-client entitlements to make a test pass.

Report the driver version, Mac/macOS, interface model, application, actual buffer/margin and observed result. Redact personal information from diagnostic attachments. Do not include microphone recordings unless you intend to share them.

Source changes are licensed GPL-2.0-only. Preserve provenance and license notices for referenced code. Do not include vendor firmware or proprietary driver binaries.
