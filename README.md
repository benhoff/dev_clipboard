## Dev Clipboard Kernel Module

Dev Clipboard is a Linux kernel module that provides a per-user clipboard device. It allows each user to have their own isolated clipboard buffer, ensuring that one user's clipboard data is inaccessible to others. The module leverages a hash table with per-bucket locking to efficiently handle concurrent access from multiple users.
Features

    Per-User Isolation: Each user has a separate clipboard buffer identified by their UID.
    Efficient Data Management: Utilizes a hash table with per-bucket mutexes for fast and concurrent access.
    Dynamic Buffer Management: Automatically resizes clipboard buffers up to a configurable maximum capacity.
    IOCTL Interface: Provides an IOCTL command to clear the clipboard buffer from user space.
    Robust Synchronization: Ensures thread safety with proper mutex locking mechanisms.
    User-Space Tools: Includes example user-space tools for interacting with the clipboard device.


#### Prerequisites

    Linux Kernel Headers: Ensure that the kernel headers matching your current kernel version are installed.
    Build Essentials: make, gcc, and other development tools.
    Root Privileges: Installing and loading kernel modules require root access.

#### Installation
1. Clone the Repository

```bash
git clone https://github.com/yourusername/dev_clipboard.git
cd dev_clipboard
```

2. Ensure Kernel Headers are Installed

On Arch Linux, install the kernel headers using:

sudo pacman -Syu linux-headers

Ensure that the installed headers match your running kernel version:

`uname -r`

3. Build the Module

Use the provided Makefile to compile the kernel module.

`make`

4. Install the Module

Copy the compiled module to the appropriate directory and update module dependencies.

`sudo make install`

This command performs the following actions:

    Creates the /lib/modules/$(uname -r)/extra/ directory if it doesn't exist.
    Copies clipboard.ko to /lib/modules/$(uname -r)/extra/.
    Runs depmod -a to update the module dependency database.

5. Load the Module

Load the clipboard module into the kernel.

`sudo modprobe clipboard`

Verify that the module is loaded:

`lsmod | grep clipboard`

Check kernel logs to confirm successful loading:

`dmesg | tail`

You should see messages indicating that the clipboard module has been initialized successfully.

#### Usage

Once the module is loaded, a device file /dev/clipboard is created, allowing user-space applications to interact with the clipboard.
Reading from the Clipboard

To read data from your clipboard:

cat /dev/clipboard

This command reads the current contents of your clipboard buffer.
Writing to the Clipboard

To write data to your clipboard:

echo "Your clipboard text" > /dev/clipboard

This command writes the specified text to your clipboard buffer.
Clearing the Clipboard

To clear your clipboard buffer, use the provided IOCTL command.

#### Uninstallation

To unload and remove the clipboard kernel module:

    Unload the Module

sudo rmmod clipboard

#### Uninstall the Module

sudo make uninstall

This command removes clipboard.ko from /lib/modules/$(uname -r)/extra/ and updates module dependencies.

Verify Removal

    lsmod | grep clipboard

    The above command should return no results, indicating that the module is unloaded.


 Permission Denied When Accessing /dev/clipboard

Solution:

Ensure that your user has the necessary permissions to read and write to /dev/clipboard. You can change the device file permissions or add your user to an appropriate group.

sudo chmod 666 /dev/clipboard

Note: Adjusting permissions to 666 allows all users to read and write to the device. For better security, consider creating a dedicated group and assigning ownership accordingly.

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
