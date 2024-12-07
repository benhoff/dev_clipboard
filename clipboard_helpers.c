#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/init.h>

#include "clipboard.h"

/* Initialize hash table and bucket locks */
DEFINE_HASHTABLE(clipboard_hash, CLIPBOARD_HASH_BITS);
struct mutex clipboard_hash_locks[1 << CLIPBOARD_HASH_BITS];

/* Initial capacity for a new clipboard buffer */
#define INITIAL_CLIPBOARD_CAPACITY 1024

static struct mutex *get_hash_lock(uid_t uid)
{
    int hash = hash_min(uid, CLIPBOARD_HASH_BITS);
    return &clipboard_hash_locks[hash];
}

static struct user_clipboard *find_user_clipboard(uid_t uid)
{
    struct user_clipboard *ucb;

    /* The caller must hold the hash bucket lock */
    hash_for_each_possible(clipboard_hash, ucb, hash_node, uid) {
        if (ucb->uid == uid)
            return ucb;
    }
    return NULL;
}

static struct user_clipboard *get_or_create_user_clipboard(uid_t uid)
{
    struct user_clipboard *ucb;

    ucb = find_user_clipboard(uid);
    if (ucb)
        return ucb;

    ucb = kzalloc(sizeof(*ucb), GFP_KERNEL);
    if (!ucb)
        return NULL;

    ucb->uid = uid;
    ucb->capacity = INITIAL_CLIPBOARD_CAPACITY;
    ucb->buffer = kzalloc(ucb->capacity, GFP_KERNEL);
    if (!ucb->buffer) {
        kfree(ucb);
        return NULL;
    }
    ucb->size = 0;

    hash_add(clipboard_hash, &ucb->hash_node, uid);
    return ucb;
}

ssize_t clipboard_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
    ssize_t ret = 0;
    uid_t uid;
    struct user_clipboard *ucb;
    struct mutex *lock;

    if (!user_buf)
        return -EINVAL;

    uid = from_kuid(current_user_ns(), current_fsuid());
    lock = get_hash_lock(uid);

    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = find_user_clipboard(uid);
    if (!ucb) {
        /* No data for this user */
        ret = 0;
        goto out;
    }

    /* If reading beyond current size, return 0 */
    if (*ppos >= ucb->size) {
        ret = 0;
        goto out;
    }

    /* Adjust count if it exceeds available data */
    if (count > (ucb->size - *ppos))
        count = ucb->size - *ppos;

    if (copy_to_user(user_buf, ucb->buffer + *ppos, count)) {
        pr_err("Failed to copy data to user.\n");
        ret = -EFAULT;
        goto out;
    }

    *ppos += count;
    ucb->read_count++;
    ret = count;

out:
    mutex_unlock(lock);
    return ret;
}

ssize_t clipboard_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
    ssize_t ret = 0;
    uid_t uid;
    struct user_clipboard *ucb;
    struct mutex *lock;

    if (!user_buf)
        return -EINVAL;

    uid = from_kuid(current_user_ns(), current_fsuid());
    lock = get_hash_lock(uid);

    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = get_or_create_user_clipboard(uid);
    if (!ucb) {
        pr_err("Failed to create or find user clipboard.\n");
        ret = -ENOMEM;
        goto out;
    }

    /* Check if we need to expand the buffer */
    if (*ppos + count > ucb->capacity) {
        size_t new_capacity = ucb->capacity;
        while (new_capacity < *ppos + count) {
            if (new_capacity >= MAX_CLIPBOARD_CAPACITY) {
                /* We cannot grow further */
                pr_err("Reached max clipboard capacity.\n");
                ret = -ENOMEM;
                goto out;
            }
            new_capacity *= 2;
            if (new_capacity > MAX_CLIPBOARD_CAPACITY)
                new_capacity = MAX_CLIPBOARD_CAPACITY;
        }

        if (new_capacity > ucb->capacity) {
            char *new_buf = krealloc(ucb->buffer, new_capacity, GFP_KERNEL);
            if (!new_buf) {
                pr_err("Failed to expand clipboard buffer.\n");
                ret = -ENOMEM;
                goto out;
            }
            ucb->buffer = new_buf;
            ucb->capacity = new_capacity;
        }
    }

    /* Copy data from user space */
    if (copy_from_user(ucb->buffer + *ppos, user_buf, count)) {
        pr_err("Failed to copy data from user.\n");
        ret = -EFAULT;
        goto out;
    }

    *ppos += count;
    if (*ppos > ucb->size)
        ucb->size = *ppos;

    ucb->write_count++;
    ret = count;

out:
    mutex_unlock(lock);
    return ret;
}

/* IOCTL handler to clear the clipboard */
long clipboard_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = 0;
    uid_t uid;
    struct user_clipboard *ucb;
    struct mutex *lock;

    uid = from_kuid(current_user_ns(), current_fsuid());
    lock = get_hash_lock(uid);

    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = find_user_clipboard(uid);
    if (!ucb) {
        /* No clipboard yet, nothing to clear */
        goto out;
    }

    switch (cmd) {
    case CLIPBOARD_CLEAR:
        memset(ucb->buffer, 0, ucb->capacity);
        ucb->size = 0;
        pr_info("Cleared clipboard for UID %u\n", uid);
        break;

    default:
        ret = -ENOTTY;
        break;
    }

out:
    mutex_unlock(lock);
    return ret;
}

void free_clipboard_buffers(void)
{
    struct user_clipboard *ucb;
    struct hlist_node *tmp;
    int bkt;

    /* Iterate over all buckets */
    for (bkt = 0; bkt < (1 << CLIPBOARD_HASH_BITS); bkt++) {
        struct mutex *lock = &clipboard_hash_locks[bkt];

        /* Lock the bucket to ensure thread safety */
        mutex_lock(lock);

        /* Iterate safely over the hash table bucket */
        hash_for_each_safe(clipboard_hash, bkt, tmp, ucb, hash_node) {
            /* Remove from hash table */
            hash_del(&ucb->hash_node);

            /* Free resources */
            kfree(ucb->buffer);
            kfree(ucb);
        }

        /* Unlock the bucket */
        mutex_unlock(lock);
    }
}

