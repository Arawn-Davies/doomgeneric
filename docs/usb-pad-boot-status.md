# PS2OOM — real-hardware boot / controller / music status

_Snapshot: 2026-06-14. Target hardware: **FAT PS2, soft-modded (FreeMcBoot), NO modchip**.
Media: **CD-R only** (no DVD-R) → boot is via **USB** (FMCB → wLaunchELF → ELF). USB stick
(FAT32) + Ethernet (ps2link) available._

## TL;DR — all working on real hardware
- ✅ **gsKit renderer** — hi-res 640×400, full speed.
- ✅ **WAD loading from USB** — custom `fileXio` reader (`w_file_usb.c`).
- ✅ **Renderer re-exec** — `cdrom0:` via `LoadExecPS2`; `mass:` via `LoadELFFromFile` (elf-loader2).
- ✅ **Controller** — works. The `PAD_STATE_EXECCMD` wedge was a module **load-order** bug (below).
- ✅ **Music + level transitions** — the intermission/E1M2 hard-lock was a MIDI-loader heap
  starvation, now fixed (below). Music plays, the screen melt runs, no crash.
- ✅ **Quit** — the straight-boot embedded build's "quit to DOS" returns to `rom0:OSDSYS`.

## Hardware / media constraints (these rule a lot out)
- Soft-mod only (FMCB, no chip) ⇒ **cannot boot a burned CD-R/DVD-R**. Burning is OUT.
- So every viable boot goes through **USB** (FMCB → wLaunchELF → ELF) — which means the USB stack is
  resident, which was exactly the pad conflict (now solved by load order).

## SOLVED — controller dead on USB boot: it was IOP module load order
`libpad` wedged at `PAD_STATE_EXECCMD` (5), never reaching `STABLE` (6), **whenever `usbd` (the USB
BDM stack) loaded before `sio2man`/`padman`**. wLaunchELF and OPL both load the pad modules *first*;
libps2_drivers' default brought USB up first. **Fix: bring the pad up first** (sio2man + mtap +
padman + padInit), *then* the USB/filesystem stack.
- **Proven** by a stripped build with **no `usbd` at all** (embedded-WAD gsKit ELF) → pad works.
- **The gotcha that cost hours:** each renderer has its **own `main()`** — the pad bring-up has to
  be in the gsKit main (`doomgeneric_ps2_gs.c`), not just the SDL one (`init_joystick_driver(true)`).
- **OPL can never give the pad** — it pre-loads `usbd` to serve the ISO and owns the IOP. The clean
  path is a direct **wLaunchELF** launch (its `usbd` is wiped by the ELF's own `SifIopReset`).

## SOLVED — digital / stick-less pads looked "stuck looking left"
An original (D-pad-only) PS1 pad reports its analog-axis bytes as `0`, which `axis()` (centre `0x80`)
reads as "stick hard-left" → the view spins left forever. Fix: if the pad isn't an analog/DualShock
type (`padInfoMode`), force the axes to centre — the D-pad (mapped to the arrow keys) drives instead.

## SOLVED — level-end / intermission hard-lock: MIDI-loader heap starvation
Completing a level (any change to a bigger song) hard-locked on real HW — the "stuttering audio, all
threads frozen" TLB-miss crash. **It was never the screen melt** (the wipe runs fine). The OPL MIDI
loader (`midifile.c`) grows its event array with `realloc` on the **system malloc heap** — whatever
EE RAM is left after Doom's zone + the ELF. The zone was an over-generous **18 MB** and the
embedded-WAD ELF is ~4 MB bigger (4 MB WAD in `.data`), so the leftover heap was a sliver: larger
songs failed to `realloc` and the unchecked code wrote `midi_event_t`s **through NULL** → near-NULL
low RAM → **TLB-miss hard fault** (real EE only; PCSX2 maps low RAM and tolerates it — which is why
it "worked" on the emulator).
- **Diagnosed** from PCSX2's `TLB Miss, pc=… addr=…` log → `addr2line` put the PC in
  `ReadTrack`/`ReadVariableLength`; the `addr` climbing by 20 bytes (`= sizeof(midi_event_t)`)
  confirmed an array write through a NULL base.
- **Fix:** size the zone **per build** (`Makefile` `DG_ZONE_MB`: embedded **12**, external-WAD **16**
  — external ELF is smaller but levels are bigger: Doom 2, SIGIL) so the loader keeps heap, plus a
  **NULL-check** in `ReadTrack` so a too-big track bails instead of faulting.

## Useful diagnostic facts (don't relearn these)
- **"Runs on PCSX2 but locks on real PS2" = a wild / near-NULL / OOB write.** PCSX2 maps low RAM
  (only logs `TLB Miss`); real EE TLB-faults → hard lock. Chase it via the TLB-Miss log + `addr2line`.
- PCSX2 prints EE `printf` straight to its console — drop `printf(...); fflush(stdout);` phase
  markers and read the log; no ps2link/serial needed.
- `fileXioDopen/Dread/Open/Read` work on `mass:`; newlib `fopen`/`opendir` do **not**.
- The libdebug GS text console swallows lines — don't trust it for diagnostics.
- Button bitmap is **active-low** (`0xFFFF` = all released; X clears `0x4000`).

## Boot methods
| Method | Result |
|---|---|
| **Direct USB** — wLaunchELF → `mass:…/ps2oom.elf` | **The path.** WADs load, pad works, re-exec works. |
| **OPL → ISO from USB** | Renders + runs, but **OPL pre-loads `usbd`** → the pad never comes up first. Not for the pad. |
| **Burn CD-R / DVD-R** | Not possible on a soft-mod. |
