// PS2 IWAD + PWAD selection.
//
// Scans cdfs (disc/ISO) and hostfs (host:) for IWADs and PWADs, shows a
// controller setup menu (IWAD / PWAD / Music / Render), and returns the chosen
// IWAD. The chosen PWAD (if any) is merged by d_main.c right after the IWAD
// (see PS2_GetPWAD + the __PS2__ hook there).
//
// Called from the one PS2 hook in the upstream d_main.c (guarded by __PS2__).

#include <stdio.h>
#include <stdlib.h>     // atoi
#include <string.h>     // strncmp
#include <kernel.h>     // LoadExecPS2
#include <loadfile.h>   // SifExecModuleBuffer
#include <libpwroff.h>  // poweroffInit / poweroffShutdown
#include <elf-loader.h>  // LoadELFFromFile -- exec an ELF off mass: (LoadExecPS2 can't)
#include <sbv_patches.h> // sbv_patch_disable_prefix_check (required before LoadELFFromFile)

// USB (mass0:) directory enumeration uses the fileXio dir API directly, exactly
// as wLaunchELF does. The newlib opendir/readdir glue does NOT reliably enumerate
// BDM devices (it came up empty on real hardware), and we can't include ps2sdk's
// iox_stat.h / fileXio_rpc.h here -- the newlib port poisons them with an #error
// against direct fileXio use. So declare the minimal struct + the three dir calls
// ourselves. fileXio dir handles live in a separate fd space from newlib stdio
// (the WAD header is read with fopen, after the dir handle is closed), so the two
// never collide. The 64-byte alignment is required: the fileXio RPC DMAs the
// dirent into this buffer.
typedef struct {
    unsigned int  mode, attr, size;
    unsigned char ctime[8], atime[8], mtime[8];
    unsigned int  hisize, priv[6];
} ps2_iox_stat_t __attribute__((aligned(64)));
typedef struct {
    ps2_iox_stat_t stat;
    char           name[256];
    void          *privdata;
} ps2_iox_dirent_t __attribute__((aligned(64)));
extern int fileXioDopen(const char *name);
extern int fileXioDclose(int fd);
extern int fileXioDread(int fd, ps2_iox_dirent_t *dirent);
// ...and file I/O (newlib fopen can't reach mass:; the WAD magic is read here, and
// the WAD itself by w_file_usb.c -- both via fileXio).
extern int fileXioOpen(const char *name, int flags);
extern int fileXioRead(int fd, void *buf, int size);
extern int fileXioClose(int fd);
#define PS2_FIO_ISDIR(m)  (((m) & 0x0038) == 0x0020)   // FIO_SO_IFDIR
#define PS2_FIO_O_RDONLY  0x0001

#include "ps2_menu.h"   // PS2_SettingsMenu, ps2_setting_t
#include "m_argv.h"     // M_CheckParmWithArgs, myargv

// Power the PS2 down from the setup menu's "Shutdown" row. Loads the poweroff
// IOP module, then asks it to put the console into standby. Does not return on
// real hardware (on PCSX2 it just halts here -- emulators don't power off).
static void PS2_Shutdown(void)
{
    extern unsigned char poweroff_irx[];
    extern unsigned int  size_poweroff_irx;
    int ret;

    printf("PS2OOM: shutting down ...\n");
    SifExecModuleBuffer(poweroff_irx, size_poweroff_irx, 0, NULL, &ret);
    poweroffInit();
    poweroffShutdown();
    for (;;) { }
}

// This ELF's video renderer (set at build time). The three backends live in
// three ELFs on the disc and share one setup screen; choosing a different one
// LoadExec's its ELF with the chosen settings. 0 = SDL2 (software), 1 = gsKit
// (software), 2 = GL (hardware geometry).
#if defined(RENDERER_GL)
#define THIS_RENDERER 2
#elif defined(RENDERER_GS)
#define THIS_RENDERER 1
#else
#define THIS_RENDERER 0
#endif

