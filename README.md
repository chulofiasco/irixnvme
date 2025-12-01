# IRIX NVMe Driver v0.9.18

A high-performance NVMe driver for SGI IRIX 6.5, providing native NVMe support through SCSI translation. Enables modern NVMe SSDs to work seamlessly with legacy IRIX systems.

## Features

- **Full NVMe 1.x support** - Admin and I/O command sets
- **Multi-queue I/O** - Up to 8 queues for multi-CPU systems (configurable)
- **Cross-platform** - Supports IP30 (Octane), IP32 (O2), and IP35 (Tezro/Origin)
- **SCSI emulation** - Transparent integration with IRIX disk subsystem
- **Performance optimized** - Lock-free queue submission, interrupt coalescing
- **Production ready** - Extensive testing on multiple platforms and drives

## Quick Start

### Prerequisites

- SGI system running IRIX 6.5.x
- MIPSpro C compiler 7.4.x installed
- IRIX development libraries (build_dev)
- PCI to PCIe bridge (PLX, Pericom, or ASMedia chipset)
- NVMe M.2 SSD via PCIe adapter

### Installation


# Download and extract the tardist package
tar xf irixnvme-0.9.18.tardist

# Install using IRIX inst
inst -f irixnvme

# At the inst> prompt:
inst> install irixnvme
inst> go
inst> quit

# Enable at boot
chkconfig nvme on

# Reboot to load driver
reboot


### Building from Source


# Set platform: IP30, IP32, or IP35
export CPUBOARD=IP30  # or IP32, IP35

# Build driver
make

# Load driver
make load

# Verify detection (appears as "Integral SCSI controller X: Version QL1280, single ended" where X is the controller number.)
hinv -v

# Create partitions
mkparts

# Format filesystem
mkfs /dev/dsk/dks3d0s0

# Mount
mount /dev/dsk/dks3d0s0 /mnt

**Note:** NVMe controllers appear in `hinv` as "Integral SCSI controller X: Version QL1280, single ended" where X is the controller number.

## Supported Platforms

| Platform | CPU        | Architecture     | Status  | Notes                              |
|----------|------------|------------------|---------|------------------------------------|
| IP30     | R12K-R16K  | HEART/BRIDGE     | Full    | Cannot unload driver once loaded   |
| IP32     | R5K-R12K   | MACE             | Full    | Full load/unload support           |
| IP35     | R14K-R16K  | BEDROCK/XBRIDGE  | Full    | 8-CPU multi-queue support          |

### Platform-Specific Configuration

**IP30 (Octane):**
- Multi-queue disabled (single I/O queue)
- Driver persistence: Cannot unload without reboot
- Verbose mode: Controlled by /etc/chkconfig/verbose

**IP32 (O2):**
- Multi-queue disabled (single I/O queue)
- Full load/unload support
- Special MACE PCI handling for cache coherency

**IP35 (Tezro/Origin 3000):**
- Multi-queue enabled (up to 8 queues for 8 CPUs)
- Write-gather flush disabled (XBRIDGE handles ordering)
- Verbose mode: Set nvme_verbose=1 in /var/sysgen/master.d/nvme, then run autoconfig

## Tested Hardware

### PCI-PCIe Bridges
- ✅ PLX PEX8112 (Sedna adapter)
- ✅ Pericom PI7C9X2G304 (StarTech adapter)
- ✅ ASMedia ASM1083 (generic adapters)
- ⚠️ Avoid: Marvell-based bridges (compatibility issues)

### NVMe Drives
- ✅ Samsung PM9A1, 970 EVO, 980 PRO
- ✅ Crucial P3, P5
- ✅ WD Black SN750, SN850
- ✅ Patriot P300, P400
- ✅ Liteon CV8 series
- ❌ Samsung 990 EVO (not detected - controller issue)

### Performance Results

**IP35 (8-CPU, 8 queues):**
- Sequential Read: 201 MB/s (4MB blocks)
- Sequential Write: 153 MB/s (2MB blocks)
- Random Read: 200 MB/s
- Random Write: 158 MB/s

**IP30 (2-CPU, 1 queue):**
- Sequential Read: 185 MB/s
- Sequential Write: 140 MB/s

## Project Structure


irixnvme/
├── nvmedrv.c         - Main driver (attach, init, shutdown)
├── nvmedrv.h         - Driver structures and definitions
├── nvme_scsi.c       - SCSI to NVMe translation layer
├── nvme_cmd.c        - NVMe command construction/submission
├── nvme_cpl.c        - Completion queue processing
├── nvme.h            - NVMe specification definitions
├── Makefile          - Build system
├── nvme.master       - IRIX kernel master file
├── nvme.sm           - IRIX system file
├── nvme.init         - Boot-time initialization script
├── mkparts.c         - Partition creation utility
├── nvmetest.c        - Comprehensive test program
├── LICENSE           - BSD 3-Clause license
└── README.md         - This file


## How It Works

### Driver Architecture

1. **PCI Discovery**
   - Registers with wildcard PCI IDs to catch all devices
   - Filters by class code 0x010802 (NVMe controller)
   - Maps BAR0 (controller registers) using pciio_piomap_alloc()

2. **Controller Initialization**
   - Reads capabilities (CAP register)
   - Allocates admin queue pair
   - Allocates I/O queue pairs (1 for IP30/IP32, up to 8 for IP35)
   - Enables controller (CC.EN)
   - Creates SCSI controller in hwgraph

