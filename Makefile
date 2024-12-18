# Makefile for the Clipboard Kernel Module

# ================================
# Version Integration
# ================================

# Read the version from the VERSION file using an absolute path
VERSION := $(shell cat $(PWD)/VERSION)

# ================================
# Module Configuration
# ================================

# Name of the module
MODULE_NAME := clipboard

# Kernel build directory
KERNEL_DIR := /lib/modules/$(shell uname -r)/build

# Source files
SRC_FILES := clipboard_main.c clipboard_helpers.c

# Object files
OBJ_FILES := $(SRC_FILES:.c=.o)

# Destination directory for compiled module
DEST_DIR := /lib/modules/$(shell uname -r)/extra

# Module to be built from object files
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-objs := clipboard_main.o clipboard_helpers.o

# ================================
# Compiler Flags
# ================================

# Pass the module version as a compiler flag using a different macro
# This avoids redefining MODULE_VERSION and prevents conflicts
ccflags-y += -DCLIPBOARD_MODULE_VERSION=\"$(VERSION)\"

# ================================
# Build Targets
# ================================

# Default target: Build the module
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# Clean target: Clean the build artifacts
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

# Install target: Install the compiled module to the destination directory
install: all
	@sudo mkdir -p $(DEST_DIR)
	@sudo cp $(MODULE_NAME).ko $(DEST_DIR)
	@sudo depmod -a
	@echo "Module $(MODULE_NAME) version $(VERSION) installed to $(DEST_DIR)"

# Uninstall target: Remove the installed module from the destination directory
uninstall:
	@sudo rm -f $(DEST_DIR)/$(MODULE_NAME).ko
	@sudo depmod -a
	@echo "Module $(MODULE_NAME) removed from $(DEST_DIR)"

# ================================
# Phony Targets
# ================================

.PHONY: all clean install uninstall

