# Makefile for the Clipboard Kernel Module

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

# Default target
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# Clean target
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

# Install target
install: all
	sudo mkdir -p $(DEST_DIR)
	sudo cp $(MODULE_NAME).ko $(DEST_DIR)
	sudo depmod -a
	echo "Module $(MODULE_NAME) installed to $(DEST_DIR)"

# Uninstall target
uninstall:
	sudo rm -f $(DEST_DIR)/$(MODULE_NAME).ko
	sudo depmod -a
	echo "Module $(MODULE_NAME) removed"

.PHONY: all clean install uninstall

