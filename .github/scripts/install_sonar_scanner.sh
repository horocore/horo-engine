#!/usr/bin/env bash
set -euo pipefail

scanner="sonar-scanner-${SONAR_SCANNER_VERSION:?}-linux-x64"
if [[ -x "$SONAR_USER_HOME/$scanner/bin/sonar-scanner" ]]; then
  exit 0
fi

mkdir -p "$SONAR_USER_HOME"
curl --proto '=https' --proto-redir '=https' --retry 3 -fsSL \
  "https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/${scanner}.zip" \
  -o /tmp/sonar-scanner.zip
unzip -q /tmp/sonar-scanner.zip -d "$SONAR_USER_HOME"