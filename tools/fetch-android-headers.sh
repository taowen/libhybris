#!/usr/bin/env bash
# Fetch the Android 11 header revision used by this repository.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
revision=2c6ac3dcc4f8db593dd69906b0ec22822abfed91
repository=https://github.com/Halium/android-headers.git
cache="${HYBRIS_DEPS_DIR:-$root/tests/baseline/build/deps}"
dest="$cache/android-headers-$revision"
if [[ ! -e "$dest" ]]; then
    mkdir -p "$cache"
    scratch="$(mktemp -d "$cache/headers.XXXXXX")"
    trap 'rm -rf "$scratch"' EXIT
    git init -q "$scratch/repo"
    git -C "$scratch/repo" fetch --depth=1 "$repository" "$revision" >&2
    [[ "$(git -C "$scratch/repo" rev-parse FETCH_HEAD)" == "$revision" ]]
    mkdir "$scratch/headers"
    git -C "$scratch/repo" archive FETCH_HEAD | tar -x -C "$scratch/headers"
    printf '%s\n' "$repository $revision" > "$scratch/headers/SOURCE_REVISION"
    mv "$scratch/headers" "$dest"
fi
# Check cached contents too, including modes and symlink targets.
actual="$(python3 - "$root/tools" "$dest" <<'PYTHON'
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
from build_inputs import tree_identity
print(tree_identity(Path(sys.argv[2]))['sha256'])
PYTHON
)"
expected=fd30a127eb85c3258cf471906cb002b1f1e110acdccb6247c5dc3aee3a1ddffc
if [[ "$actual" != "$expected" ]]; then
    echo "Android header cache has unexpected contents: $dest" >&2
    echo "Use --headers for custom headers, or remove this cache to fetch it again." >&2
    exit 2
fi
printf '%s\n' "$dest"
