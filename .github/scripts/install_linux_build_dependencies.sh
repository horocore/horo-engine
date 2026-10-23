#!/usr/bin/env bash
# Bound network acquisition separately from dpkg: a slow mirror must not hold
# every PR for minutes, and retrying a download must never interrupt installation.
set -euo pipefail

packages=(
  ninja-build ccache lld unzip python3-venv xvfb xauth libssl-dev
  libwayland-dev wayland-protocols libxkbcommon-dev
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
  libxext-dev libxss-dev libxtst-dev libxxf86vm-dev
  libgl1-mesa-dev libglu1-mesa-dev
  libvulkan1 mesa-vulkan-drivers vulkan-validationlayers vulkan-tools glslang-tools
)

distribution="$(lsb_release --id --short)"
codename="$(lsb_release --codename --short)"
architecture="$(dpkg --print-architecture)"
if [[ "$distribution" != Ubuntu || ! "$codename" =~ ^[a-z]+$ || "$architecture" != amd64 ]]; then
  echo "::error::Linux CI dependency installation requires an Ubuntu amd64 runner."
  exit 1
fi

# Scope sources and package indexes to this invocation. In particular, unrelated
# Azure CLI/Microsoft repositories cannot delay setup or need global deletion.
apt_root="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/horo-ci-apt.XXXXXX")"
trap 'sudo -n rm -rf -- "$apt_root"' EXIT
chmod 755 "$apt_root"
mkdir -p "$apt_root/archives/partial" "$apt_root/lists/partial"

apt_options=(
  -o "Dir::Etc::sourcelist=$apt_root/ubuntu.sources"
  -o 'Dir::Etc::sourceparts=-'
  -o "Dir::State::lists=$apt_root/lists"
  -o "Dir::Cache::archives=$apt_root/archives"
  -o 'APT::Update::Error-Mode=any'
  -o 'Acquire::Retries=0'
  -o 'Acquire::http::Timeout=20'
  -o 'Acquire::https::Timeout=20'
  -o 'Acquire::Languages=none'
)

# Probe-only capture, before the original EXIT cleanup removes private indexes.
python3 .github/scripts/apt_hosted_probe.py early "${apt_options[@]}" --packages "${packages[@]}"
probe_captured=false

report_phase() {
  local phase="$1" result="$2" elapsed="$3"
  echo "Linux dependencies: $phase: $result (${elapsed}s)"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '| %s | %s | %ss |\n' "$phase" "$result" "$elapsed" >> "$GITHUB_STEP_SUMMARY"
  fi
}

acquire() {
  local phase="$1" limit="$2" started="$SECONDS" result=0
  shift 2
  # GNU timeout runs the root-owned apt process in its own process group. Only
  # index/download operations are bounded; dpkg has not started at this point.
  sudo -n timeout --signal=TERM --kill-after=10s "$limit" apt-get "${apt_options[@]}" "$@" || result=$?
  report_phase "$phase" "$result" "$((SECONDS - started))"
  if [[ "$result" == 130 || "$result" == 143 ]]; then
    exit "$result"
  fi
  if [[ "$result" != 0 && "$phase" == *archives && "$probe_captured" == false ]]; then
    probe_captured=true
    printf '%s\n' "$result" > "$RUNNER_TEMP/apt-hosted-evidence/original-archive-exit.txt"
    python3 .github/scripts/apt_hosted_probe.py failure "${apt_options[@]}" --packages "${packages[@]}" || true
  fi
  return "$result"
}

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  printf '\n### Linux dependency setup\n\n| Phase | Exit status | Duration |\n| --- | --- | --- |\n' >> "$GITHUB_STEP_SUMMARY"
fi

# Both mirrors serve Ubuntu-signed indexes and packages. APT retains its normal
# signature/hash checks, including when completed archives survive a retry.
mirrors=(https://archive.ubuntu.com/ubuntu http://azure.archive.ubuntu.com/ubuntu)
downloaded=false
for mirror in "${mirrors[@]}"; do
  cat > "$apt_root/ubuntu.sources" <<EOF
Types: deb
URIs: $mirror
Suites: $codename $codename-updates $codename-backports $codename-security
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
  echo "Acquiring Linux dependencies from $mirror"
  if acquire "$mirror indexes" 45s update &&
     acquire "$mirror archives" 90s install --download-only --yes --no-install-recommends "${packages[@]}"; then
    downloaded=true
    break
  fi
  echo "::warning::Dependency acquisition failed or exceeded its budget at $mirror."
done

if [[ "$downloaded" != true ]]; then
  echo '::error::Neither Ubuntu mirror supplied all required dependencies within the network budget.'
  exit 1
fi

# Probe-only guard: this workflow must never execute dpkg or install packages.
printf '%s\n' 'archives acquired; no installation attempted in diagnostic probe' > "$RUNNER_TEMP/apt-hosted-evidence/acquisition-success.txt"
exit 0

# Do not retry a partially completed installation or silently fetch again. All
# required archives must already be present and verified by APT.
started="$SECONDS"
result=0
sudo -n env DEBIAN_FRONTEND=noninteractive apt-get "${apt_options[@]}" \
  install --no-download --yes --no-install-recommends "${packages[@]}" || result=$?
report_phase installation "$result" "$((SECONDS - started))"
exit "$result"
