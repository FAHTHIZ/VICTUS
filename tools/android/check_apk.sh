#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Checks a VICTUS Android APK without a device: required entries, the exported SDL_main entry
# point, 16 KB ELF segment alignment and 16 KB zip alignment of the uncompressed native libraries.
# Usage: tools/android/check_apk.sh app-debug.apk   (needs ANDROID_HOME; NDK r29 and Build Tools 36)
set -euo pipefail
apk="${1:?usage: check_apk.sh <apk>}"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$sdk" ] || { echo "set ANDROID_HOME" >&2; exit 2; }
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/29.0.14206865}"
readelf="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
zipalign="$sdk/build-tools/36.0.0/zipalign"
fail() { echo "check_apk: $*" >&2; exit 1; }

listing="$(unzip -l "$apk")"
for entry in classes.dex AndroidManifest.xml assets/game.vpak assets/THIRD_PARTY_NOTICES.txt \
             lib/arm64-v8a/libmain.so lib/arm64-v8a/libSDL3.so lib/arm64-v8a/libc++_shared.so; do
    grep -q " $entry\$" <<<"$listing" || fail "missing $entry"
done
# Native libraries and the pack must be stored (not deflated): 16 KB page-aligned mmap and fast streaming.
unzip -v "$apk" | awk '$NF ~ /^(lib\/.*\.so|assets\/game\.vpak)$/ && $2 != "Stored" { bad = bad " " $NF } END { if (bad) { print "compressed:" bad; exit 1 } }' \
    || fail "native libraries or the pack are compressed"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
unzip -q -o "$apk" 'lib/arm64-v8a/*.so' -d "$work"
"$readelf" --dyn-syms "$work/lib/arm64-v8a/libmain.so" | grep -E >/dev/null 'FUNC +GLOBAL +DEFAULT +[0-9]+ SDL_main$' \
    || fail "libmain.so does not export SDL_main"
"$readelf" --dyn-syms "$work/lib/arm64-v8a/libSDL3.so" | grep -E >/dev/null 'FUNC +GLOBAL +DEFAULT +[0-9]+ JNI_OnLoad(@|$)' \
    || fail "libSDL3.so does not export JNI_OnLoad (SDL registers its SDLActivity natives there)"
for so in "$work"/lib/arm64-v8a/*.so; do
    while read -r align; do
        [ $((align)) -ge 16384 ] || fail "$(basename "$so") has a LOAD segment aligned to $align (< 16 KB)"
    done < <("$readelf" -lW "$so" | awk '$1 == "LOAD" { print $NF }')
    "$readelf" -hW "$so" | grep >/dev/null 'Machine: *AArch64' || fail "$(basename "$so") is not arm64"
done
"$zipalign" -c -P 16 4 "$apk" || fail "APK is not 16 KB zip-aligned"
echo "check_apk: OK ($(du -h "$apk" | cut -f1)): $(grep -c ' lib/' <<<"$listing") native libraries, game.vpak, classes.dex"
