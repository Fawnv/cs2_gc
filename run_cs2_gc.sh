#!/usr/bin/env bash
# Starts CS2 with cs2_gc on Linux (always -insecure, VAC off) and cleans up after it.
# Normal CS2 is unaffected: launch it from Steam as usual.
#
# cs2_gc has to sit next to cs2 to start the engine, so it is copied
# there only for this session and removed when the game closes.
# All gc data (config, inventory, log, editor) stays in release/cs2_gc.
# Keep this terminal open while playing. If it gets closed early, run
# cleanup_cs2_folder.sh afterwards.
# Extra launch options are passed through, e.g. ./run_cs2_gc.sh -novid -console
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Edit if CS2 lives somewhere else
if [ -z "$CS2_ROOT" ]; then
    if [ -d "$HOME/.steam/steam/steamapps/common/Counter-Strike Global Offensive" ]; then
        CS2_ROOT="$HOME/.steam/steam/steamapps/common/Counter-Strike Global Offensive"
    elif [ -d "$HOME/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive" ]; then
        CS2_ROOT="$HOME/.local/share/Steam/steamapps/common/Counter-Strike Global Offensive"
    else
        CS2_ROOT="$HOME/.steam/root/steamapps/common/Counter-Strike Global Offensive"
    fi
fi

CS2_BIN="$CS2_ROOT/game/bin/linuxsteamrt64"

if [ -d "$SCRIPT_DIR/release_linux" ]; then
    GC_RELEASE="$SCRIPT_DIR/release_linux"
else
    GC_RELEASE="$SCRIPT_DIR/release"
fi

GC_DATA="$GC_RELEASE/cs2_gc"

if [ ! -f "$CS2_BIN/cs2" ]; then
    echo "CS2 not found in '$CS2_BIN'. Set CS2_ROOT environment variable before running."
    exit 1
fi

LAUNCHER_BIN="cs2_gc_launcher"
if [ ! -f "$GC_RELEASE/$LAUNCHER_BIN" ]; then
    if [ -f "$GC_RELEASE/cs2_gc" ]; then
        LAUNCHER_BIN="cs2_gc"
    else
        echo "cs2_gc_launcher not found in '$GC_RELEASE'. Build it first using ./build.sh."
        exit 1
    fi
fi

cp -f "$GC_RELEASE/$LAUNCHER_BIN" "$CS2_BIN/$LAUNCHER_BIN"
chmod +x "$CS2_BIN/$LAUNCHER_BIN"

echo "CS2 with cs2_gc is running. Inventory editor: http://127.0.0.1:13001/"
echo "Do not close this window: cs2_gc files are removed from the CS2 folder when the game exits."

# Environment settings matching Valve's cs2.sh
ulimit -n 65535 2>/dev/null || true
ulimit -Ss 2048 2>/dev/null || true
export ENABLE_PATHMATCH=1
export LD_LIBRARY_PATH="$CS2_BIN:${LD_LIBRARY_PATH}"

# Run launcher
"$CS2_BIN/$LAUNCHER_BIN" -insecure -gcdata "$GC_DATA" "$@" || true

# Cleanup
"$SCRIPT_DIR/cleanup_cs2_folder.sh" --quiet
