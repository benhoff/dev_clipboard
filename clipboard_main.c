#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>

#include "clipboard.h"

static const struct file_operations clipboard_fops = {
    .owner = THIS_MODULE,
    .read = clipboard_read,
    .write = clipboard_write,
    .unlocked_ioctl = clipboard_ioctl,
};

static struct miscdevice clipboard_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "clipboard",
    .fops = &clipboard_fops,
};

static int __init clipboard_init(void)
{
    int ret;
    int i;

    pr_info("Initializing clipboard module with improvements...\n");

    /* Initialize per-bucket mutexes */
    for (i = 0; i < (1 << CLIPBOARD_HASH_BITS); i++)
        mutex_init(&clipboard_hash_locks[i]);

    ret = misc_register(&clipboard_dev);
    if (ret) {
        pr_err("Unable to register clipboard misc device\n");
        return ret;
    }

    pr_info("Clipboard device registered successfully with improvements\n");
    return 0;
}

static void __exit clipboard_exit(void)
{
    pr_info("Unregistering clipboard device and freeing buffers...\n");
    misc_deregister(&clipboard_dev);
    free_clipboard_buffers();
    pr_info("Clipboard module exited\n");
}

module_init(clipboard_init);
module_exit(clipboard_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SenpaiSilver, with improvements");
MODULE_DESCRIPTION("Clipboard device with per-user isolation, hash table, IOCTL, max capacity, and better logging.");
MODULE_VERSION("3.0");

