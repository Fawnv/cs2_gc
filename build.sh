#!/usr/bin/env bash
set -e

# Builds cs2_gc on Linux (x64)
# Usage:
#   ./build.sh            Build Release binaries
#   ./build.sh --package  Build and pack release archives (.tar.gz and .zip)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="Release"
PACKAGE=false

for arg in "$@"; do
    case "$arg" in
        --package|-p)
            PACKAGE=true
            ;;
        Debug|Release|RelWithDebInfo)
            BUILD_TYPE="$arg"
            ;;
        *)
            ;;
    esac
done

echo "Building cs2_gc ($BUILD_TYPE)..."

cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build_linux" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DOUTDIR="$SCRIPT_DIR/release_linux"

cmake --build "$SCRIPT_DIR/build_linux" --config "$BUILD_TYPE"

if [ "$PACKAGE" = true ]; then
    echo ""
    echo "Packaging release..."
    python3 "$SCRIPT_DIR/tools/make_release_zip.py" --bin "$SCRIPT_DIR/release_linux/cs2_gc_launcher"
fi

echo ""
echo "Done. Output is in $SCRIPT_DIR/release_linux"
