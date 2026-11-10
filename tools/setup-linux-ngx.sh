#!/usr/bin/env bash
# Optional personal-use NVIDIA runtime setup. No NVIDIA binaries ship in the AppImage.
set -euo pipefail
commit=9981c6350dc58c0ce21f024fd65e31e851c31689
base="https://raw.githubusercontent.com/NVIDIA/DLSS/$commit"
if [[ "${1:-}" != --accept-nvidia-license ]]; then
  printf '%s\n' 'Review the NVIDIA RTX SDK license before proceeding:' "$base/LICENSE.txt" \
    'This downloads NVIDIA proprietary runtime components for your own local use.' \
    'After reviewing/accepting those terms, run this script with --accept-nvidia-license.'
  exit 2
fi
command -v curl >/dev/null
command -v c++ >/dev/null || { echo 'A C++ compiler is needed to build the official SDK loader shared library.' >&2; exit 1; }
dest="${EDEN_NGX_INSTALL_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/eden/ngx}"
mkdir -p "$dest"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl --fail --location --proto '=https' "$base/LICENSE.txt" -o "$work/NVIDIA-RTX-SDK-LICENSE.txt"
curl --fail --location --proto '=https' "$base/lib/Linux_x86_64/libnvsdk_ngx.a" -o "$work/libnvsdk_ngx.a"
curl --fail --location --proto '=https' "$base/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16" -o "$work/libnvidia-ngx-dlss.so.310.9.16"
(cd "$work" && printf '%s\n' \
 'd3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df  libnvsdk_ngx.a' \
 '30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6  libnvidia-ngx-dlss.so.310.9.16' | sha256sum -c -)
c++ -shared -o "$work/libnvsdk_ngx.so" -Wl,--whole-archive "$work/libnvsdk_ngx.a" -Wl,--no-whole-archive -ldl -lpthread
for f in libnvsdk_ngx.so libnvidia-ngx-dlss.so.310.9.16 NVIDIA-RTX-SDK-LICENSE.txt; do
  if [[ -e "$dest/$f" ]]; then
    echo "Already exists; not overwriting: $dest/$f" >&2
  else
    install -m 0644 "$work/$f" "$dest/$f"
  fi
done
printf 'Installed optional NVIDIA runtime in %s\n' "$dest"
printf '%s\n' 'The NVIDIA driver (libnvidia-ngx.so.*) must also be installed; no driver is bundled.' \
  'Restart the game after enabling NVIDIA DLSS in Graphics settings.'
