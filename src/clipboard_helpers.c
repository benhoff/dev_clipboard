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
#include <linux/uio.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/fs.h>

#include "clipboard.h"

/* Initialize hash table and bucket locks */
DEFINE_HASHTABLE(clipboard_hash, CLIPBOARD_HASH_BITS);
struct mutex clipboard_hash_locks[1 << CLIPBOARD_HASH_BITS];

DEFINE_HASHTABLE(clipboard_fasync_hash, CLIPBOARD_HASH_BITS);
struct mutex clipboard_fasync_locks[1 << CLIPBOARD_HASH_BITS];

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

int clipboard_fasync_handler(int fd, struct file *file, int on)
{
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct clipboard_fasync_entry *entry = NULL;
    int hash;
    int ret = 0;

    /* Find the fasync entry for this user */
    hash = hash_min(uid, CLIPBOARD_HASH_BITS);
    mutex_lock(&clipboard_fasync_locks[hash]);
    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            ret = fasync_helper(-1, file, on, &entry->fasync);
        }
    }

    /* If subscribing and no entry exists, create one */
    if (on && entry == NULL) {
        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->uid = uid;
        entry->fasync = NULL;

        hash_add(clipboard_fasync_hash, &entry->hash_node, uid);
        ret = fasync_helper(-1, file, on, &entry->fasync);
    }
out:
	mutex_unlock(&clipboard_fasync_locks[hash]);
    return ret;
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

int clipboard_open(struct inode *inode, struct file *file)
{
	uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct mutex *lock;

    lock = get_hash_lock(uid);

    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = get_or_create_user_clipboard(uid);
    if (!ucb) {
        mutex_unlock(lock);
        return -ENOMEM;
    }

    /* Handle O_TRUNC: if the file is opened with O_TRUNC, clear the buffer */
    if (file->f_flags & O_TRUNC) {
        memset(ucb->buffer, 0, ucb->capacity);
        ucb->size = 0;

        // pr_info("Clipboard buffer truncated for UID %u\n", uid);
    }

    mutex_unlock(lock);
    return 0;
}


ssize_t clipboard_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
    ssize_t ret = 0;
    uid_t uid;
    struct user_clipboard *ucb;
    struct mutex *lock;
    struct clipboard_fasync_entry *entry;
    int hash;
    int result;

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

	/* Handle O_APPEND: set ppos to the end if O_APPEND is set */
    if (file->f_flags & O_APPEND) {
        *ppos = ucb->size;
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

    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            if (entry->fasync)
                kill_fasync(&entry->fasync, SIGIO, POLL_IN);
            break;
        }
    }

out:
    mutex_unlock(lock);
    return ret;
}


ssize_t clipboard_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    loff_t *ppos = &iocb->ki_pos;
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct mutex *lock;
    size_t available, to_copy;
    ssize_t ret = 0;

    lock = get_hash_lock(uid);
    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = find_user_clipboard(uid);
    if (!ucb) {
        // No data for this user
        ret = 0;
        goto out;
    }

    if (*ppos >= ucb->size) {
        ret = 0;
        goto out;
    }

    available = ucb->size - *ppos;
    to_copy = min_t(size_t, available, iov_iter_count(to));
    if (to_copy == 0) {
        ret = 0;
        goto out;
    }

    if (copy_to_iter(ucb->buffer + *ppos, to_copy, to) != to_copy) {
        ret = -EFAULT;
        goto out;
    }

    *ppos += to_copy;
    ret = to_copy;

out:
    mutex_unlock(lock);
    return ret;
}

ssize_t clipboard_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    loff_t *ppos = &iocb->ki_pos;
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct mutex *lock;
    size_t to_copy;
    ssize_t ret = 0;
    struct clipboard_fasync_entry *entry;

    lock = get_hash_lock(uid);
    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    ucb = get_or_create_user_clipboard(uid);
    if (!ucb) {
        ret = -ENOMEM;
        goto out;
    }

    /* Handle O_APPEND: set ppos to the end if O_APPEND is set */
    if (file->f_flags & O_APPEND) {
        *ppos = ucb->size;
    }

    to_copy = min_t(size_t, iov_iter_count(from), ucb->capacity - *ppos);
    if (to_copy == 0) {
        // No more capacity
        ret = -ENOSPC;
        goto out;
    }

    if (copy_from_iter(ucb->buffer + *ppos, to_copy, from) != to_copy) {
        ret = -EFAULT;
        goto out;
    }

    *ppos += to_copy;
    if (*ppos > ucb->size)
        ucb->size = *ppos;

    ret = to_copy;

    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            if (entry->fasync)
                kill_fasync(&entry->fasync, SIGIO, POLL_IN);
            break;
        }
    }

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
    struct clipboard_fasync_entry *entry;
    int hash;
    int result;

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

    case CLIPBOARD_SUBSCRIBE:
        /* Allocate a new fasync entry */
        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            pr_err("Failed to allocate fasync entry.\n");
            ret = -ENOMEM;
            goto out;
        }
        entry->uid = uid;
        entry->fasync = NULL;

        /* Add to fasync hash table */
        hash = hash_min(uid, CLIPBOARD_HASH_BITS);
        mutex_lock(&clipboard_fasync_locks[hash]);
        hash_add(clipboard_fasync_hash, &entry->hash_node, uid);
        mutex_unlock(&clipboard_fasync_locks[hash]);

        /* Register the fasync_struct */
        result = fasync_helper(-1, file, 1, &entry->fasync);
        if (result < 0) {
            pr_err("Failed to subscribe to clipboard updates.\n");
            hash_del(&entry->hash_node);
            kfree(entry);
            ret = result;
            goto out;
        }

        pr_info("Subscribed to clipboard updates for UID %u\n", uid);
        break;

    case CLIPBOARD_UNSUBSCRIBE:
        /* Find the fasync entry for this user */
        hash = hash_min(uid, CLIPBOARD_HASH_BITS);
        hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
            if (entry->uid == uid) {
                /* Unregister the fasync_struct */
                fasync_helper(-1, file, 0, &entry->fasync);

                /* Remove from hash table and free */
                hash_del(&entry->hash_node);
                kfree(entry);

                pr_info("Unsubscribed from clipboard updates for UID %u\n", uid);
                break;
            }
        }
        break;

    default:
        ret = -ENOTTY;
        break;
    }

out:
    mutex_unlock(lock);
    return ret;
}

void free_clipboard_fasync_entries(void)
{
    struct clipboard_fasync_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    for (bkt = 0; bkt < (1 << CLIPBOARD_HASH_BITS); bkt++) {
        struct mutex *lock = &clipboard_fasync_locks[bkt];
        mutex_lock(lock);

        hash_for_each_safe(clipboard_fasync_hash, bkt, tmp, entry, hash_node) {
            /* Notify the subscriber and remove the fasync_struct */
            if (entry->fasync) {
                /* 
                 * Kill the fasync_struct by sending POLL_HUP.
                 * This effectively notifies the subscriber that the
                 * module is being unloaded and cleans up the fasync_struct.
                 */
                kill_fasync(&entry->fasync, POLL_HUP, POLL_HUP);
            }

            /* Remove from hash table and free */
            hash_del(&entry->hash_node);
            kfree(entry);
        }

        mutex_unlock(lock);
    }
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
