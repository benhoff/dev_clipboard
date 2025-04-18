#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hashtable.h>
#include <linux/errno.h>
#include <linux/rwsem.h>
#include "clipboard.h"

#define NBUCKETS (1U << CLIPBOARD_HASH_BITS)

/* Declare the per-bucket rwlock arrays (must match the ones in clipboard.c) */
extern struct rw_semaphore clipboard_hash_sems[NBUCKETS];
extern struct rw_semaphore clipboard_fasync_sems[NBUCKETS];

/* Define the max clipboard capacity with a default value */
unsigned long max_clipboard_capacity = 10 * 1024 * 1024; // 10 MB

/* Register the parameter with read/write permissions for root only */
module_param(max_clipboard_capacity, ulong, 0600);
MODULE_PARM_DESC(max_clipboard_capacity, "Maximum clipboard capacity in bytes");

/* Rest of your code remains unchanged */
static const struct file_operations clipboard_fops = {
    .owner = THIS_MODULE,
    .read = clipboard_read,
    .open = clipboard_open,
    .write = clipboard_write,
    .read_iter = clipboard_read_iter,
    .write_iter = clipboard_write_iter,
    .unlocked_ioctl = clipboard_ioctl,
    .fasync = clipboard_fasync_handler,
    .llseek = clipboard_llseek,
	.release = clipboard_release,
    .poll = clipboard_poll,
};

static struct miscdevice clipboard_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "clipboard",
    .fops = &clipboard_fops,
    .mode   = 0666,
};

static int __init clipboard_init(void)
{
    int ret;
    int i;

    pr_info("Initializing clipboard module with improvements...\n");

    for (i = 0; i < NBUCKETS; i++) {
        init_rwsem(&clipboard_hash_sems[i]);
        init_rwsem(&clipboard_fasync_sems[i]);
    }

    /* Register the misc device */
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
    free_clipboard_fasync_entries();
    pr_info("Clipboard module exited\n");
}

module_init(clipboard_init);
module_exit(clipboard_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SenpaiSilver, with improvements");
MODULE_DESCRIPTION("Clipboard device with per-user isolation, hash table, IOCTL, max capacity, and better logging.");
MODULE_VERSION(CLIPBOARD_MODULE_VERSION); // Use the new macro
