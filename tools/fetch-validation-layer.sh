#!/usr/bin/env bash
# Optional glibc AArch64 VVL matching the pinned Debian Vulkan loader.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/tests/baseline/build/validation}"
mkdir -p "$dest"
package=vulkan-validationlayers_1.4.309.0-1_arm64.deb
digest=b81a8ada938d71a3344c52f77d94d031a2d7722fcbd066cca067c19124f312ee
if [[ ! -f "$dest/$package" ]]; then
    curl --fail --location --retry 2 \
        "https://snapshot.debian.org/archive/debian/20260801T000000Z/pool/main/v/vulkan-validationlayers/$package" \
        -o "$dest/$package.partial"
    mv "$dest/$package.partial" "$dest/$package"
fi
printf '%s  %s\n' "$digest" "$dest/$package" | sha256sum --check >&2
# Re-extract the checked package so modified extracted libraries are not reused.
scratch="$(mktemp -d "$dest/extract.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
if command -v dpkg-deb >/dev/null; then
    dpkg-deb -x "$dest/$package" "$scratch"
else
    image="$("$root/tools/ensure-builder.sh")"
    "${CONTAINER_ENGINE:-podman}" run --rm --userns=keep-id \
        --volume "$(cd "$dest" && pwd):/packages:Z" \
        "$image" dpkg-deb -x "/packages/$package" "/packages/$(basename "$scratch")"
fi
rm -rf "$dest/extracted"
mv "$scratch" "$dest/extracted"
trap - EXIT
printf '%s\n' "$dest/extracted"