// Per-renderer re-exec ELFs. Defaults are the disc paths; PS2_InitBootPaths()
// rewrites them to mass0:/ps2oom/ when we were booted off USB (so a renderer
// switch / quit-to-menu re-execs from the same USB folder, not the disc).
static char        g_elf_buf[3][96];
static const char *g_renderer_elf[3] = {
    "cdrom0:\\DOOMSDL.ELF;1",
    "cdrom0:\\DOOMGS.ELF;1",
    "cdrom0:\\DOOMGL.ELF;1",
};

// Set when we were launched off USB (argv[0] = "mass...:..."): gates the USB WAD
// probe fallback and tells the re-exec to come from the boot folder.
static int  g_boot_usb = 0;
// Directory the WADs live in. Defaults to mass0: but is rewritten from argv[0] on
// a USB boot (the device may be "mass:" or "mass0:" depending on the launcher).
static char g_usb_waddir[96] = "mass0:/ps2oom/wads";

// The fileXio dir API needs the EXACT registered device name, and launchers lie:
// wLaunchELF handed us "mass:..." but the BDM stack typically registers "mass0:",
// and the dir API can be picky about the slash after the colon. So given the boot
// base (e.g. "mass:ps2oom/") try the equivalent forms of "<base>wads", print the
// result of each (this is the on-screen probe), and keep the first the dir API
// actually opens. If none open we fall back to the "mass0:/" form as the best
// guess for the fopen probe.
static void PS2_ResolveUSBBase(char *base, int basesz)
{
    char        rel[80], dev[16], cand[4][96], wad[96];
    const char *colon = strchr(base, ':');
    const char *r     = colon ? colon + 1 : base;
    int         dl    = colon ? (int)(colon - base) + 1 : 0;
    int         i, dd, best = -1;

    while (*r == '/') r++;                                   // strip leading '/'
    snprintf(rel, sizeof(rel), "%s", r);                    // "ps2oom/"
    if (dl > (int)sizeof(dev) - 1) dl = (int)sizeof(dev) - 1;
    memcpy(dev, base, dl); dev[dl] = '\0';                  // "mass:"

    snprintf(cand[0], sizeof(cand[0]), "%s",        base);       // as derived
    snprintf(cand[1], sizeof(cand[1]), "%s/%s",     dev, rel);   // device + slash
    snprintf(cand[2], sizeof(cand[2]), "mass0:/%s", rel);        // mass0: + slash
    snprintf(cand[3], sizeof(cand[3]), "mass0:%s",  rel);        // mass0: no slash

    for (i = 0; i < 4; i++)
    {
        snprintf(wad, sizeof(wad), "%swads", cand[i]);
        dd = fileXioDopen(wad);
        printf("IWAD: probe %-26s -> %d%s\n", wad, dd, dd >= 0 ? "  <== OPENS" : "");
        if (dd >= 0) { fileXioDclose(dd); best = i; break; }
    }
    if (best < 0) best = 2;          // none opened -> mass0:/ form for the fopen probe
    snprintf(base, basesz, "%s", cand[best]);
}

static void PS2_InitBootPaths(void)
{
    static const char *names[3] = { "DOOMSDL.ELF", "DOOMGS.ELF", "DOOMGL.ELF" };
    static int done = 0;
    int i;

    if (done) return;
    done = 1;

    // argv[0] is the exact path the launcher loaded us from -- DON'T assume the
    // device name. wLaunchELF/FMCB hand us e.g. "mass:ps2oom/launch.elf" on some
    // setups, "mass0:/ps2oom/launch.elf" on others (the BDM USB unit is "mass:"
    // here, "mass0:" elsewhere). Take everything up to the last '/' as the base
    // folder, then hang the re-exec ELFs + the WAD dir off it -- so they always
    // match the device we actually booted from.
    printf("IWAD: boot path = %s\n",
           (myargc > 0 && myargv[0] != NULL) ? myargv[0] : "(none)");
    if (myargc > 0 && myargv[0] != NULL && !strncmp(myargv[0], "mass", 4))
    {
        const char *slash = strrchr(myargv[0], '/');
        char base[72];
        int  blen = slash ? (int)(slash - myargv[0]) + 1   // keep the '/'
                          : (int)strlen(myargv[0]);        // ELF at device root
        if (blen > (int)sizeof(base) - 1) blen = (int)sizeof(base) - 1;
        memcpy(base, myargv[0], blen);
        base[blen] = '\0';

        g_boot_usb = 1;
        PS2_ResolveUSBBase(base, sizeof(base));   // pick the device form that opens
        for (i = 0; i < 3; i++)
        {
            snprintf(g_elf_buf[i], sizeof(g_elf_buf[i]), "%s%s", base, names[i]);
            g_renderer_elf[i] = g_elf_buf[i];
        }
        snprintf(g_usb_waddir, sizeof(g_usb_waddir), "%swads", base);
        printf("IWAD: USB base = %s   wad dir = %s\n", base, g_usb_waddir);
    }
}

