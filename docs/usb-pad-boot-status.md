# PS2OOM — USB / disc boot + controller status

_Snapshot: 2026-06-14. Target hardware: **FAT PS2, soft-modded (FreeMcBoot), NO modchip**.
Media available: **CD-R only** (no DVD-R). USB stick (FAT32) + Ethernet (ps2link) available._

## TL;DR — current state
- ✅ **gsKit renderer** — renders correctly, runs the attract-mode demo smoothly (hi-res 640×400).
- ✅ **WAD loading from USB** — via a custom `fileXio` reader (`w_file_usb.c`).
- ✅ **Renderer re-exec** — `cdrom0:` via `LoadExecPS2`; `mass:` via `LoadELFFromFile` (elf-loader2).
- ❌ **CONTROLLER** — stuck at `PAD_STATE_EXECCMD` (5), never reaches `STABLE` (6), **whenever the
  USB BDM stack is loaded**. No setup-menu input, no in-game input. **This is the one blocker.**

Because the menu can't read the pad it auto-skips to the gsKit default, so you get gsKit Doom
running a demo with no way to drive it.

## Hardware / media constraints (these rule a lot out)
- Soft-mod only (FMCB, no chip) ⇒ **cannot boot a burned CD-R or DVD-R** (drive rejects burned
  optical media without a chip). ESR / FreeDVDBoot are DVD-only and need their own setup; CD-R
  homebrew boot effectively needs a chip. **Burning is OUT.**
- So every viable boot path goes through **USB** (FMCB → ELF, or OPL → ISO-on-USB), which means
  the USB stack is always resident → the pad conflict is always in play.

## Boot methods tried
| Method | Result |
|---|---|
| **Direct USB** — wLaunchELF → `mass:ps2oom/launch.elf` | WADs load (fileXio). Pad `EXECCMD`-stalls. Re-exec dropped to OSDSYS → **FIXED** (elf-loader2). |
| **OPL → ISO from USB** (`cdrom0:`) | gsKit renders + demo runs well. Re-exec works. **Pad still `EXECCMD`-stalls** — OPL loads the USB stack to read the ISO, so same conflict. |
| **PCSX2** — ISO as a disc | Boots. But wLaunchELF inside PCSX2 fails with a MagicGate/KELF "decrypt" error (that ELF was the encrypted FMCB build; a plain `BOOT-UNC.ELF` was built to work around it). USB-image route abandoned. |
| **Burn CD-R / DVD-R** | Not possible on a soft-mod (see constraints). |

## Fixes applied during the USB saga (chronological)
1. **Device name** — don't assume `mass0:`. Derive base from `argv[0]`; `PS2_ResolveUSBBase`
   tries `mass:`, `mass:/`, `mass0:/`, `mass0:`. This console reports the stick as **`mass:`**.
2. **WAD reading** — newlib `fopen`/POSIX can't reach the BDM device (returns NULL even for the
   booted ELF). Added `w_file_usb.c`: reads WADs on demand via `fileXioOpen/Read/Lseek/Close`.
   `W_OpenFile` routes `mass*` paths to it. **Works.**
3. **Directory enumeration** — `opendir`/`readdir` don't enumerate BDM. Switched the WAD scan to
   `fileXioDopen`/`fileXioDread` (the wLaunchELF way). **Works.**
4. **Pad (attempt)** — suspected a double-load of `rom0:SIO2MAN`/`PADMAN` vs the framework's own.
   Switched `PS2Pad_Init` to `init_joystick_driver(true)` (libps2_drivers' embedded
   sio2man+mtap+padman). **Did NOT fix the `EXECCMD` stall.**
5. **Re-exec** — `LoadExecPS2` loads via the EE kernel's ioman/fio path, which can't read an
   `iomanX`/BDM `mass:` device → dropped to OSDSYS. Switched USB re-exec to `LoadELFFromFile`
   (libelf-loader2; docs explicitly support `mass:`). Made the re-exec'd ELF USB-aware
   (`argv[0]` = real `mass:` path). **Works — no more OSDSYS.**
6. **Renderer default** — defaults to gsKit (so an auto-skip lands in gsKit, not SDL).
7. **Pad probe** (file-log via fileXio, because the GS console is unreliable) returned the
   decisive datum: `reached_stable=0  saw_press=0  last_state=5` (`PAD_STATE_EXECCMD`).
8. Abandoned/reverted: `padPortClose`+reopen in `PS2Pad_Init` (wedged padman → boot hang).

## The core unsolved problem
`libpad` reaches `PAD_STATE_EXECCMD` (the pad is detected and a config command is issued) but the
command **never completes** to `STABLE`. It happens whenever the **USB BDM stack** is resident —
both direct-USB boot and OPL-from-USB. Leading hypothesis: an **IOP-level conflict between the USB
stack (`usbd`) and `sio2man`** — IRQ contention or load-order — so the SIO2 transaction that
finishes pad config never lands.

## Useful diagnostic facts (don't relearn these)
- `fileXioDopen/Dread/Open/Read` all work on `mass:`. `fopen`/`opendir` (newlib) do **not**.
- The **libdebug GS text console swallows lines unreliably** — do not trust it for diagnostics.
  Write to a file on the stick via `fileXio`, or use ps2link, instead.
- ps2link caveat: the game's startup (SDL2main `prepare_IOP`) resets the IOP, which kills
  ps2link's network — a ps2link probe must avoid the IOP reset.
- Button bitmap is **active-low** (`0xFFFF` = all released; pressing X clears `0x4000`).

## Candidate next levers (for the plan)
- **IOP init order**: bring `sio2man`/`padman` up *before* the USB stack (take control of IOP
  module init rather than relying on libps2_drivers' default order).
- **freesio2 + freepad** instead of rom0:/framework `padman` (different IRQ handling).
- **Confirm the hypothesis**: does the pad reach `STABLE` in a build with **no USB stack** (e.g.
  embedded-WAD gsKit)? If yes, it's definitively the USB↔sio2man conflict.
- **ps2link** live pad-state readout (with a no-IOP-reset probe ELF).
- Inspect how OPL/wLaunchELF keep their own pad working alongside USB (they do).
