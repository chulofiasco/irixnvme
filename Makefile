#!smake
#
# Makefile for IRIX NVMe Driver
#
# This builds a loadable kernel module for NVMe support on IRIX 6.5
#
# Uses smake (SGI's parallel make) and follows IRIX loadable module conventions
#

# Target CPU board - change this based on your system
# Common values: IP32 (O2), IP30 (Octane), IP27 (Origin), IP22 (Indy/Indigo2)
CPUBOARD=IP30

# Include the IRIX kernel loadable I/O module makefile
# This provides $(CC), $(LD), $(CFLAGS), $(LDFLAGS), $(ML), etc.
include /var/sysgen/Makefile.kernloadio

COMMON_FLAGS=
COMMON_LDFLAGS=-v
COMMON_CFLAGS=-O3
DEBUG_CFLAGS=-g3 -DDEBUG -DNVME_DEBUG -DNVME_DBG_CMD
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
	@echo ""
	@echo "=========================================="
	@echo "NVMe Driver Build Complete!"
	@echo "=========================================="
	@echo ""
	@echo "Module: $(MODULE)"
	@echo "Platform: $(CPUBOARD)"
	@echo ""
	@echo "Next Steps - Choose ONE option:"
	@echo ""
	@echo "OPTION 1: Loadable Module (Dynamic)"
	@echo "  - For testing and development"
	@echo "  - Load/unload without rebooting"
	@echo "  - Run: make load    (load driver now)"
	@echo "  - Run: make unload  (unload driver)"
	@echo ""
	@echo "OPTION 2: Static Kernel (Permanent Install)"
	@echo "  - Auto-loads at every boot"
	@echo "  - Registers with kernel configuration"
	@echo "  - Run: make install"
	@echo "  - Then reboot system"
	@echo ""
	@echo "OPTION 3: Quick Setup (Automated)"
	@echo "  - Run: make setup   (load + makeparts + test)"
	@echo ""
	@echo "=========================================="
	@echo ""

# Debug build with symbols and debug flags
debug: clean
debug: COMMON_CFLAGS=$(DEBUG_CFLAGS)
debug: $(MODULE)
	@echo "Debug build complete with -g3 -DDEBUG -DNVME_DEBUG"

# Link all object files into a single loadable module
# Use ld with -r flag to create relocatable object (kernel module)
$(MODULE): $(OBJS)
	$(LD) $(LDFLAGS) -r $(OBJS) -o $(MODULE)

# Compile C files to object files
# Makefile.kernloadio should provide proper CFLAGS for loadable modules
.c.o:
	$(CC) $(CFLAGS) $(MYCFLAGS) -c $<

# Dependencies
nvmedrv.o: nvmedrv.c $(HDRS)
nvme_scsi.o: nvme_scsi.c $(HDRS)
nvme_cmd.o: nvme_cmd.c $(HDRS)
nvme_cpl.o: nvme_cpl.c $(HDRS)

# Load the driver into the running kernel using ml (module loader)
# Register as character device with auto-unload to properly free major number
# -c = character device
# -p nvme_ = driver prefix
# -s -1 = auto-assign major device number
# -t 0 = auto-unload delay of 0 seconds (immediate cleanup)
# -v = verbose output
load: $(MODULE)
	@echo "Loading NVMe driver module..."
	$(ML) ld -v -c $(MODULE) -p nvme_ -s -1 -t 0

# Unload the driver from the kernel
# ml unld requires module ID, so we grep it from ml list
# Extract the ID number that appears after "Id: " and before the next space
unload:
	@echo "Searching for nvme module to unload..."
	@$(ML) list | grep -i nvme || echo "No nvme module found"
	@for id in `$(ML) list | grep 'prefix nvme_' | sed 's/Id: *\([0-9]*\).*/\1/'`; do \
		echo "Unloading module ID $$id..."; \
		$(ML) unld -v $$id; \
	done

# Reload the driver (unload then load)
reload: unload load

# List loaded modules
list:
	$(ML) list

# Clean build artifacts
clean:
	rm -f $(OBJS) $(MODULE) $(MKPARTS) $(NVMETEST)

# Install target - Install driver permanently into the kernel with auto-registration
# This copies the module to /var/sysgen/boot/ and updates the system configuration
# The 'R' flag in nvme.master enables automatic registration at boot
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
	@mkdir -p /usr/share/catman/local/cat7
	@if [ -f mkparts.1 ]; then \
		cp mkparts.1 /usr/share/catman/local/cat1/mkparts.1; \
	fi
	@if [ -f nvmetest.1 ]; then \
		cp nvmetest.1 /usr/share/catman/local/cat1/nvmetest.1; \
	fi
	@if [ -f nvme.7 ]; then \
		cp nvme.7 /usr/share/catman/local/cat7/nvme.7; \
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

