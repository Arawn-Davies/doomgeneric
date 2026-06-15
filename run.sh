#!/usr/bin/env bash
#
# Build (optionally) + launch + debug a PS2 build in Windows PCSX2, from WSL.
#
# Mirrors /ps2-run: name a renderer/disc config and run.sh builds it (via
# build.sh) and then boots it. Or hand it an already-built .elf/.iso to just
# launch. Boots with -fastboot, writes PCSX2's log to the WAD folder, then
# live-tails it filtered to the lines that actually matter (the IOP console:
# disc/WAD scan, audsrv bring-up, video mode, pad, EE exceptions, and the
# `# Restart.` LoadExec markers). NB: our EE printf goes to the on-screen GS
# console, NOT this log -- the log is the IOP side + EE exceptions.
#
# Usage (same renderer/disc vocabulary as /ps2-run):
#   ./run.sh gs                   # JUST LAUNCH the existing standalone ELF (bin/ps2oom.elf), no build
#   ./run.sh gl                   #   "   (gl/sdl behave the same -- they boot whatever standalone ELF exists)
#   ./run.sh sdl                  #   "
#   ./run.sh gs usb               # BUILD gsKit standalone USB ELF (embedgs) then launch
#   ./run.sh gl usb               # BUILD GL   standalone USB ELF then launch
#   ./run.sh sdl usb              # BUILD SDL  standalone USB ELF then launch
#   ./run.sh iso                  # build full 3-renderer launcher ISO + launch
#   ./run.sh fastiso              # build fast SDL+gsKit launcher ISO + launch
#   ./run.sh freeiso              # build Freedoom-only launcher ISO + launch
#   ./run.sh bin/ps2oom.elf       # launch a pre-built ELF (copied to the PCSX2 folder)
#   ./run.sh path/to/foo.iso      # launch a pre-built ISO
#   ./run.sh --log                # don't launch; just summarise the existing log
#   ./run.sh -h | --help          # show this usage  (bare `./run.sh` does the same)
#
# So a bare renderer (gs/gl/sdl) = standalone-launch mode (no build); add `usb`
# to (re)build the standalone ELF first. iso/fastiso/freeiso always build.
# Override paths via env: PCSX2=... OUTDIR=...
set -uo pipefail

PCSX2="${PCSX2:-/mnt/c/Program Files/PCSX2/pcsx2-qt.exe}"
OUTDIR="${OUTDIR:-/mnt/c/Users/azama/Downloads/doom}"   # Windows-visible; WADs live here
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$OUTDIR/pcsx2.log"

# Lines worth seeing while debugging (IOP console + EE exceptions).
PATTERNS='cdvd|iso|SYSTEM\.CNF|cdrom0|DOOMSDL|\.ELF|cdfs:/|IWAD|PWAD|FREESD|libsd|audsrv|mc:|Mode Changed|GS CRTC|target speed|Pad:|# Restart|exception|tlb|address error|DOOM ERROR|EE'

usage() { sed -n '2,/^set /p' "$0" | sed 's/^# \{0,1\}//; /^set /d'; }

show_summary() {
  if [[ ! -f "$LOG" ]]; then echo "no log yet at $LOG"; exit 1; fi
  echo ">> summary of $LOG"
  grep -inE "$PATTERNS" "$LOG" | tail -40
}

# Build the requested /ps2-run config (build.sh) and set TARGET to its artifact.
# USB/embed configs -> bin/ps2oom.elf ; disc configs -> bin/ps2oom.iso.
build_cfg() {
  case "$1" in
    gs|gskit|embedgs)
      echo ">> building gsKit standalone USB ELF (embedgs) ..."
      "$HERE/build.sh" embedgs || exit 1
      TARGET="$HERE/bin/ps2oom.elf" ;;
    gl)
      echo ">> building GL standalone USB ELF (clean + GL_VIDEO/EMBED/STRAIGHT) ..."
      "$HERE/build.sh" clean || exit 1
      "$HERE/build.sh" GL_VIDEO=1 EMBED_WAD=1 BOOT_STRAIGHT=1 || exit 1
      TARGET="$HERE/bin/ps2oom.elf" ;;
    sdl)
      echo ">> building SDL standalone USB ELF (clean + EMBED/STRAIGHT) ..."
      "$HERE/build.sh" clean || exit 1
      "$HERE/build.sh" EMBED_WAD=1 BOOT_STRAIGHT=1 || exit 1
      TARGET="$HERE/bin/ps2oom.elf" ;;
    iso|fastiso|freeiso)
      echo ">> building $1 launcher disc ..."
      "$HERE/build.sh" "$1" || exit 1
      TARGET="$HERE/bin/ps2oom.iso" ;;
    *) return 1 ;;
  esac
}

