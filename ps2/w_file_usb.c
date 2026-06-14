// WAD I/O backend for USB mass storage (BDM, "mass:"/"mass0:" via fileXio/iomanX).
//
// On this toolchain newlib stdio (fopen) and POSIX open() do NOT reach the BDM
// device -- only the direct fileXio API does. Confirmed on real hardware: fopen
// returns NULL even for the booted ELF, while fileXioDopen/Dread/Read all work.
// So WADs on a USB stick are read on demand with fileXioOpen/Read/Lseek/Close,
// mirroring the cdfs (fio) backend in w_file_cdfs.c. On-demand reads also let a
// large IWAD stream off the stick without fitting in the 32 MB of RAM.
//
// NEWLIB_PORT_AWARE silences the SDK guard that otherwise #errors on direct
// fileXio use in the newlib port (same trick w_file_cdfs.c uses for fio).

#define NEWLIB_PORT_AWARE

#include <stdlib.h>
#include <fileXio_rpc.h>        // fileXioOpen/Read/Lseek/Close (iomanX / BDM)

#include "doomtype.h"
#include "w_file.h"

#ifndef FIO_O_RDONLY
#define FIO_O_RDONLY 0x0001
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

extern wad_file_class_t usb_wad_file;

typedef struct
{
    wad_file_t wad;
    int        fd;
} usb_wad_file_t;

static wad_file_t *USB_OpenFile(char *path)
{
    usb_wad_file_t *result;
    int             fd;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return NULL;

    result = malloc(sizeof(usb_wad_file_t));
    if (result == NULL)
    {
        fileXioClose(fd);
        return NULL;
    }
    result->wad.file_class = &usb_wad_file;
    result->wad.mapped     = NULL;
    result->wad.length     = (unsigned int) fileXioLseek(fd, 0, SEEK_END);
    result->fd             = fd;
    return &result->wad;
}

static void USB_CloseFile(wad_file_t *wad)
{
    usb_wad_file_t *usb = (usb_wad_file_t *) wad;
    fileXioClose(usb->fd);
    free(usb);
}

static size_t USB_Read(wad_file_t *wad, unsigned int offset,
                       void *buffer, size_t buffer_len)
{
    usb_wad_file_t *usb = (usb_wad_file_t *) wad;
    int             n;

    fileXioLseek(usb->fd, (int) offset, SEEK_SET);
    n = fileXioRead(usb->fd, buffer, (int) buffer_len);
    return (n < 0) ? 0 : (size_t) n;
}

wad_file_class_t usb_wad_file =
{
    USB_OpenFile,
    USB_CloseFile,
    USB_Read,
};
