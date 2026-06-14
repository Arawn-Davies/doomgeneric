// IOP driver bring-up ORDER control.
//
// libpad wedges at PAD_STATE_EXECCMD if the USB BDM stack (usbd/bdm/usbmass_bd) is
// loaded on the IOP BEFORE sio2man/padman. The ps2dev SDL2 shim's main() does
// exactly that: it calls init_ps2_filesystem_driver() (which loads USB) and then
// waitUntilDeviceIsReady(cwd) BEFORE handing control to our main() -- so the pad,
// brought up later, never reaches STABLE. Both wLaunchELF and OPL avoid this by
// loading sio2man/padman BEFORE the USB modules.
//
// So we no-op the shim's pre-main storage bring-up here (the linker prefers these
// definitions over libps2_drivers.a -- same trick as ps2_audio_driver.c) and
// instead bring every driver up ourselves, PAD FIRST, in PS2_BringUpDrivers()
// (doomgeneric_ps2.c). waitUntilDeviceIsReady must be neutralised too, else the
// shim spins its ~28 s timeout (USB isn't mounted at that point); we do our own
// bounded device wait after we load USB.
//
// dev9 (network adapter / HDD bay) stays stubbed: unused, and it otherwise spins a
// long enumeration timeout when absent.

#include <stdbool.h>
#include <ps2_dev9_driver.h>
#include <ps2_filesystem_driver.h>

enum DEV9_INIT_STATUS init_dev9_driver(void)
{
    return DEV9_INIT_STATUS_OK;
}

void deinit_dev9_driver(void)
{
}

// Pre-main storage bring-up is OURS now (PS2_BringUpDrivers, pad-first) -- no-op
// the shim's calls so they don't load USB before the pad.
void init_ps2_filesystem_driver(void)
{
}

void deinit_ps2_filesystem_driver(void)
{
}

// Skip the shim's pre-main device wait (USB isn't up yet -> would spin ~28 s).
bool waitUntilDeviceIsReady(char *path)
{
    (void) path;
    return true;
}
