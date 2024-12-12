## Dev Clipboard Kernel Module

Dev Clipboard is a Linux kernel module that provides a per-user clipboard device. It allows each user to have their own isolated clipboard buffer, ensuring that one user's clipboard data is inaccessible to others.

- Per-User Isolation: Each user has a separate clipboard buffer identified by their UID.
- Efficient Data Management: Utilizes a hash table with per-bucket mutexes for fast and concurrent access.
- Dynamic Buffer Management: Automatically resizes clipboard buffers up to a configurable maximum capacity.
- IOCTL Interface: Provides an IOCTL command to clear the clipboard buffer from user space.


#### Installation

Option 1: Automated Installation via Bootstrap Script

You can use the provided bootstrap script. This script automates the process of installing dependencies, setting up DKMS, downloading the project, building and installing the module, and configuring it to load on startup.

```bash
curl -sSL https://raw.githubusercontent.com/benhoff/dev_clipboard/refs/heads/master/bootstrap.sh | bash

# Alternatively, using wget:
wget -qO- https://raw.githubusercontent.com/benhoff/dev_clipboard/refs/heads/master/bootstrap.sh | bash
```

Option 2: Manual installation

Pre-reqs include:
- Linux Kernel Headers: Ensure that the kernel headers matching your current kernel version are installed.
- Build Essentials: make, gcc, and other development tools.
- Root Privileges: Installing and loading kernel modules require root access.
- (optional) DKMS
  
```bash
# Arch Linux
sudo pacman -Syu linux-headers

# Fedora
sudo dnf install kernel-headers kernel-devel

# Debian/Ubuntu
sudo apt-get update
sudo apt-get install linux-headers-$(uname -r)

# Optionally, install DKMS
# Arch
sudo pacman -Syu dkms

# Fedora
sudo dnf install dkms

# Debian/Ubuntu
sudo apt-get install dkms

```

1. Clone the Repository

```bash
git clone https://github.com/benhoff/dev_clipboard.git
cd dev_clipboard
```

2. Build the Module using make or DKMS

Either use the provided Makefile to compile the kernel module.

```
cd src && make && make install
```

-Or- use DKMS
```
# Add the module to DKMS
sudo dkms add -m clipboard -v 3.0

# Build the module using DKMS
sudo dkms build -m clipboard -v 3.0

# Install the module using DKMS
sudo dkms install -m clipboard -v 3.0
```

Load the clipboard module into the kernel.

`sudo modprobe clipboard`


#### Usage

Once the module is loaded, a device file /dev/clipboard is created, allowing user-space applications to interact with the clipboard.
Reading from the Clipboard

To read data from your clipboard:

`cat /dev/clipboard`

This command reads the current contents of your clipboard buffer.
Writing to the Clipboard

To write data to your clipboard:

`echo "Your clipboard text" > /dev/clipboard`

This command writes the specified text to your clipboard buffer.
Clearing the Clipboard

To clear your clipboard buffer, use the provided IOCTL command.

#### Uninstallation

To unload and remove the clipboard kernel module:

Unload the Module

`sudo rmmod clipboard`

#### Uninstall the Module

`sudo make uninstall`

This command removes clipboard.ko from /lib/modules/$(uname -r)/extra/ and updates module dependencies.


5. Maximum Capacity Reached

Symptoms:

Write operations fail with -ENOMEM.
Clipboard cannot be resized further.

Solution:

The module enforces a maximum clipboard capacity (MAX_CLIPBOARD_CAPACITY). To increase this limit, modify the clipboard.h file and recompile the module.

#define MAX_CLIPBOARD_CAPACITY (2 * 1024 * 1024) // 2 MB

After making changes, rebuild and reinstall the module.

#### Contributing

Contributions are welcome! If you encounter issues or have suggestions for improvements, feel free to open an issue or submit a pull request.
License