// Music engine chosen at the startup menu: 0 = OPL/FM (audsrv), 1 = SPU2 synth.
// Default OPL (the classic sound); the menu's second option is SPU2. Read by
// i_sound.c's InitMusicModule.
static int g_music_engine = 0;
int PS2_MusicEngine(void) { return g_music_engine; }

// GS output mode chosen at the setup menu: 0 = interlaced (480i/NTSC, any TV),
// 1 = progressive 480p (sharp, no flicker; needs component/YPbPr on real
// hardware, works directly in PCSX2). Honored by the gsKit backend at GS init
// (doomgeneric_ps2_gs.c). Default follows the build flag so a GS480P build still
// defaults to 480p.
#ifdef GS_OUTPUT_480P
static int g_video_mode = 1;
#else
static int g_video_mode = 0;
#endif
int PS2_VideoMode(void) { return g_video_mode; }

// Jump enabled (0 = vanilla, no jumping). Toggled on the setup menu; read by
// p_user.c (P_MovePlayer). Carried across a renderer switch with -jump.
static int g_jump = 0;
int PS2_JumpEnabled(void) { return g_jump; }

// PWAD chosen on the setup menu (or passed via -pwad on a renderer switch).
// NULL = none. d_main.c calls PS2_GetPWAD() after loading the IWAD.
static char *g_pwad_path = NULL;
char *PS2_GetPWAD(void) { return g_pwad_path; }

// Re-exec another ELF. LoadExecPS2 loads via the EE kernel's ioman/fio path, which
// CANNOT read a BDM "mass:" device (it's iomanX-only) -- on a USB boot it just drops
// to the OSDSYS. So when we booted off USB, use elf-loader2's LoadELFFromFile, which
// reads the ELF properly (it explicitly supports mass:) and execs it.
static void PS2_Exec(const char *path, int argc, char **argv)
{
    if (g_boot_usb)
    {
        sbv_patch_disable_prefix_check();    // required before LoadELFFromFile
        LoadELFFromFile(path, argc, argv);   // noreturn on success
    }
    LoadExecPS2(path, argc, argv);           // disc/host (and fallback)
}

// Called from I_Quit (DOOM "quit to DOS").
//   * Launcher builds re-exec the boot ELF with no -iwad arg, so PS2_GetIWAD
//     shows the setup menu again instead of the PS2 BIOS.
//   * Straight-boot embedded builds (BOOT_STRAIGHT) have no setup menu to return
//     to, so they exit to the PS2 system menu instead: rom0:OSDSYS (the browser,
//     or FMCB's menu on a modded console). rom0: is ROM, so plain LoadExecPS2
//     reads it -- the mass:-only LoadELFFromFile path in PS2_Exec isn't needed.
void PS2_ReturnToLauncher(void)
{
#ifdef BOOT_STRAIGHT
    LoadExecPS2("rom0:OSDSYS", 0, NULL);   // -> PS2 system menu; noreturn
#else
    char *args[1];
    PS2_InitBootPaths();   // make sure g_renderer_elf points at the boot device
    args[0] = (char *)g_renderer_elf[THIS_RENDERER];   // argv[0]=real path => USB-aware re-exec
    PS2_Exec(g_renderer_elf[THIS_RENDERER], 1, args);   // noreturn
#endif
}

