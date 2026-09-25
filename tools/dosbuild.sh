#!/bin/sh
# dosbuild.sh — drive the 1995 Microsoft toolchain non-interactively under DOSBox-X.
#
# The engine was linked with LINK 5.50, which ships in Visual C++ 1.0 (VC++ 1.5
# and 1.52 ship 5.60, which is what built the game's SETUP.EXE but not the
# engine). See docs/PORT_PLAN.md.
#
# Usage:  tools/dosbuild.sh <srcdir> <command-file>
#
#   srcdir        host directory mounted as C:, holding the sources and
#                 receiving the objects. Must be writable: the Phar Lap
#                 extender that CL and LINK run under needs somewhere to put
#                 its swap file, and pointing TMP at the read-only CD fails
#                 with "Phar Lap err 58: Can't create VM swap file".
#   command-file  DOS commands to run, one per line, relative to C:.
#
# Environment:
#   VC100_ISO     path to the Visual C++ 1.0 ISO   (default: guest/toolchain/VC100PRO.ISO)
#   DOSBOX        dosbox-x binary                  (default: dosbox-x)
set -e

SRC=$1
CMDS=$2
[ -n "$SRC" ] && [ -n "$CMDS" ] || { echo "usage: dosbuild.sh <srcdir> <command-file>" >&2; exit 2; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO=${VC100_ISO:-$ROOT/guest/toolchain/VC100PRO.ISO}
DOSBOX=${DOSBOX:-dosbox-x}
SRC=$(cd "$SRC" && pwd)

[ -f "$ISO" ] || { echo "dosbuild: no VC++ 1.0 ISO at $ISO" >&2; exit 1; }

CONF=$(mktemp -t dosbuild)
{
    echo "[dosbox]"
    echo "memsize=32"
    echo "[cpu]"
    echo "cputype=pentium"
    echo "core=normal"
    echo "cycles=max"
    echo "[dos]"
    echo "ver=6.22"
    echo "[autoexec]"
    echo "mount c $SRC"
    echo "imgmount d $ISO -t iso"
    echo "c:"
    # Phar Lap swap must land on the writable drive, not the CD.
    echo "set TMP=C:\\"
    echo "set TEMP=C:\\"
    echo "set PATH=D:\\MSVC\\BIN"
    echo "set INCLUDE=D:\\MSVC\\INCLUDE"
    echo "set LIB=D:\\MSVC\\LIB"
    cat "$CMDS"
    echo "exit"
} > "$CONF"

# -silent is load-bearing: without it DOSBox-X blocks on its configuration-tool
# GUI and never reaches AUTOEXEC, burning the timeout at 0% CPU.
"$DOSBOX" -conf "$CONF" -silent -nogui -nomenu >/dev/null 2>&1
rm -f "$CONF"
