#!/usr/bin/env bash
set -euo pipefail

scanner="sonar-scanner-${SONAR_SCANNER_VERSION:?}-linux-x64"
if [[ -x "$SONAR_USER_HOME/$scanner/bin/sonar-scanner" ]]; then
  exit 0
fi

mkdir -p "$SONAR_USER_HOME"
download_directory=$(mktemp -d)
trap 'rm -rf "$download_directory"' EXIT
curl --proto '=https' --proto-redir '=https' --retry 3 -fsSL \
  "https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/sonar-scanner-cli-${SONAR_SCANNER_VERSION}-linux-x64.zip" \
  -o "$download_directory/sonar-scanner.zip"
unzip -q "$download_directory/sonar-scanner.zip" -d "$SONAR_USER_HOME"
