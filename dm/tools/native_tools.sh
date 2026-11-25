#!/bin/sh
# Builds the repository's C# tool as a native executable (NativeAOT), universal (arm64 and x86_64) as spirv-cross is, so running it needs no .NET;
# building it needs the .NET SDK 10:
#   dmsh_xlate → dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate   the shader translator (Ryujinx's shader library, dm/third_party/ryujinx), part of the translation chain
# It is stamped with the digest of its sources, the vendored Ryujinx ones included (dmsh_chain.py --xlate-source-sha), which --source-sha prints: tool_lock.py refuses a
# translator not built from the sources in the repository.
# The build is not byte-reproducible (the linker gives each build its own UUID): a rebuild of the same sources has another fingerprint. So rebuild only after changing a tool's sources, then:
#   dmsh_xlate: tool_lock.py --update-lock and dmsh_chain.py --header, commit the binary with the lock and the header (a chain change: rebuild the shaders), and check that the translator's
#               output is the same as before for the shaders of the game (or say why it changes).
# Usage: native_tools.sh [dmsh_xlate]   Environment: WORK=folder for the intermediate files (default: a temporary folder)
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(mktemp -d -t native_tools)}
mkdir -p "$WORK"
command -v dotnet > /dev/null || { echo '[RED] building the native tools needs the .NET SDK 10 (brew install --cask dotnet-sdk); running them does not' >&2; exit 1; }

build() {   # <name> <csproj> <output executable> [msbuild property]
    for rid in osx-arm64 osx-x64; do   # NativeAOT compiles one architecture per publish; the SDK cross-compiles the other
        dotnet publish "$2" -c Release -r "$rid" -nologo -v quiet --artifacts-path "$WORK/$1-artifacts" -o "$WORK/$1-$rid" ${4:+"$4"} > "$WORK/$1-$rid.log" 2>&1 \
            || { tail -20 "$WORK/$1-$rid.log" >&2; echo "[RED] $1 did not build for $rid (log: $WORK/$1-$rid.log)" >&2; exit 1; }
        # A weak import is a libSystem function newer than the executable's minimum macOS: it binds to 0 on a Mac without it, and the runtime calls it unchecked. A .NET whose native
        # libraries were configured against the current SDK (a source-built one, such as Homebrew's formula) gives these (pipe2); Microsoft's SDK (the cask) does not
        weak=$(nm -m -u "$WORK/$1-$rid/$1" | grep -w weak || true)
        [ -z "$weak" ] || { echo "$weak" >&2; echo "[RED] $1 ($rid) imports functions its minimum macOS does not have, and would crash at address 0 there: build with Microsoft's .NET SDK (brew install --cask dotnet-sdk), not a source-built one" >&2; exit 1; }
        # A source-built .NET's runtime pack can link libraries of the machine that built it (nixpkgs' links its brotli from /nix/store), which no other Mac has
        foreign=$(otool -L "$WORK/$1-$rid/$1" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/Library/' || true)
        [ -z "$foreign" ] || { echo "$foreign" >&2; echo "[RED] $1 ($rid) links libraries outside macOS, which a player's Mac does not have: build with Microsoft's .NET SDK (nix develop, or brew install --cask dotnet-sdk), not a source-built one" >&2; exit 1; }
    done
    lipo -create "$WORK/$1-osx-arm64/$1" "$WORK/$1-osx-x64/$1" -output "$3"
    for a in arm64 x86_64; do sha=$(arch -"$a" "$3" --source-sha); echo "$1 ($a): source $sha"; done
    echo "$1: sha256 $(shasum -a 256 "$3" | cut -c1-64), $(wc -c < "$3" | tr -d ' ') bytes"
}

for t in ${*:-dmsh_xlate}; do
    case $t in
        dmsh_xlate)
            sha=$(python3 "$REPO/dm/gfx/tools/dmsh_chain.py" --xlate-source-sha)
            build dmsh_xlate "$REPO/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj" "$REPO/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate" "-p:XlateSourceSha=$sha" ;;
        *) echo "usage: native_tools.sh [dmsh_xlate]" >&2; exit 2 ;;
    esac
done
