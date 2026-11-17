#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Installs the system packages needed to build and test VICTUS from source.
# Everything else (SDL3, Luau, Jolt, Box2D, ...) is fetched and hash-checked by CMake.
#
#   tools/setup/install_deps.sh            # install build + test packages
#   tools/setup/install_deps.sh --dry-run  # print the command only
#   tools/setup/install_deps.sh --tidy     # also install clang/clang-tidy for static analysis
#
# Verified: Ubuntu 26.04 (this list is the one CI uses on ubuntu-latest). Fedora and macOS
# (Homebrew) lists are maintained by hand and not exercised in CI; please report problems.
set -euo pipefail

dry_run=0
tidy=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) dry_run=1 ;;
        --tidy) tidy=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 64 ;;
    esac
done

run() {
    echo "+ $*"
    if [ "$dry_run" -eq 0 ]; then "$@"; fi
}

os="$(uname -s)"
if [ "$os" = "Darwin" ]; then
    command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh" >&2; exit 1; }
    pkgs=(cmake ninja python3)
    [ "$tidy" -eq 1 ] && pkgs+=(llvm)
    run brew install "${pkgs[@]}"
    echo "Xcode command line tools provide the compiler (xcode-select --install)."
    exit 0
fi

if command -v apt-get >/dev/null; then
    # Keep in sync with LINUX_DEPS in .github/workflows/ci.yml.
    pkgs=(build-essential cmake ninja-build python3 git
          libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
          libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev
          libdecor-0-dev libasound2-dev libpulse-dev libdrm-dev libgbm-dev libudev-dev
          mesa-vulkan-drivers libvulkan1 xvfb)
    [ "$tidy" -eq 1 ] && pkgs+=(clang clang-tidy clang-format)
    sudo_cmd=()
    [ "$(id -u)" -ne 0 ] && sudo_cmd=(sudo)
    run "${sudo_cmd[@]}" apt-get update
    run "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${pkgs[@]}"
elif command -v dnf >/dev/null; then
    pkgs=(gcc-c++ cmake ninja-build python3 git
          libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel
          libXScrnSaver-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel
          mesa-libEGL-devel libdecor-devel alsa-lib-devel pulseaudio-libs-devel libdrm-devel
          mesa-libgbm-devel systemd-devel mesa-vulkan-drivers vulkan-loader xorg-x11-server-Xvfb)
    [ "$tidy" -eq 1 ] && pkgs+=(clang clang-tools-extra)
    sudo_cmd=()
    [ "$(id -u)" -ne 0 ] && sudo_cmd=(sudo)
    run "${sudo_cmd[@]}" dnf install -y "${pkgs[@]}"
else
    echo "Unsupported package manager. Install CMake >= 3.24, Ninja, a C++20 compiler (GCC 13+, Clang 17+)," >&2
    echo "Python 3.10+, and the X11/Wayland/ALSA/PulseAudio development headers SDL3 needs." >&2
    exit 1
fi
echo "Done. Next: tools/setup/check_env.py, then cmake --preset dev && cmake --build --preset dev"