// Candidate IWADs to probe. cdfs (disc/ISO) and hostfs (host:) are scanned into
// one list; the user picks from whatever is actually present. PWADs are a
// separate list (below) -- this row stays IWAD-only.
static char *cd_iwads[] = {
    "cdfs:/DOOM.WAD", "cdfs:/DOOM2.WAD", "cdfs:/DOOM1.WAD",
    "cdfs:/PLUTONIA.WAD", "cdfs:/TNT.WAD",
    "cdfs:/FREEDOOM1.WAD", "cdfs:/FREEDOOM2.WAD", "cdfs:/FREEDM.WAD", NULL
};
static char *host_iwads[] = {
    "host:DOOM.WAD", "host:DOOM2.WAD", "host:DOOM1.WAD",
    "host:PLUTONIA.WAD", "host:TNT.WAD", "host:DOOM64.WAD",
    "host:freedoom1.wad", "host:freedoom2.wad", "host:freedm.wad", NULL
};
// USB mass-storage WADs live in <bootdir>/wads -- see g_usb_waddir, which
// PS2_InitBootPaths sets from argv[0] (the device is "mass:" or "mass0:" depending
// on the launcher). WADs are NOT hardcoded: the folder is enumerated and each
// *.wad is classified by its header magic (see scan_usb_wads below), so any WAD
// you drop in just shows up.

// Candidate PWADs (episode mods / megawads merged on top of an IWAD). cdfs
// names are UPPERCASE (the ISO graft uppercases them); host names keep the
// on-disk case. SIGIL_COMPAT first, as requested.
static char *cd_pwads[] = {
    "cdfs:/SIGIL_COMPAT.WAD", "cdfs:/SIGIL.WAD", "cdfs:/SIGIL_SHREDS.WAD",
    "cdfs:/NERVE.WAD", "cdfs:/SCYTHE.WAD", "cdfs:/THATCHER.WAD",
    "cdfs:/NUTS.WAD", "cdfs:/NUTS2.WAD", "cdfs:/NUTS3.WAD", NULL
};
static char *host_pwads[] = {
    "host:SIGIL_COMPAT.wad", "host:SIGIL.wad", "host:SIGIL_SHREDS.wad",
    "host:NERVE.WAD", "host:SCYTHE.WAD", "host:THATCHER.wad",
    "host:NUTS.WAD", "host:NUTS2.WAD", "host:NUTS3.WAD", NULL
};
// Enumerate the USB WAD dir (g_usb_waddir) and add every *.wad whose magic equals
// `magic4` ("IWAD" or "PWAD") to labels[]/paths[]. Full paths are kept in the
// caller's persistent `pool`; labels point at the bare filename. Nothing is
// hardcoded -- whatever WADs are in mass0:/ps2oom/wads/ are discovered here.
static int scan_usb_wads(const char *magic4, char **labels, char **paths,
                         int count, int max,
                         char pool[][96], int *pool_n, int pool_max)
{
    ps2_iox_dirent_t de;
    char names[32][72];   // bare *.wad filenames collected in one dir pass
    int  nn = 0, i, dd;

    dd = fileXioDopen(g_usb_waddir);
    if (dd < 0)
    {
        printf("  fileXioDopen(%s) = %d (no stick / not mounted?)\n", g_usb_waddir, dd);
        return count;
    }
    // Collect *.wad names first, then close the dir handle before any fopen so a
    // fileXio dir handle and stdio file reads never overlap.
    while (nn < 32 && fileXioDread(dd, &de) > 0)
    {
        int len = (int) strlen(de.name);
        if (len < 5 || len >= (int) sizeof(names[0]))   continue;
        if (PS2_FIO_ISDIR(de.stat.mode))                continue;
        if (strcasecmp(de.name + len - 4, ".wad") != 0) continue;
        strcpy(names[nn++], de.name);
    }
    fileXioDclose(dd);

    if (nn == 0)
        printf("  (%s opened OK but has no *.wad entries)\n", g_usb_waddir);

    for (i = 0; i < nn && count < max && *pool_n < pool_max; ++i)
    {
        char *full = pool[*pool_n];
        char  hdr[4];
        int   fd;

        snprintf(full, 96, "%s/%s", g_usb_waddir, names[i]);
        fd = fileXioOpen(full, PS2_FIO_O_RDONLY);   // fopen can't reach mass:
        if (fd < 0)
            continue;
        if (fileXioRead(fd, hdr, 4) == 4 && memcmp(hdr, magic4, 4) == 0)
        {
            printf("IWAD:   %-34s [%s]\n", full, magic4);
            labels[count] = full + strlen(g_usb_waddir) + 1;   // bare filename (past ".../wads/")
            paths[count]  = full;
            (*pool_n)++;
            count++;
        }
        fileXioClose(fd);
    }
    return count;
}

