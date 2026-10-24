#!/usr/bin/env bash
#
# Builds the local components/bt from the pinned ESP-IDF plus this project's
# patches. The Bash twin of tools/setup.ps1, for Linux and the CI container:
# the same five steps, the same refusals, see docs/BASE.md.
#
# The shared IDF at $IDF_PATH is never written to. It is read, copied out of,
# and left exactly as it was found.
#
#   tools/setup.sh               refuses to touch an existing components/bt
#   tools/setup.sh --clean       discards it and copies again (patches do not apply twice)
#   tools/setup.sh --with-lhdc   also fetches the LHDC V5 decoder named in lhdc.pin

set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bt_local="$repo/components/bt"
patch_dir="$repo/patches"
pin_file="$repo/idf.pin"
lhdc_local="$repo/components/lhdcv5_decoder"
lhdc_pin="$repo/lhdc.pin"

fail() { echo; echo "setup: $*" >&2; exit 1; }

clean=0
with_lhdc=0
for arg in "$@"; do
    case "$arg" in
        --clean) clean=1 ;;
        --with-lhdc) with_lhdc=1 ;;
        *) fail "unknown option $arg. Known: --clean, --with-lhdc." ;;
    esac
done

# --- 1. The pinned IDF must be the one that is active ---------------------------

[[ -n "${IDF_PATH:-}" ]] || fail 'IDF_PATH is not set. Activate ESP-IDF first (see CLAUDE.md, section Build).'
[[ -f "$pin_file" ]] || fail "idf.pin not found at $pin_file"

pin_version=$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*$/\1/p' "$pin_file" | tr -d '\r' | head -n1)
pin_commit=$(sed -n 's/^[[:space:]]*commit[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*$/\1/p' "$pin_file" | tr -d '\r' | head -n1)
[[ -n "$pin_version" && -n "$pin_commit" ]] || fail 'idf.pin must define both version= and commit=.'

version_file="$IDF_PATH/tools/cmake/version.cmake"
[[ -f "$version_file" ]] || fail "No tools/cmake/version.cmake under $IDF_PATH. That does not look like an ESP-IDF."
v_major=$(sed -n 's/^[[:space:]]*set(IDF_VERSION_MAJOR[[:space:]]*\([0-9]*\)).*/\1/p' "$version_file" | head -n1)
v_minor=$(sed -n 's/^[[:space:]]*set(IDF_VERSION_MINOR[[:space:]]*\([0-9]*\)).*/\1/p' "$version_file" | head -n1)
v_patch=$(sed -n 's/^[[:space:]]*set(IDF_VERSION_PATCH[[:space:]]*\([0-9]*\)).*/\1/p' "$version_file" | head -n1)
actual_version="v$v_major.$v_minor.$v_patch"
[[ "$actual_version" == "$pin_version" ]] || \
    fail "IDF_PATH is $actual_version, idf.pin wants $pin_version. Patches are cut against $pin_version only."

# The tag can be moved; the commit cannot. Check it when the tree is a git checkout.
if actual_commit=$(git -C "$IDF_PATH" rev-parse HEAD 2>/dev/null); then
    [[ "$actual_commit" == "$pin_commit" ]] || fail "IDF_PATH is at commit $actual_commit, idf.pin wants $pin_commit."
else
    echo 'setup: IDF_PATH is not a git checkout, commit not verified.'
fi
echo "setup: IDF $pin_version at $IDF_PATH - matches idf.pin."

# --- 2. Copy components/bt out of the shared tree -------------------------------

bt_source="$IDF_PATH/components/bt"
[[ -d "$bt_source" ]] || fail "No bt component at $bt_source."

if [[ -e "$bt_local" ]]; then
    (( clean )) || fail 'components/bt already exists. Re-run with --clean to rebuild it: patches do not apply twice.'
    echo 'setup: removing the old components/bt ...'
    rm -rf "$bt_local"
fi
mkdir -p "$repo/components"
echo 'setup: copying components/bt (about 71 MB, takes a moment) ...'
cp -R "$bt_source" "$bt_local"

# --- 3. Apply our patches on top ------------------------------------------------