3. **SCSI Emulation**
   - Creates /hw/scsi_ctlr/N hwgraph vertex
   - NVMe namespace 1 → SCSI target 0, LUN 0
   - IRIX dksc driver attaches automatically
   - Block devices created: /dev/dsk/dksNd0sX

4. **I/O Path**
   - SCSI command → nvme_scsi_command()
   - Translate to NVMe command → nvme_cmd_read/write()
   - Submit to queue → nvme_submit_cmd()
   - Ring doorbell → Direct MMIO write
   - Completion → Interrupt or polling → nvme_handle_io_completion()
   - Complete SCSI request back to dksc

### SCSI Translation Table

| SCSI Command          | NVMe Translation                    | Notes                  |
|-----------------------|-------------------------------------|------------------------|
| INQUIRY               | Synthesized from Identify Controller| Vendor, model, serial  |
| READ CAPACITY(10/16)  | Identify Namespace (NSZE)           | Total blocks           |
| TEST UNIT READY       | Check CSTS.RDY                      | Controller ready       |
| READ(6/10/16)         | NVMe Read                           | LBA + block count      |
| WRITE(6/10/16)        | NVMe Write                          | LBA + block count      |
| MODE SENSE            | Synthesized                         | Minimal page support   |
| SYNCHRONIZE CACHE     | NVMe Flush                          | Write-through mode     |

## Usage

### Utilities

**mkparts** - Create partition device nodes

mkparts          # Auto-detect controller
mkparts 3        # Specific controller number


**nvmetest** - Comprehensive testing

nvmetest -a                    # All basic tests
nvmetest -i                    # INQUIRY only
nvmetest -c                    # READ CAPACITY
nvmetest -r 0                  # Read single block at LBA 0
nvmetest -s 0 8192            # Sequential read (4MB)
nvmetest -W 100               # Write single block
nvmetest -w 100               # Write/read verify
nvmetest -R 1000              # Random stress test (DESTRUCTIVE!)
nvmetest -x -a                # Extended debug output


### Filesystem Operations


# Create XFS filesystem
mkfs -t xfs /dev/dsk/dks3d0s0

# Mount
mount /dev/dsk/dks3d0s0 /mnt

# Add to /etc/fstab for automatic mount
echo "/dev/dsk/dks3d0s0 /nvme xfs rw 0 0" >> /etc/fstab

# Performance test
diskperf -D -W -r4k -m4m /mnt/testfile


### Troubleshooting

**Driver doesn't load:**

# Check system log
cat /var/adm/SYSLOG

# Manual load with debugging
ml ld -v -c nvme.o -p nvme_

# Check PCI devices
hinv -mv | grep -i pci


**No device appears:**

# Verify driver attached (appears as "Integral SCSI controller X: Version QL1280")
hinv -v

# Check hwgraph
ls -la /hw/scsi_ctlr/*/target/0/lun/0/scsi

# Check if controller detected
ls -la /hw/node/io/pci/*/scsi_ctlr


**Poor performance:**

# Check queue configuration
grep "Allocated.*queue" /var/adm/SYSLOG

# Verify interrupt mode (not polling)
grep "interrupt.*enabled" /var/adm/SYSLOG

# Test raw performance
diskperf -D -r1m -m4m /dev/rdsk/dks3d0vol


## Development

### Enabling Multi-Queue (Developers)

Edit nvmedrv.h:

#define NVME_MAX_IO_QUEUES  8  // Up to 8 queues

Rebuild with NVME_MULTI_QUEUE defined in Makefile.

### Debug Options

**Compile-time:**
- NVME_DBG - Basic debugging
- NVME_DBG_EXTRA - Verbose debugging
- NVME_DBG_CMD - Command tracing

**Runtime:**
- Set nvme_verbose=1 (IP35: in nvme.master)
- Set chkconfig verbose on (IP30/IP32)

## Known Issues

1. **IP30 Driver Persistence** - Cannot unload driver once loaded. This is intentional to prevent kernel panics. Reboot required to remove.

2. **Samsung 990 EVO** - Not detected by some systems. Use 980 PRO or other models.

3. **IP35 File I/O During Init** - Disabled due to NUMA/PM subsystem not ready. Verbose mode requires kernel tunable.

## Version History

**0.9.18** (December 2025)
- Full IP35 (Tezro/Origin) support with 8-queue multi-CPU optimization
- Fixed PCI write-gather flush crashes on IP35
- Fixed NUMA/PM crashes during module init on IP35
- DMA mapping corrections for IP35 (kvtophys)
- Cleaned up debug output
- Updated documentation

**0.9.17** (November 2025)
- Multi-queue support for multi-CPU systems
- Improved interrupt coalescing
- Performance optimizations

**0.9.x** (2025)
- Initial public releases
- IP30/IP32 support
- Basic SCSI translation

## References

- [NVMe 1.0e Specification](https://nvmexpress.org/specifications/)
- IRIX Device Driver Programmer's Guide
- SGI IRIX 6.5 Manual Pages

## License

BSD 3-Clause License

Copyright (c) 2025, NVMe IRIX NVMEe Driver Project

Use at your own risk. No warranties express or implied.

## Authors

- TechoMancer
- @chulofiasco
- Rogelio

Bringing NVMe to IRIX, one rediculous project at a time.

## Credits

Inspired by the Windows 2000 NVMe driver project - bringing modern storage to legacy operating systems since 2025.

