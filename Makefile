#!smake
#
# Makefile for IRIX NVMe Driver
#
# This builds a loadable kernel module for NVMe support on IRIX 6.5
#
# Uses smake (SGI's parallel make) and follows IRIX loadable module conventions
#

# Target CPU board - override with: smake CPUBOARD=IP35
# Common values: IP32 (O2), IP30 (Octane), IP27 (Origin), IP35 (Tezro), IP22 (Indy/Indigo2)
CPUBOARD?=IP30

# Include the IRIX kernel loadable I/O module makefile
# This provides $(CC), $(LD), $(CFLAGS), $(LDFLAGS), $(ML), etc.
include /var/sysgen/Makefile.kernloadio

COMMON_FLAGS=
COMMON_LDFLAGS=-v
COMMON_CFLAGS=-O3
LDFLAGS_IP35=-nostdlib -64 -mips4
LDFLAGS_IP30=-nostdlib -64 -mips4
LDFLAGS_IP32=-nostdlib -n32 -mips3
MYCFLAGS_IP35=-mips4 -DPTE_64BIT
MYCFLAGS_IP30=-mips4 -DPTE_64BIT -DHEART_INVALIDATE_WAR
MYCFLAGS_IP32=-mips3

#if $(CPUBOARD) == "IP30"
MYCFLAGS=$(MYCFLAGS_IP30) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP30) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#elif $(CPUBOARD) == "IP32"
MYCFLAGS=$(MYCFLAGS_IP32) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP32) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#elif $(CPUBOARD) == "IP35"
MYCFLAGS=$(MYCFLAGS_IP35) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP35) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#else
#endif
# Define module loader tool
ML=ml

# Source files
SRCS = nvmedrv.c nvme_scsi.c nvme_cmd.c nvme_cpl.c
OBJS = $(SRCS:.c=.o)

# Header dependencies
HDRS = nvme.h nvmedrv.h

# Module name
MODULE = nvme.o

# Utilities
MKPARTS = mkparts
NVMETEST = nvmetest

# Default target
all: $(MODULE)

# Link all object files into a single loadable module
# Use ld with -r flag to create relocatable object (kernel module)
$(MODULE): $(OBJS)
	$(LD) $(LDFLAGS) -r $(OBJS) -o $(MODULE) 2>&1 | grep -v "^ld64:" || true

# Compile C files to object files
# Makefile.kernloadio should provide proper CFLAGS for loadable modules
# Suppress harmless BHV_VERSION macro redefinition warnings
.c.o:
	$(CC) $(CFLAGS) $(MYCFLAGS) -c $< 2>&1 | grep -v -E "(BHV_VERSION|redefinition|File = /usr/include|cc-1047|^\s*\^|^\s*$$)" || true

# Dependencies
nvmedrv.o: nvmedrv.c $(HDRS)
nvme_scsi.o: nvme_scsi.c $(HDRS)
nvme_cmd.o: nvme_cmd.c $(HDRS)
nvme_cpl.o: nvme_cpl.c $(HDRS)

# Debug build with extra output
debug: MYCFLAGS += -g -DDEBUG
debug: all

# Load the driver into the running kernel using ml (module loader)
# Register as character device to allow ml loading (even though we don't use it)
# -c = character device
# -p nvme_ = driver prefix
# -s 13 = major device number (arbitrary, change if conflicts)
# -v = verbose output
load: $(MODULE)
	@echo "Loading NVMe driver module..."
	$(ML) ld -v -c $(MODULE) -p nvme_ -s 13

# List loaded modules
list:
	@echo "=== All Kernel Modules ==="
	$(ML) list
	@echo ""
	@echo "=== NVMe Module ==="
	@$(ML) list | grep nvme_ || echo "No nvme module loaded"

# Clean build artifacts
clean:
	rm -f $(OBJS) $(MODULE) $(MKPARTS) $(NVMETEST)

# Build the mkparts utility (explicit rule)
mkparts:
	cc -o mkparts mkparts.c
	chmod +x mkparts

# Build nvmetest utility
# Auto-detects controller from syslog, or use: smake nvmetest CTLR=X
nvmetest:
	@echo "Building nvmetest..."
	@rm -f nvmetest
	@if [ -z "$$CTLR" ]; then \
		CTLR=`grep 'nvme.*assigned adapter=' /var/adm/SYSLOG 2>/dev/null | tail -1 | sed 's/.*adapter=\([0-9][0-9]*\).*/\1/'`; \
		if [ -z "$$CTLR" ]; then \
			echo "WARNING: Could not detect NVMe controller from syslog"; \
			echo "Please specify controller: smake nvmetest CTLR=X"; \
			exit 1; \
		fi; \
	fi; \
	echo "Building for controller $$CTLR"; \
	cc -woff 3970 -DCTLR_NUM=$$CTLR -o nvmetest nvmetest.c; \
	chmod +x nvmetest

