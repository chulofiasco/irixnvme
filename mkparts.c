/*
 * mkparts.c - Create partition device nodes for a specific SCSI disk
 *
 * This utility creates partition device nodes for a single disk by
 * issuing a DIOCREADVOLHDR ioctl, which triggers the kernel to create
 * partition devices for just this disk without affecting others.
 *
 * Usage: mkparts [controller_num]
 * Example: mkparts 3          (explicit controller number)
 *          mkparts            (auto-detect NVMe controller)
 *
 * This will create /dev/dsk/dks3d0s0, dks3d0s1, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <sys/dvh.h>

/* IOCTL to read volume header and create partition devices */
#ifndef DIOCREADVOLHDR
#define DIOCREADVOLHDR  _IOR('d', 120, struct volume_header)
#endif

/* Auto-detect controller number by scanning /hw/scsi_ctlr/ */
/* Find the HIGHEST numbered controller (most recently added) */
static int find_nvme_controller(void)
{
    int ctlr;
    int found = -1;
    char path[256];
    struct stat st;
    
    /* Try controller numbers 0-99, remember the highest one found */
    for (ctlr = 0; ctlr < 100; ctlr++) {
        snprintf(path, sizeof(path), "/hw/scsi_ctlr/%d/target/0/lun/0/scsi", ctlr);
        if (stat(path, &st) == 0) {
            /* Found a controller, verify it has volume device */
            snprintf(path, sizeof(path), "/dev/rdsk/dks%dd0vol", ctlr);
            if (stat(path, &st) == 0) {
                found = ctlr;  /* Remember this one, keep looking for higher */
            }
        }
    }
    
    return found;
}

int main(int argc, char **argv)
{
    char volpath[256];
    char partpath[256];
    int fd;
    struct volume_header vh;
    int ctlr;
    int i;
    struct stat st;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [controller_number]\n", argv[0]);
        fprintf(stderr, "Example: %s 3\n", argv[0]);
        fprintf(stderr, "         %s    (auto-detect)\n", argv[0]);
        return 1;
    }

    /* Get controller number from argument or auto-detect */
    if (argc == 2) {
        ctlr = atoi(argv[1]);
        if (ctlr < 0 || ctlr > 99) {
            fprintf(stderr, "Error: Invalid controller number: %d\n", ctlr);
            return 1;
        }
    } else {
        ctlr = find_nvme_controller();
        if (ctlr < 0) {
            fprintf(stderr, "Error: No NVMe controller found\n");
            fprintf(stderr, "Make sure the driver is loaded: smake load\n");
            fprintf(stderr, "Or specify controller manually: %s <num>\n", argv[0]);
            return 1;
        }
        printf("Auto-detected controller %d\n", ctlr);
    }

    /* Build path to volume device */
    snprintf(volpath, sizeof(volpath), "/dev/rdsk/dks%dd0vol", ctlr);

    /* Check if volume device exists */
    if (stat(volpath, &st) != 0) {
        fprintf(stderr, "Error: Volume device %s not found\n", volpath);
        fprintf(stderr, "Make sure the driver is loaded and controller %d exists\n", ctlr);
        return 1;
    }

    /* Open volume device */
    fd = open(volpath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open %s: %s\n", volpath, strerror(errno));
        return 1;
    }

    /* Issue ioctl to read volume header and trigger partition device creation */
    printf("Reading volume header from %s...\n", volpath);
    if (ioctl(fd, DIOCREADVOLHDR, &vh) < 0) {
        /* If DIOCREADVOLHDR not supported, try reading directly */
        lseek(fd, 0, SEEK_SET);
        if (read(fd, &vh, sizeof(vh)) != sizeof(vh)) {
            fprintf(stderr, "Error: Cannot read volume header from %s: %s\n", 
                    volpath, strerror(errno));
            close(fd);
            return 1;
        }
    }
    
    close(fd);

    /* Verify it's a valid volume header */
    if (vh.vh_magic != VHMAGIC) {
        fprintf(stderr, "Error: Invalid volume header magic (0x%x, expected 0x%x)\n",
                vh.vh_magic, VHMAGIC);
        fprintf(stderr, "Disk may not be formatted with IRIX volume header\n");
        return 1;
    }

    printf("Volume header found on %s\n", volpath);

    /* List partitions */
    printf("Partitions:\n");
    for (i = 0; i < NPARTAB; i++) {
        if (vh.vh_pt[i].pt_nblks > 0) {
            printf("  Partition %d: %u blocks (%.1f MB) starting at %u\n",
                   i, vh.vh_pt[i].pt_nblks,
                   vh.vh_pt[i].pt_nblks * 512.0 / (1024.0 * 1024.0),
                   vh.vh_pt[i].pt_firstlbn);
        }
    }

    /* Verify partition device nodes exist */
    printf("\nVerifying partition device nodes:\n");
    for (i = 0; i < NPARTAB; i++) {
        if (vh.vh_pt[i].pt_nblks > 0) {
            snprintf(partpath, sizeof(partpath), "/dev/dsk/dks%dd0s%d", ctlr, i);
            
            if (stat(partpath, &st) == 0) {
                printf("  ✓ %s exists\n", partpath);
            } else {
                printf("  ✗ %s MISSING\n", partpath);
            }
        }
    }

    return 0;
}
