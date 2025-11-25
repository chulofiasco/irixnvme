# IRIX NVMe Driver Auto-Loading

The NVMe driver automatically loads at boot time using IRIX's init system and autoregister mechanism.

## Installation

    make install

This will:
1. Back up current kernel to /unix.nvme.bak (with incremental numbering)
2. Install driver module to /var/sysgen/boot/nvme.o
3. Install configuration files:
   - /var/sysgen/master.d/nvme (autoregister configuration)
   - /var/sysgen/system/nvme.sm (lboot directive)
   - /etc/init.d/nvme (init script)
   - /etc/rc2.d/S20nvme (startup script)
4. Install utilities:
   - /usr/sbin/mkparts (create device nodes)
   - /usr/sbin/nvmetest (NVMe test utility)
5. Run /etc/autoconfig to rebuild kernel configuration
6. Configure with chkconfig nvme on

**After installation, reboot the system.**

## How It Works

### Boot Sequence

1. **Kernel boots** - lboot processes /var/sysgen/system/nvme.sm (USE: nvme)
2. **Autoregister** - /var/sysgen/master.d/nvme with R flag registers the module
3. **Init system** - /etc/rc2.d/S20nvme runs at priority 20 (early boot)
4. **Driver loads** - Init script loads driver with ml ld
5. **Device nodes created** - mkparts creates /dev/dsk/dks* entries
6. **Ready for use** - NVMe drives available for mounting

### Configuration Files

**nvme.master** - Master.d configuration

    cbdRs nvme_       -    -
    $$$
    major_t nvme_majnum = ##E;

- **c** = character device
- **b** = block device
- **d** = dynamically loadable
- **R** = autoregister at boot
- **s** = software driver

**nvme.sm** - System configuration

    USE: nvme

**nvme.rc** - Startup script (S20nvme)
- Priority 20: Starts early in boot sequence
- Loads before filesystem mounts
- Creates device nodes automatically

## Verification

    # Check if driver is loaded
    ml list

    # Check for NVMe controller
    hinv | grep -i scsi

    # List NVMe devices
    ls -l /dev/dsk/dks* /dev/rdsk/dks*

    # Test with nvmetest utility
    nvmetest -i

## Uninstallation

    make uninstall

Removes all installed files and runs autoconfig to update kernel configuration.

## Restore Kernel Backup

If needed, restore the kernel backup:

    cp /unix.nvme.bak /unix
    # or for specific backup:
    cp /unix.nvme.bak.0 /unix

## Manual Control

    # Start driver manually
    /etc/init.d/nvme start

    # Stop driver
    /etc/init.d/nvme stop

    # Restart driver
    /etc/init.d/nvme restart

    # Disable auto-load at boot
    chkconfig nvme off

    # Enable auto-load at boot
    chkconfig nvme on
