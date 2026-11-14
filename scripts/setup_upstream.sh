#!/usr/bin/env bash
# Fetches CBFD-Recompiled at the pinned commit, builds N64Recomp, recompiles your ROM into C and
# applies this project's PS3 patches. Output: upstream/CBFD-Recompiled/RecompiledFuncs/
#
#   scripts/setup_upstream.sh /path/to/conker.z64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${1:?usage: $0 /path/to/Conker\'s Bad Fur Day (U).z64}"
CBFD_COMMIT=ed1b8bb
UP="$ROOT/upstream/CBFD-Recompiled"

sha=$(shasum -a 1 "$ROM" 2>/dev/null || sha1sum "$ROM"); sha=${sha%% *}
[ "$sha" = "4cbadd3c4e0729dec46af64ad018050eada4f47a" ] || { echo "Wrong ROM (need US .z64), SHA-1 $sha"; exit 1; }

if [ ! -d "$UP/.git" ]; then
  git clone https://github.com/sciaschi/CBFD-Recompiled.git "$UP"
fi
git -C "$UP" checkout -q "$CBFD_COMMIT"
git -C "$UP" submodule update --init --recursive tools/N64Recomp tools/N64ModernRuntime

cd "$UP/tools/N64Recomp"
git checkout -q -- .
git apply ../../recomp/n64recomp.patch
git apply "$ROOT/patches/n64recomp-ps3.patch"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target N64RecompCLI RSPRecomp

cd "$UP/tools/N64ModernRuntime"
git checkout -q -- . && git clean -qfd
git apply ../../recomp/n64modernruntime.patch
git apply "$ROOT/patches/n64modernruntime-ps3.patch"
git apply "$ROOT/patches/n64modernruntime-ps3-saves.patch"
git apply "$ROOT/patches/n64modernruntime-ps3-order.patch"
# The runtime carries its own copy of N64Recomp's recomp.h: patch it the same way.
git -C N64Recomp checkout -q -- include/recomp.h
git -C N64Recomp apply "$ROOT/patches/n64recomp-ps3.patch"

cd "$UP"
git checkout -q -- host/src
git apply "$ROOT/patches/cbfd-host-ps3.patch"
cp "$ROM" conker/baserom.us.z64
python3 recomp/recompile.py
echo "Recompiled C is in $UP/RecompiledFuncs"