# Plain `git apply`, not --3way: components/bt is ignored, its blobs are not in
# this repository, and git has nothing to merge against. It fails loudly on the
# first hunk that does not fit, which is the property wanted (docs/BASE.md).
shopt -s nullglob
patches=("$patch_dir"/*.patch)
shopt -u nullglob
if (( ${#patches[@]} == 0 )); then
    echo 'setup: no patches yet - this is stock Bluedroid, which means SBC only.'
else
    for patch in "${patches[@]}"; do
        echo "setup: applying $(basename "$patch") ..."
        git -C "$repo" apply --directory=components/bt --whitespace=nowarn -- "$patch" || \
            fail "$(basename "$patch") did not apply. Do not hand-edit components/bt - fix the patch and re-run with --clean."
    done
    echo "setup: ${#patches[@]} patch(es) applied."
fi

# --- 4. Lay the new files on top ------------------------------------------------

# Files that are entirely ours -- the codec framework, the decoders, libfreeaptx,
# Helix -- live in bt_overlay/ as real source and are copied in here (docs/BASE.md).
overlay="$repo/bt_overlay"
if [[ -d "$overlay" ]]; then
    count=$(find "$overlay" -type f | wc -l | tr -d ' ')
    cp -R "$overlay/." "$bt_local/"
    echo "setup: $count file(s) copied from bt_overlay."
else
    echo 'setup: no bt_overlay - only the patches were applied.'
fi

# --- 5. The LHDC V5 decoder, only when asked for ---------------------------------

# Not carried in this repository; lhdc.pin says why. A shallow, blobless fetch of
# the pinned commit brings its trees and no file contents, and the checkout of the
# three paths that are built then downloads only theirs: about 2 MB of a 110 MB
# repository. Fetching by full commit id is itself the check that it is the pin.
if (( with_lhdc )); then
    [[ -f "$lhdc_pin" ]] || fail "lhdc.pin not found at $lhdc_pin"
    lhdc_url=$(sed -n 's/^[[:space:]]*url[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*$/\1/p' "$lhdc_pin" | tr -d '\r' | head -n1)
    lhdc_commit=$(sed -n 's/^[[:space:]]*commit[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*$/\1/p' "$lhdc_pin" | tr -d '\r' | head -n1)
    [[ -n "$lhdc_url" && -n "$lhdc_commit" ]] || fail 'lhdc.pin must define both url= and commit=.'

    fetch_dir=$(mktemp -d)
    trap 'rm -rf "$fetch_dir"' EXIT
    echo "setup: fetching the LHDC V5 decoder at ${lhdc_commit:0:12} from $lhdc_url ..."
    git -C "$fetch_dir" init -q
    git -C "$fetch_dir" remote add origin "$lhdc_url"
    git -C "$fetch_dir" fetch -q --depth 1 --filter=blob:none origin "$lhdc_commit" || \
        fail "could not fetch $lhdc_commit from $lhdc_url."
    git -C "$fetch_dir" checkout -q FETCH_HEAD -- CMakeLists.txt README.md decoder || \
        fail "the pinned commit has no CMakeLists.txt, README.md and decoder/."

    rm -rf "$lhdc_local"
    mkdir -p "$lhdc_local"
    cp -R "$fetch_dir/CMakeLists.txt" "$fetch_dir/README.md" "$fetch_dir/decoder" "$lhdc_local/"
    count=$(find "$lhdc_local" -type f | wc -l | tr -d ' ')
    echo "setup: $count file(s) of the LHDC V5 decoder in components/lhdcv5_decoder."
elif [[ -d "$lhdc_local" ]]; then
    echo 'setup: components/lhdcv5_decoder, from an earlier --with-lhdc, left as it is.'
fi

# --- 6. Say what to do next -----------------------------------------------------

echo
echo 'setup: done. Next:'
echo '    idf.py set-target esp32'
echo '    idf.py build'
if (( with_lhdc )); then
    echo 'setup: LHDC V5 is built only with sdkconfig.lhdc, from a fresh sdkconfig:'
    echo '    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lhdc" set-target esp32'
fi
