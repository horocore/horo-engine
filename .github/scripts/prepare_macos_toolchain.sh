#!/bin/bash
set -euo pipefail

if [[ "${RUNNER_OS:-}" != macOS ]]; then
    echo '::error::The macOS qualification runner must be a native Mac.'
    exit 1
fi

# Xcode can ship an executable stub without its optional Metal Toolchain.
if xcrun --sdk macosx metal --version; then
    exit 0
fi

# Hosted machines are ephemeral; custom qualification hosts are provisioned by
# their operator and must not be modified implicitly by repository jobs.
if [[ "${RUNNER_ENVIRONMENT:-}" != github-hosted ]]; then
    echo '::error::Provision this Mac with xcodebuild -downloadComponent MetalToolchain.'
    exit 1
fi

xcodebuild -downloadComponent MetalToolchain
xcrun --sdk macosx metal --version