# Uninstall target - Remove permanently installed driver
uninstall:
	@echo "Uninstalling NVMe driver..."
	@echo "Checking if driver is loaded..."
	@if /sbin/ml list | grep -q "prefix nvme_"; then \
		echo "Unloading driver first..."; \
		for id in `/sbin/ml list | grep 'prefix nvme_' | sed 's/Id: *\([0-9]*\).*/\1/'`; do \
			/sbin/ml unld -v $$id; \
		done; \
	fi
	@if [ -f /var/sysgen/boot/nvme.o ]; then \
		rm -f /var/sysgen/boot/nvme.o; \
		echo "Removed /var/sysgen/boot/nvme.o"; \
	else \
		echo "Driver not found in /var/sysgen/boot/"; \
	fi
	@if [ -f /var/sysgen/master.d/nvme ]; then \
		rm -f /var/sysgen/master.d/nvme; \
		echo "Removed /var/sysgen/master.d/nvme"; \
	fi
	@if [ -f /var/sysgen/system/nvme.sm ]; then \
		rm -f /var/sysgen/system/nvme.sm; \
		echo "Removed /var/sysgen/system/nvme.sm"; \
	fi
	@if [ -f /etc/init.d/nvme ]; then \
		/sbin/chkconfig nvme off; \
		rm -f /etc/init.d/nvme; \
		rm -f /etc/rc0.d/K80nvme /etc/rc2.d/S05nvme; \
		echo "Removed /etc/init.d/nvme and rc.d entries"; \
	fi
	@if [ -f /sbin/mkparts ]; then \
		rm -f /sbin/mkparts; \
		echo "Removed /sbin/mkparts"; \
	fi
	@if [ -f /sbin/nvmetest ]; then \
		rm -f /sbin/nvmetest; \
		echo "Removed /sbin/nvmetest"; \
	fi
	@if [ -L /usr/bin/mkparts ]; then \
		rm -f /usr/bin/mkparts; \
		echo "Removed /usr/bin/mkparts symlink"; \
	fi
	@if [ -L /usr/bin/nvmetest ]; then \
		rm -f /usr/bin/nvmetest; \
		echo "Removed /usr/bin/nvmetest symlink"; \
	fi
	@if [ -f /usr/share/catman/local/cat1/mkparts.1 ]; then \
		rm -f /usr/share/catman/local/cat1/mkparts.1; \
		echo "Removed mkparts man page"; \
	fi
	@if [ -f /usr/share/catman/local/cat1/nvmetest.1 ]; then \
		rm -f /usr/share/catman/local/cat1/nvmetest.1; \
		echo "Removed nvmetest man page"; \
	fi
	@if [ -f /usr/share/catman/local/cat7/nvme.7 ]; then \
		rm -f /usr/share/catman/local/cat7/nvme.7; \
		echo "Removed nvme man page"; \
	fi
	@echo "Running autoconfig to update kernel configuration..."
	@/etc/autoconfig
	@echo "Uninstall complete."


reboot:
	shutdown -y -g0 -i6

ioc:
	ioconfig -d -f /hw

# Build the mkparts utility (explicit rule)
mkparts:
	cc -o mkparts mkparts.c
	chmod +x mkparts

# Build nvmetest utility
# Controller number can be specified: smake nvmetest CTLR=3
nvmetest:
	@echo "Building nvmetest..."
	@rm -f nvmetest
	@CTLR=$${CTLR:-3}; \
	echo "Building for controller $$CTLR"; \
	cc -woff 3970 -DCTLR_NUM=$$CTLR -o nvmetest nvmetest.c; \
	chmod +x nvmetest

# Build both mkparts and nvmetest utilities
tools: mkparts nvmetest
	@echo "Built NVMe Utilities."

# Check/create partition device nodes after loading driver
# Uses the mkparts utility - controller number must be specified
# Usage: smake makeparts CTLR=3
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
	@CTLR=$${CTLR:-3}; \
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
# Note: Sequential execution to avoid race conditions
setup:
	@echo "=== NVMe Driver Setup ==="
	@echo ""
	@echo "Step 1: Building driver module..."
	@$(MAKE) all
	@echo ""
	@echo "Step 2: Detecting next available controller number..."
	@HIGHEST=`hinv -c disk | grep 'SCSI controller' | sed 's/.*controller \([0-9]*\).*/\1/' | sort -n | tail -1`; \
	test -z "$$HIGHEST" && HIGHEST=-1; \
	CTLR=`expr $$HIGHEST + 1`; \
	echo "Highest existing controller: $$HIGHEST, will use: $$CTLR"; \
	echo "$$CTLR" > /tmp/nvme_ctlr.txt
	@echo ""
	@echo "Step 3: Unloading any existing driver..."
	@$(MAKE) unload 2>/dev/null || echo "No previous driver loaded"
	@echo ""
	@echo "Step 4: Loading driver..."
	@$(MAKE) load > /tmp/nvme_load.log 2>&1
	@sleep 2
	@echo ""
	@echo "Step 5: Building utilities..."
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	$(MAKE) nvmetest CTLR=$$CTLR; \
	$(MAKE) mkparts; \
	test -f mkparts && chmod +x mkparts; \
	test -f nvmetest && chmod +x nvmetest
	@echo ""
	@echo "Step 6: Creating partition devices..."
	@CTLR=`cat /tmp/nvme_ctlr.txt`; \
	echo "Running prtvtoc to trigger partition device creation..."; \
	prtvtoc /dev/rdsk/dks$${CTLR}d0vol > /dev/null 2>&1 || echo "Note: prtvtoc may have failed"
	@echo ""
	@echo "Step 7: Verifying partition devices..."
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

dp:
	diskperf -D -W -c100m -r4k -m4m /nvme/lol


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
	@echo "  unload   - Unload the driver from the kernel"
	@echo "  reload   - Unload then reload the driver"
	@echo "  makeparts- Create partition device nodes for NVMe disk"
	@echo "  test     - Build nvmetest utility with auto-detected controller"
	@echo "  setup    - Complete setup: load + makeparts + test"
	@echo "  list     - List loaded modules"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Install module to /var/sysgen/boot"
	@echo "  uninstall- Remove installed module"
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
	@echo "  autoconfig       # Update kernel config"
	@echo "  smake reboot     # Reboot to load at boot time"
	@echo ""
	@echo "Module Loader Commands:"
	@echo "  ml ld -p nvme_ nvme.o   # Load module"
	@echo "  ml unld -p nvme_        # Unload by prefix"
	@echo "  ml list                 # List modules"

.PHONY: all debug tools mkparts nvmetest makeparts test setup load unload reload list clean install uninstall help reboot ioc dp