# Build both mkparts and nvmetest utilities
tools: mkparts nvmetest
	@echo "Built NVMe Utilities."

# Check/create partition device nodes after loading driver
# Auto-detects controller from syslog, or use: smake makeparts CTLR=X
makeparts:
	@if [ ! -f mkparts.c ]; then \
		echo "ERROR: mkparts.c not found!"; \
		exit 1; \
	fi
	@if [ ! -f mkparts ]; then \
		echo "Building mkparts utility..."; \
		cc -o mkparts mkparts.c || exit 1; \
		chmod +x mkparts; \
	fi
	@if [ -z "$$CTLR" ]; then \
		CTLR=`grep 'nvme.*assigned adapter=' /var/adm/SYSLOG 2>/dev/null | tail -1 | sed 's/.*adapter=\([0-9][0-9]*\).*/\1/'`; \
		if [ -z "$$CTLR" ]; then \
			echo "WARNING: Could not detect NVMe controller from syslog"; \
			echo "Please specify controller: smake makeparts CTLR=X"; \
			exit 1; \
		fi; \
	fi; \
	echo "Checking partition devices for controller $$CTLR..."; \
	./mkparts $$CTLR

# Build test utility
test:
	@if [ ! -f $(NVMETEST) ]; then \
		echo "Building nvmetest utility..."; \
		$(MAKE) nvmetest; \
	fi
	@echo "NVMe test utility ready: ./$(NVMETEST)"
	@echo "Run with: ./$(NVMETEST) -i"

# Complete workflow: load driver, create partitions, build test utility
setup:
	@echo "=== NVMe Driver Setup ==="
	@echo ""
	@echo "Step 1: Building driver module..."
	@$(MAKE) all
	@echo ""
	@echo "Step 2: Loading driver..."
	@$(MAKE) load > /tmp/nvme_load.log 2>&1
	@sleep 2
	@echo ""
	@echo "Step 3: Detecting NVMe controller number from syslog..."
	@CTLR=`grep 'nvme.*assigned adapter=' /var/adm/SYSLOG 2>/dev/null | tail -1 | sed 's/.*adapter=\([0-9][0-9]*\).*/\1/'`; \
	if [ -z "$$CTLR" ]; then \
		echo "ERROR: Could not detect NVMe controller from syslog"; \
		echo "Driver may not have loaded correctly."; \
		cat /tmp/nvme_load.log 2>/dev/null || true; \
		exit 1; \
	fi; \
	echo "NVMe controller detected at adapter: $$CTLR"; \
	echo "$$CTLR" > /tmp/nvme_ctlr.txt
	@echo ""
	@echo "Step 4: Building utilities..."
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	$(MAKE) nvmetest CTLR=$$CTLR; \
	$(MAKE) mkparts; \
	test -f mkparts && chmod +x mkparts; \
	test -f nvmetest && chmod +x nvmetest
	@echo ""
	@echo "Step 5: Creating partition devices..."
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	echo "Running prtvtoc to trigger partition device creation..."; \
	prtvtoc /dev/rdsk/dks$${CTLR}d0vol > /dev/null 2>&1 || echo "Note: prtvtoc may have failed"
	@echo ""
	@echo "Step 6: Verifying partition devices..."
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	if [ -f mkparts ]; then ./mkparts $$CTLR; fi
	@echo ""
	@echo "=== Setup Complete! ==="
	@echo ""
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	echo "NVMe controller assigned to: $$CTLR"; \
	echo ""; \
	echo "Next steps:"; \
	echo "  ./nvmetest -i          # Run inquiry test"; \
	echo "  ./nvmetest -c          # Read capacity"; \
	echo "  ./nvmetest -a          # Run all tests"; \
	echo "  ls -l /dev/dsk/dks$${CTLR}*   # Check devices"; \
	echo "  mount /dev/dsk/dks$${CTLR}d0s0 /mnt  # Mount filesystem"; \
	echo ""