// Fallback for the rare case the dir listing fails but file reads work: probe a
// list of well-known WAD filenames by fopen (the FAT/BDM layer is case-insensitive).
// The dynamic scan above stays primary -- this runs ONLY when it finds nothing on
// USB, so a stick with the usual WADs still boots even if enumeration misbehaves.
static const char *usb_probe_names[] = {
    "DOOM.WAD", "DOOM1.WAD", "DOOM2.WAD", "DOOM64.WAD", "PLUTONIA.WAD", "TNT.WAD",
    "freedoom1.wad", "freedoom2.wad", "freedm.wad", "STRIFE1.WAD", "ulsimpdm.wad",
    "SIGIL.wad", "SIGIL_COMPAT.wad", "SIGIL_SHREDS.wad",
    "NERVE.WAD", "SCYTHE.WAD", "THATCHER.wad", "NUTS.WAD", "NUTS2.WAD", "NUTS3.WAD",
    NULL
};
static int probe_usb_wads(const char *magic4, char **labels, char **paths,
                          int count, int max,
                          char pool[][96], int *pool_n, int pool_max)
{
    int i;
    printf("  (dir scan empty -- probing common WAD names by fopen)\n");
    for (i = 0; usb_probe_names[i] != NULL && count < max && *pool_n < pool_max; ++i)
    {
        char *full = pool[*pool_n];
        char  hdr[4];
        int   fd;

        snprintf(full, 96, "%s/%s", g_usb_waddir, usb_probe_names[i]);
        fd = fileXioOpen(full, PS2_FIO_O_RDONLY);
        if (fd < 0)
            continue;
        if (fileXioRead(fd, hdr, 4) == 4 && memcmp(hdr, magic4, 4) == 0)
        {
            printf("IWAD:   %-34s [%s, probe]\n", full, magic4);
            labels[count] = full + strlen(g_usb_waddir) + 1;
            paths[count]  = full;
            (*pool_n)++;
            count++;
        }
        fileXioClose(fd);
    }
    return count;
}

