#!/usr/bin/env bash
#
# Generates include/asset_enums.h, the one thing the GameCube port needs from
# the decomp's asset tooling.
#
# Why this script exists, and why it wants Linux:
#
# The port ships the ROM's assets untouched, so it does not need the tool's
# extract/build round trip to produce an asset image -- `make -f Makefile.gc
# assets` copies the ROM directly. What it cannot avoid is asset_enums.h. Of
# the 352 ASSET_* symbols the game's C code refers to, only 128 can be derived
# from tools/dkr_assets_tool_extract.json; the remaining 224 (ASSET_MENU_TEXT_*,
# ASSET_FONTS_*, and friends) are named after strings and fonts that live inside
# the assets themselves, so they only exist once the assets have actually been
# extracted and parsed.
#
# dkr_assets_tool builds and runs on Windows, but extraction there completes
# silently without writing anything, and the upstream Makefile refuses Windows
# outright and points at WSL. Rather than chase that, this script runs the
# supported path.
#
# Run it from inside WSL (or any Linux box) with the repo as the working
# directory:
#
#     wsl
#     cd /mnt/c/Users/jacqu/Documents/DKR-GC/dkr
#     ./tools/gc/extract-asset-enums.sh
#
# Afterwards the Windows-side build works normally:
#
#     make -f Makefile.gc dist
#
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script needs Linux. Open WSL and run it from there:" >&2
    echo "  wsl" >&2
    echo "  cd /mnt/c/Users/jacqu/Documents/DKR-GC/dkr" >&2
    echo "  ./tools/gc/extract-asset-enums.sh" >&2
    exit 1
fi

cd "$(dirname "$0")/../.."
REPO="$PWD"
echo "repo: $REPO"

# ---- the ROM --------------------------------------------------------------
# Already in place and verified if the Windows side has been used; check anyway,
# because a wrong revision produces a header with the wrong enum values rather
# than an error, which would be a miserable thing to debug later.
EXPECTED_SHA1=0cb115d8716dbbc2922fda38e533b9fe63bb9670
ROM=$(ls baseroms/*.z64 2>/dev/null | head -1 || true)
if [[ -z "$ROM" ]]; then
    echo "error: no ROM in baseroms/. Put your own copy of DKR US 1.0 there." >&2
    exit 1
fi
ACTUAL_SHA1=$(sha1sum "$ROM" | cut -d' ' -f1)
if [[ "$ACTUAL_SHA1" != "$EXPECTED_SHA1" ]]; then
    echo "error: $ROM is not DKR US 1.0 (us.v77)." >&2
    echo "  expected $EXPECTED_SHA1" >&2
    echo "  got      $ACTUAL_SHA1" >&2
    echo "Other revisions work, but ASSETS_LUT_START/END in Makefile.gc are" >&2
    echo "specific to us.v77 and would need updating from ver/splat/." >&2
    exit 1
fi
echo "rom:  $ROM (us.v77, verified)"

# ---- dependencies ---------------------------------------------------------
# The package list is the repo's own, from README.md. pcre2 is optional -- the
# tool falls back to std::regex without it -- but it makes extraction markedly
# faster, so it is worth having.
if ! command -v g++ >/dev/null || ! command -v python3 >/dev/null; then
    echo "installing build dependencies (sudo)..."
    sudo apt-get update
    sudo apt-get install -y build-essential pkg-config git python3 python3-pip \
                            python3-venv libpcre2-dev libpcre2-8-0
fi

# ---- the tool -------------------------------------------------------------
echo "building dkr_assets_tool..."
make -C tools dkr_assets_tool -j"$(nproc)"

# ---- python side ----------------------------------------------------------
# A Linux venv of its own: the .venv the Windows side created holds Windows
# binaries and cannot be reused here.
if [[ ! -x .venv-linux/bin/python3 ]]; then
    echo "creating .venv-linux..."
    python3 -m venv .venv-linux
    .venv-linux/bin/python3 -m pip install --quiet --upgrade pip
    .venv-linux/bin/python3 -m pip install --quiet -r requirements.txt
fi
.venv-linux/bin/python3 ver/splat/update_baserom_names.py

# ---- extract and build ----------------------------------------------------
echo "splitting the rom..."
.venv-linux/bin/python3 -m splat split ver/splat/dkr.us.v77.yaml

echo "extracting assets..."
./tools/dkr_assets_tool extract -dkrv us.v77

# The build step is what writes include/asset_enums.h. Its binary output is not
# used by the port -- the asset image is the ROM itself -- so it goes to a
# scratch path.
echo "generating asset_enums.h..."
mkdir -p build/gc
./tools/dkr_assets_tool build -o build/gc/dkr.assets.rebuilt -dkrv us.v77

if [[ ! -f include/asset_enums.h ]]; then
    echo "error: include/asset_enums.h was not generated." >&2
    exit 1
fi

echo
echo "done: include/asset_enums.h ($(grep -c . include/asset_enums.h) lines)"
echo "Now build the port from Windows:  make -f Makefile.gc dist"