case "${1:-}" in
  ''|-h|--help) usage; exit 0 ;;   # bare run.sh shows usage (no implicit build/boot)
  --log)        show_summary; exit 0 ;;
esac

# Dispatch:
#   gs/gl/sdl ALONE        -> standalone-launch mode: boot the existing bin/ps2oom.elf, no build
#   gs/gl/sdl + usb (or any 2nd token) -> (re)build the standalone ELF, then launch
#   iso/fastiso/freeiso    -> always build the disc, then launch
#   anything else          -> treat as a pre-built .elf/.iso path to launch directly
case "${1:-}" in
  gs|gl|sdl)
    if [[ -n "${2:-}" ]]; then
      build_cfg "$1"
    else
      TARGET="$HERE/bin/ps2oom.elf"
      if [[ ! -f "$TARGET" ]]; then
        echo "!! no standalone ELF at $TARGET -- build one first:  ./run.sh $1 usb"
        exit 1
      fi
      echo ">> standalone launch (no build): $TARGET"
    fi
    ;;
  gskit|embedgs|iso|fastiso|freeiso)
    build_cfg "$1"
    ;;
  *)
    TARGET="$1"
    ;;
esac
if [[ ! -e "$TARGET" ]]; then
  echo "!! not found: $TARGET"
  echo "   name a config to build+launch (./run.sh gs usb | iso | fastiso ...),"
  echo "   or pass an existing .elf/.iso path.  See ./run.sh --help"
  exit 1
fi
[[ -x "$PCSX2" ]] || { echo "!! PCSX2 not found at: $PCSX2 (set PCSX2=...)"; exit 1; }

# PCSX2 is a Windows process; copy any artifact that lives outside the
# Windows-visible OUTDIR (e.g. a freshly-built bin/ps2oom.{elf,iso}) into it.
if [[ "$TARGET" != "$OUTDIR"/* ]]; then
  cp -f "$TARGET" "$OUTDIR/" && echo ">> copied $(basename "$TARGET") -> $OUTDIR/"
  TARGET="$OUTDIR/$(basename "$TARGET")"
fi
case "$TARGET" in
  *.elf|*.ELF) BOOTARGS=(-elf "$(wslpath -w "$TARGET")") ;;
  *)           BOOTARGS=("$(wslpath -w "$TARGET")") ;;   # ISO / disc image = positional
esac

# Don't fight an existing instance (PCSX2 single-instance + shared MC/settings).
if tasklist.exe /FI "IMAGENAME eq pcsx2-qt.exe" /NH 2>/dev/null | grep -qi pcsx2; then
  echo "!! PCSX2 is already running -- close it first (this script won't kill it)."
  exit 1
fi

LOG_WIN="$(wslpath -w "$LOG")"
: > "$LOG"   # truncate so the tail below shows only this run

echo ">> booting: $TARGET"
echo ">> log:     $LOG"
nohup "$PCSX2" -fastboot -logfile "$LOG_WIN" "${BOOTARGS[@]}" >/dev/null 2>&1 &
PCSX2_PID=$!   # WSL interop PID -- tracks the Windows process's lifetime (dies on quit/alt-F4)
disown

echo ">> following the log (Ctrl-C stops the tail; closing PCSX2 stops it too). Filtered lines:"
echo "------------------------------------------------------------------------"
# Wait for PCSX2 to (re)create the file, then follow it filtered. `--pid` makes
# tail exit once PCSX2 quits, so this script returns instead of hanging forever.
for _ in $(seq 1 50); do [[ -s "$LOG" ]] && break; sleep 0.2; done
exec tail --pid="$PCSX2_PID" -n +1 -F "$LOG" 2>/dev/null | grep --line-buffered -iE "$PATTERNS"