char *PS2_GetIWAD(void)
{
#ifdef EMBED_WAD
    static char embedded_iwad[] = "doom1.wad";   // served from the baked-in array
#endif
    extern void PS2Cdfs_Init(void);
    extern int  PS2Cdfs_Exists(const char *path);

    char *labels[24];     // IWADs shown in the menu
    char *paths[24];      // IWAD path returned to the WAD loader
    int   n = 0;
    char *pw_labels[24];  // PWADs ("None" + whatever is present)
    char *pw_paths[24];   // matching PWAD paths (pw_paths[0] = NULL = none)
    int   pwn = 0;
    int   i;
    FILE *f;
    // Persistent storage for USB WAD paths discovered by scan_usb_wads (the
    // returned IWAD path + g_pwad_path point into it, so it must outlive us).
    static char usb_pool[24][96];
    int   usb_pool_n = 0;

#ifdef SPU_BEEP
    // S1: prove the SPU2 hardware-voice path in isolation, before any audio
    // stack is up, then halt so the tone is all we hear.
    {
        extern void PS2Spu_BeepTest(void);
        printf("\n=== SPU2 synth S1: hardware voice self-test ===\n");
        PS2Spu_BeepTest();
        printf("spu: halted -- you should hear a steady tone.\n");
        for (;;) { }
    }
#endif

#if defined(BOOT_STRAIGHT) && defined(EMBED_WAD)
    // Single-file quick-test build: skip the setup menu entirely and boot
    // straight into the embedded WAD. This replicates the known-good DOOMGS.ELF
    // path (light boot banner -> gsKit, no menu). Running the menu in this same
    // ELF drove the libdebug GS console and THEN gsKit, which came up garbled
    // (no LoadExec in between to reset the GS).
    printf("BOOT_STRAIGHT: skipping setup menu, using embedded DOOM1.WAD\n");
    return embedded_iwad;
#endif

    // Launched by the OTHER renderer's ELF (a renderer switch) with explicit
    // settings? Use them and skip the setup menu -- no second screen.
    {
        int pi = M_CheckParmWithArgs("-iwad", 1);
        if (pi > 0)
        {
            int pm = M_CheckParmWithArgs("-music", 1);
            int pp = M_CheckParmWithArgs("-pwad", 1);
            int pv = M_CheckParmWithArgs("-video", 1);
            int pj = M_CheckParmWithArgs("-jump", 1);
            if (pm > 0) g_music_engine = atoi(myargv[pm + 1]);
            if (pp > 0) g_pwad_path = myargv[pp + 1];
            if (pv > 0) g_video_mode = atoi(myargv[pv + 1]);
            if (pj > 0) g_jump = atoi(myargv[pj + 1]);
            printf("IWAD (from launcher): %s   pwad: %s   music: %d   video: %d   jump: %d\n",
                   myargv[pi + 1], g_pwad_path ? g_pwad_path : "(none)", g_music_engine, g_video_mode, g_jump);
            return myargv[pi + 1];
        }
    }

    PS2_InitBootPaths();   // point re-exec at the boot device (USB vs disc)

    // Scan USB mass storage first (mass0:/ps2oom/wads/) -- the primary boot source
    // for a FMCB/wLaunchELF install. Dynamic: every IWAD-magic *.wad in the folder
    // is discovered. An absent stick just opens nothing.
    printf("IWAD: scanning USB (%s)...\n", g_usb_waddir);
    n = scan_usb_wads("IWAD", labels, paths, n, 24, usb_pool, &usb_pool_n, 24);
    if (g_boot_usb && n == 0)   // dir listing failed but files may be readable
        n = probe_usb_wads("IWAD", labels, paths, n, 24, usb_pool, &usb_pool_n, 24);

    // Scan the disc (cdfs). PS2Cdfs_Init() is quick now that the boot-device
    // wait is gone (see ps2_drivers_stub.c); a missing disc just probes empty.
    printf("IWAD: scanning disc (cdfs)...\n");
    PS2Cdfs_Init();
    for (i = 0; cd_iwads[i] != NULL && n < 24; ++i)
    {
        if (PS2Cdfs_Exists(cd_iwads[i]))
        {
            printf("  %-24s [found]\n", cd_iwads[i]);
            labels[n] = cd_iwads[i];
            paths[n]  = cd_iwads[i];
            n++;
        }
    }

    // Scan hostfs (host:, e.g. PCSX2's mapped folder, next to the ELF).
    printf("IWAD: scanning hostfs...\n");
    for (i = 0; host_iwads[i] != NULL && n < 24; ++i)
    {
        f = fopen(host_iwads[i], "rb");
        if (f != NULL)
        {
            printf("  %-24s [found]\n", host_iwads[i]);
            fclose(f);
            labels[n] = host_iwads[i];
            paths[n]  = host_iwads[i];
            n++;
        }
    }

#ifdef EMBED_WAD
    if (n < 24)
    {
        labels[n] = "Embedded shareware DOOM1.WAD";
        paths[n]  = embedded_iwad;
        n++;
    }
#endif

    // Scan for PWADs (USB folder + cdfs/host probe). Index 0 is always "None".
    pw_labels[0] = "None"; pw_paths[0] = NULL; pwn = 1;
    printf("PWAD: scanning...\n");
    pwn = scan_usb_wads("PWAD", pw_labels, pw_paths, pwn, 24, usb_pool, &usb_pool_n, 24);
    if (g_boot_usb && pwn == 1)   // still only "None": same fallback as IWADs
        pwn = probe_usb_wads("PWAD", pw_labels, pw_paths, pwn, 24, usb_pool, &usb_pool_n, 24);
    for (i = 0; cd_pwads[i] != NULL && pwn < 24; ++i)
        if (PS2Cdfs_Exists(cd_pwads[i]))
        {
            printf("  %-24s [found]\n", cd_pwads[i]);
            pw_labels[pwn] = cd_pwads[i]; pw_paths[pwn] = cd_pwads[i]; pwn++;
        }
    for (i = 0; host_pwads[i] != NULL && pwn < 24; ++i)
    {
        f = fopen(host_pwads[i], "rb");
        if (f != NULL)
        {
            printf("  %-24s [found]\n", host_pwads[i]);
            fclose(f);
            pw_labels[pwn] = host_pwads[i]; pw_paths[pwn] = host_pwads[i]; pwn++;
        }
    }

    {
        // One setup page: IWAD, PWAD, music engine, and renderer. Up/Down pick a
        // row, Left/Right change it, Start/X launches. See PS2_SettingsMenu.
        static char  *eng[]  = { "OPL / FM (AdLib)", "SPU2 hardware synth" };
        static char  *rend[] = { "SDL2 (software)", "gsKit (software)", "GL (hardware)" };
        static char  *vid[]  = {
            "NTSC 480i", "NTSC 480p", "PAL 576i", "PAL 576p",
            "720p (exp)", "1080i (exp)"
        };
        static char  *jmp[]  = { "Off (vanilla)", "On (advanced)" };
        static char  *shut[] = { "Power off PS2" };
        ps2_setting_t settings[7] = {0};   // zero-init -> .action NULL by default
        char         *wad;

        if (n == 0)
        {
            printf("\n  *** No IWAD found ***\n");
            printf("  Supply a WAD on hostfs (host:) or a cdfs disc, or build EMBED_WAD=1.\n");
            for (;;) { }   // halt visibly instead of dropping to the PS2 BIOS
        }

        settings[0].label = "IWAD";   settings[0].values = labels;    settings[0].count = n;   settings[0].cur = 0;
        settings[1].label = "PWAD";   settings[1].values = pw_labels; settings[1].count = pwn; settings[1].cur = 0;
        settings[2].label = "Music";  settings[2].values = eng;       settings[2].count = 2;   settings[2].cur = g_music_engine;
        settings[3].label = "Render"; settings[3].values = rend;      settings[3].count = 3;   settings[3].cur = 1;   /* default gsKit; auto-skip re-execs DOOMGS.ELF */
        settings[4].label = "Video";  settings[4].values = vid;       settings[4].count = 6;   settings[4].cur = g_video_mode;
        settings[5].label = "Jump";   settings[5].values = jmp;       settings[5].count = 2;   settings[5].cur = g_jump;
        settings[6].label = "Shutdown"; settings[6].values = shut;    settings[6].count = 1;   settings[6].cur = 0;
        settings[6].action = PS2_Shutdown;   // Start/X on this row powers off

        PS2_SettingsMenu("PS2OOM  --  setup", settings, 7);

        wad            = paths[settings[0].cur];
        g_pwad_path    = pw_paths[settings[1].cur];   // NULL when "None"
        g_music_engine = settings[2].cur;
        g_video_mode   = settings[4].cur;
        g_jump         = settings[5].cur;

        // If the user picked a DIFFERENT renderer, hand off to its ELF on the
        // disc with these settings (it reads -iwad/-pwad/-music/-video, skips
        // the menu).
        if (settings[3].cur != THIS_RENDERER)
        {
            static char musbuf[4], vidbuf[4], jmpbuf[4];
            char *args[11];
            int   na;
            musbuf[0] = (char)('0' + (g_music_engine & 1)); musbuf[1] = '\0';
            vidbuf[0] = (char)('0' + g_video_mode);         vidbuf[1] = '\0';  // 0..5
            jmpbuf[0] = (char)('0' + (g_jump & 1));         jmpbuf[1] = '\0';
            args[0] = (char *)g_renderer_elf[settings[3].cur];   // argv[0]=real path => USB-aware
            args[1] = "-iwad"; args[2] = wad;
            args[3] = "-music"; args[4] = musbuf;
            args[5] = "-video"; args[6] = vidbuf;
            args[7] = "-jump";  args[8] = jmpbuf;
            na = 9;
            if (g_pwad_path) { args[na++] = "-pwad"; args[na++] = g_pwad_path; }
            printf("renderer switch -> %s\n", g_renderer_elf[settings[3].cur]);
            PS2_Exec(g_renderer_elf[settings[3].cur], na, args);   // noreturn
        }

        printf("IWAD: %s   pwad: %s   music: %s   video: %s\n",
               wad, g_pwad_path ? g_pwad_path : "(none)", eng[g_music_engine], vid[g_video_mode]);
        return wad;
    }
}