# Install target - Install driver permanently into the kernel with auto-registration
install: $(MODULE)
	@echo "Installing NVMe driver with auto-registration..."
	@if [ ! -d /var/sysgen/boot ]; then \
		echo "ERROR: /var/sysgen/boot directory not found"; \
		exit 1; \
	fi
	@echo "Backing up current kernel..."
	@if [ -f /unix ]; then \
		BACKUP=/unix.nvme.bak; \
		if [ -f $$BACKUP ]; then \
			COUNT=0; \
			while [ -f $$BACKUP.$$COUNT ]; do \
				COUNT=`expr $$COUNT + 1`; \
			done; \
			cp /unix $$BACKUP.$$COUNT; \
			echo "Kernel backed up to $$BACKUP.$$COUNT"; \
		else \
			cp /unix $$BACKUP; \
			echo "Kernel backed up to $$BACKUP"; \
		fi; \
	fi
	cp $(MODULE) /var/sysgen/boot/nvme.o
	@echo "Installing master.d configuration..."
	cp nvme.master /var/sysgen/master.d/nvme
	@echo "Installing system configuration..."
	cp nvme.sm /var/sysgen/system/nvme.sm
	@echo "Installing init script..."
	cp nvme.init /etc/init.d/nvme
	chmod 755 /etc/init.d/nvme
	@echo "Creating rc.d startup symlink..."
	ln -sf /etc/init.d/nvme /etc/rc2.d/S05nvme
	@echo "Configuring chkconfig for auto-load at boot..."
	/sbin/chkconfig -f nvme on
	@echo "Installing utilities to /sbin..."
	@if [ ! -f $(MKPARTS) ]; then \
		echo "Building mkparts..."; \
		$(MAKE) mkparts; \
	fi
	cp $(MKPARTS) /sbin/mkparts
	chmod 755 /sbin/mkparts
	@if [ ! -f $(NVMETEST) ]; then \
		echo "Building nvmetest..."; \
		$(MAKE) nvmetest; \
	fi
	cp $(NVMETEST) /sbin/nvmetest
	chmod 755 /sbin/nvmetest
	@echo "Creating symlinks in /usr/bin..."
	@ln -sf /sbin/mkparts /usr/bin/mkparts
	@ln -sf /sbin/nvmetest /usr/bin/nvmetest
	@echo "Installing man pages..."
	@mkdir -p /usr/share/catman/local/cat1
	@if [ -f mkparts.1 ]; then \
		cp mkparts.1 /usr/share/catman/local/cat1/mkparts.1; \
	fi
	@if [ -f nvmetest.1 ]; then \
		cp nvmetest.1 /usr/share/catman/local/cat1/nvmetest.1; \
	fi
	@echo ""
	@echo "Driver installed to /var/sysgen/boot/nvme.o"
	@echo "Configuration files installed:"
	@echo "  /var/sysgen/master.d/nvme (with R flag for autoregister)"
	@echo "  /var/sysgen/system/nvme.sm (USE: nvme)"
	@echo "  /etc/init.d/nvme (init script)"
	@echo "  /etc/rc2.d/S05nvme -> /etc/init.d/nvme (symlink)"
	@echo "Utilities installed:"
	@echo "  /sbin/mkparts (create device nodes)"
	@echo "  /sbin/nvmetest (NVMe test utility)"
	@echo "  /usr/bin/mkparts -> /sbin/mkparts (symlink)"
	@echo "  /usr/bin/nvmetest -> /sbin/nvmetest (symlink)"
	@echo ""
	@echo "Running autoconfig to rebuild kernel configuration..."
	@/etc/autoconfig
	@echo ""
	@echo "Installation complete!"
	@echo ""
	@echo "To verify:"
	@echo "  ml list  (should show nvme_ loaded)"
	@echo "  hinv | grep -i scsi"
	@echo "  ls -l /dev/dsk/dks*"
	@echo ""
	@echo "To restore kernel backup if needed:"
	@echo "  cp /unix.nvme.bak /unix"
	@echo ""
	@echo "==========================================="
	@echo "  REBOOT REQUIRED TO ACTIVATE NVMe DRIVER"
	@echo "==========================================="
	@echo ""

