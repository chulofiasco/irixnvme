#!/bin/sh
#
# Manual build script for IRIX NVMe Driver
# This bypasses the broken Makefile to get a working build
#

CPUBOARD=${1:-IP35}

echo "Building NVMe driver for $CPUBOARD..."

# Set platform-specific flags
# These are copied directly from /var/sysgen/Makefile.kernloadio
GLOBAL_CFLAGS="-D_KERNEL -DMP_STREAMS -D_MP_NETLOCKS -DMRSP_AS_MR -fullwarn -non_shared -G 0 -TARG:force_jalr -TENV:kernel -OPT:space -OPT:Olimit=0 -CG:unique_exit=on -TENV:X=1 -OPT:IEEE_arithmetic=1 -OPT:roundoff=0 -OPT:wrap_around_unsafe_opt=off"

case $CPUBOARD in
    IP35)
        CFLAGS="$GLOBAL_CFLAGS -64 -D_PAGESZ=16384 -D_MIPS3_ADDRSPACE -DIP35 -DR10000 -DMP -DSN1 -DSN -DMAPPED_KERNEL -DLARGE_CPU_COUNT -DPTE_64BIT -DULI -DCKPT -DMIPS4_ISA -DNUMA_BASE -DNUMA_PM -DNUMA_TBORROW -DNUMA_MIGR_CONTROL -DNUMA_REPLICATION -DNUMA_REPL_CONTROL -DNUMA_SCHED -DCELL_PREPARE -DBHV_PREPARE -TARG:processor=r10000 -DNVME_FORCE_4K"
        LDFLAGS="-64 -r"
        ;;
    IP30)
        CFLAGS="$GLOBAL_CFLAGS -64 -D_PAGESZ=16384 -D_MIPS3_ADDRSPACE -DIP30 -DR10000 -DMP -DCELL_PREPARE -DBHV_PREPARE -TARG:processor=r10000 -DHEART_INVALIDATE_WAR -DNVME_FORCE_4K"
        LDFLAGS="-64 -r"
        ;;
    IP32)
        CFLAGS="$GLOBAL_CFLAGS -n32 -D_PAGESZ=4096 -DIP32 -DR4000 -DR10000 -DTRITON -DUSE_PCI_PIO"
        LDFLAGS="-n32 -r"
        ;;
    *)
        echo "Unknown CPUBOARD: $CPUBOARD"
        echo "Usage: $0 [IP35|IP30|IP32]"
        exit 1
        ;;
esac

# Compile each source file
echo "--- Compiling nvmedrv.c ---"
cc $CFLAGS -c nvmedrv.c -o nvmedrv.o || exit 1

echo "--- Compiling nvme_scsi.c ---"
cc $CFLAGS -c nvme_scsi.c -o nvme_scsi.o || exit 1

echo "--- Compiling nvme_cmd.c ---"
cc $CFLAGS -c nvme_cmd.c -o nvme_cmd.o || exit 1

echo "--- Compiling nvme_cpl.c ---"
cc $CFLAGS -c nvme_cpl.c -o nvme_cpl.o || exit 1

# Link the module
echo "--- Linking nvme.o ---"
ld $LDFLAGS -r nvmedrv.o nvme_scsi.o nvme_cmd.o nvme_cpl.o -o nvme.o || exit 1

echo ""
echo "=========================================="
echo "Build Complete!"
echo "Module: nvme.o"
echo "Platform: $CPUBOARD"
echo "=========================================="
echo ""
echo "To load: ml ld -v -c nvme.o -p nvme_ -s -1 -t 0"
