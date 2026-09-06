#!/usr/bin/env bash
# Print the repository-owned, content-tagged AArch64 builder image.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
engine="${CONTAINER_ENGINE:-podman}"
recipe="$root/tools/container/Containerfile.aarch64"
recipe_hash="$(sha256sum "$recipe" | cut -d' ' -f1)"
image="localhost/libhybris-aarch64:$recipe_hash"
if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    "$engine" build --network=host --tag "$image" --file "$recipe" \
        "$root/tools/container" >&2
fi
printf '%s\n' "$image"