# Uninstall target - removes driver, config files, utilities, and rebuilds kernel
uninstall:
	@echo "Uninstalling NVMe driver..."
	@echo ""
	@echo "Backing up current kernel before uninstall..."
	@if [ -f /unix ]; then \
		BACKUP=/unix.pre-uninstall.bak; \
		if [ -f $$BACKUP ]; then \
			COUNT=0; \
			while [ -f $$BACKUP.$$COUNT ]; do \
				COUNT=`expr $$COUNT + 1`; \
			done; \
			cp /unix $$BACKUP.$$COUNT; \
			echo "Kernel backed up to $$BACKUP.$$COUNT"; \
		else \
			cp /unix $$BACKUP; \
			echo "Kernel backed up to $$BACKUP"; \
		fi; \
	fi
	@echo "Disabling chkconfig auto-load..."
	@/sbin/chkconfig -f nvme off || true
	@echo "Removing chkconfig flag file..."
	@rm -f /etc/config/nvme
	@echo "Removing startup symlink..."
	@rm -f /etc/rc2.d/S05nvme
	@echo "Removing init script..."
	@rm -f /etc/init.d/nvme
	@echo "Removing system configuration..."
	@rm -f /var/sysgen/system/nvme.sm
	@echo "Removing master.d configuration..."
	@rm -f /var/sysgen/master.d/nvme
	@echo "Removing driver module..."
	@rm -f /var/sysgen/boot/nvme.o
	@echo "Removing utilities..."
	@rm -f /sbin/mkparts /sbin/nvmetest
	@echo "Removing utility symlinks..."
	@rm -f /usr/bin/mkparts /usr/bin/nvmetest
	@echo "Removing man pages..."
	@rm -f /usr/share/catman/local/cat1/mkparts.1
	@rm -f /usr/share/catman/local/cat1/nvmetest.1
	@echo ""
	@echo "Running autoconfig to rebuild kernel configuration..."
	@/etc/autoconfig
	@echo ""
	@echo "Uninstall complete!"
	@echo ""
	@echo "Files removed:"
	@echo "  /var/sysgen/boot/nvme.o (driver module)"
	@echo "  /var/sysgen/master.d/nvme (master config)"
	@echo "  /var/sysgen/system/nvme.sm (system config)"
	@echo "  /etc/init.d/nvme (init script)"
	@echo "  /etc/rc2.d/S05nvme (startup symlink)"
	@echo "  /sbin/mkparts, /sbin/nvmetest (utilities)"
	@echo "  /usr/bin/mkparts, /usr/bin/nvmetest (symlinks)"
	@echo "  /usr/share/catman/local/cat1/mkparts.1 (man page)"
	@echo "  /usr/share/catman/local/cat1/nvmetest.1 (man page)"
	@echo ""
	@echo "Kernel rebuilt without NVMe driver."
	@echo ""
	@echo "========================================="
	@echo "  REBOOT REQUIRED TO COMPLETE UNINSTALL"
	@echo "========================================="
	@echo ""

# Reboot system
reboot:
	shutdown -y -g0 -i6

# IOconfig dump
ioc:
	ioconfig -d -f /hw

# Help target
help:
	@echo "IRIX NVMe Driver Makefile"
	@echo ""
	@echo "Build using smake (SGI's parallel make)"
	@echo ""
	@echo "Quick Start:"
	@echo "  smake setup  - Complete setup (load + makeparts + test)"
	@echo "  ./nvmetest -a - Run all tests"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build nvme.o loadable module (default)"
	@echo "  debug    - Build with debug symbols and -DDEBUG"
	@echo "  tools    - Build userspace tools (mkparts + nvmetest)"
	@echo "  load     - Load the driver into the running kernel"
	@echo "  makeparts- Create partition device nodes for NVMe disk"
	@echo "  test     - Build nvmetest utility with auto-detected controller"
	@echo "  setup    - Complete setup: load + makeparts + test"
	@echo "  install  - Install module to /var/sysgen/boot/"
	@echo "  uninstall- Remove driver, config files, utilities, rebuild kernel"
	@echo "  list     - List loaded modules"
	@echo "  clean    - Remove build artifacts"
	@echo "  reboot   - Reboot the system"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  CPUBOARD - Target CPU board (currently: $(CPUBOARD))"
	@echo ""
	@echo "Typical Workflow:"
	@echo "  smake            # Build module"
	@echo "  smake setup      # Load driver, create partitions, build test"
	@echo "  ./nvmetest -a    # Run tests"
	@echo "  mount /dev/dsk/dks*d0s0 /mnt  # Mount filesystem"
	@echo ""
	@echo "Permanent Installation:"
	@echo "  smake install    # Copy to /var/sysgen/boot/"
	@echo "  smake reboot     # Reboot to activate"
	@echo ""
	@echo "Uninstallation:"
	@echo "  smake uninstall  # Remove driver and rebuild kernel"
	@echo "  smake reboot     # Reboot to complete removal"
	@echo ""
	@echo "Module Loader Commands:"
	@echo "  ml ld -p nvme_ nvme.o   # Load module"
	@echo "  ml list                 # List modules"

# Phony targets (not actual files)
.PHONY: all debug tools mkparts nvmetest makeparts test setup load list clean install uninstall reboot ioc help
