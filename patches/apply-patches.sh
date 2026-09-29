#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
fire_patches="$root/device/xiaomi/fire/patches"

apply_patch() {
    local repo="$1"
    local patch="$2"
    local description="$3"

    if git -C "$repo" apply --check "$patch" 2>/dev/null; then
        git -C "$repo" apply "$patch"
        echo "Applied $description"
    elif git -C "$repo" apply --reverse --check "$patch" 2>/dev/null; then
        echo "$description already applied"
    else
        echo "ERROR: $description does not match ${repo#"$root/"}" >&2
        exit 1
    fi
}

apply_patch \
    "$root/hardware/interfaces" \
    "$fire_patches/hardware_interfaces/0001-sensors-drop-unsupported-moisture-intrusion.patch" \
    "fire sensors patch"

apply_patch \
    "$root/frameworks/native" \
    "$fire_patches/frameworks_native/0001-libgui-feed-frames-to-mtk-fpsgo.patch" \
    "fire FPSGO libgui patch"

apply_patch \
    "$root/frameworks/base" \
    "$fire_patches/frameworks_base/0001-SystemUI-cache-custom-header-on-screen-on.patch" \
    "fire SystemUI custom header patch"
