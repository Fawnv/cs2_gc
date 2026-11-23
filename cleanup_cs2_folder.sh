#!/usr/bin/env bash
# Removes every cs2_gc trace from the CS2 install folder on Linux:
#   game/bin/linuxsteamrt64/cs2_gc
#   game/bin/linuxsteamrt64/cs2_gc/
#   game/bin/linuxsteamrt64/cs2_gc_*.mdmp
# run_cs2_gc.sh calls this automatically when the game exits.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

if pgrep -x "cs2_gc_launcher" >/dev/null 2>&1 || pgrep -x "cs2_gc" >/dev/null 2>&1; then
    echo "CS2 with cs2_gc is still running. Close the game first."
    exit 1
fi

rm -f "$CS2_BIN/cs2_gc_launcher"
rm -f "$CS2_BIN/cs2_gc"
rm -rf "$CS2_BIN/cs2_gc"

# Crash dumps
if compgen -G "$CS2_BIN/cs2_gc_*.mdmp" > /dev/null; then
    mkdir -p "$SCRIPT_DIR/release/cs2_gc/crashes"
    mv -f "$CS2_BIN"/cs2_gc_*.mdmp "$SCRIPT_DIR/release/cs2_gc/crashes/" 2>/dev/null || true
    echo "Crash dump moved to $SCRIPT_DIR/release/cs2_gc/crashes"
fi

if [ "$1" != "--quiet" ] && [ "$1" != "-q" ]; then
    echo "CS2 folder is clean: no cs2_gc files left."
fi
