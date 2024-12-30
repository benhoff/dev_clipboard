#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include "clipboard.h"

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

    /* Initialize per-bucket mutexes */
    for (i = 0; i < (1 << CLIPBOARD_HASH_BITS); i++)
        mutex_init(&clipboard_hash_locks[i]);

    for (i = 0; i < (1 << CLIPBOARD_HASH_BITS); i++)
        mutex_init(&clipboard_fasync_locks[i]);

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
    struct user_clipboard *ucb;
    struct hlist_node *tmp;
    int bkt;
    pr_info("Unregistering clipboard device and freeing buffers...\n");

    // Iterate over all user clipboards
    for (bkt = 0; bkt < (1 << CLIPBOARD_HASH_BITS); bkt++) {
        struct mutex *lock = &clipboard_hash_locks[bkt];

        mutex_lock(lock);

        hash_for_each_safe(clipboard_hash, bkt, tmp, ucb, hash_node) {
            // Wake up all readers and writers waiting on the queues
            wake_up_interruptible_all(&ucb->read_queue);
            wake_up_interruptible_all(&ucb->write_queue);
        }

        mutex_unlock(lock);
    }
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
