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
#include <linux/vmalloc.h>

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

    /* Compute the hash based on UID */
    hash = hash_min(uid, CLIPBOARD_HASH_BITS);
    mutex_lock(&clipboard_fasync_locks[hash]);

    /* Find the fasync entry for this UID */
    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            /* Register or deregister the fasync_struct */
            ret = fasync_helper(fd, file, on, &entry->fasync);
            
            /* If deregistering and no more fasync structs, clean up */
            if (!on && !entry->fasync) {
                hash_del(&entry->hash_node);
                kfree(entry);
            }
            break; /* UID is unique, no need to continue */
        }
    }

    /* If subscribing and no entry exists, create one */
    if (on && entry == NULL) {
        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            ret = -ENOMEM;
            goto out;
        }

        entry->uid = uid;
        entry->fasync = NULL;

        hash_add(clipboard_fasync_hash, &entry->hash_node, uid);
        ret = fasync_helper(fd, file, on, &entry->fasync);
        if (ret < 0) {
            /* Cleanup if fasync_helper fails */
            hash_del(&entry->hash_node);
            kfree(entry);
        }
    }

out:
    mutex_unlock(&clipboard_fasync_locks[hash]);
    return ret;
}

int clipboard_release(struct inode *inode, struct file *file)
{
    uid_t uid;
    struct clipboard_fasync_entry *entry = NULL;
    int hash;
    int ret = 0;

    /* Check if the file was opened with write access */
    if (!(file->f_mode & FMODE_WRITE)) {
        /* If opened read-only, do not call kill_fasync */
        return ret;
    }

    /* Retrieve the UID of the current process */
    uid = from_kuid(current_user_ns(), current_fsuid());

    /* Compute the hash based on UID */
    hash = hash_min(uid, CLIPBOARD_HASH_BITS);

    /* Lock the corresponding mutex to protect the hash table */
    mutex_lock(&clipboard_fasync_locks[hash]);

    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            if (entry->fasync)
                kill_fasync(&entry->fasync, SIGIO, POLL_IN);
            break;
        }
    }

    /* Unlock the mutex after operation */
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

    // Allocate buffer with vmalloc
    ucb->buffer = vmalloc(ucb->capacity);
    if (!ucb->buffer) {
        kfree(ucb);
        return NULL;
    }
    memset(ucb->buffer, 0, ucb->capacity);
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

static int expand_clipboard_buffer(struct user_clipboard *ucb, size_t required_size)
{
    unsigned long new_capacity = ucb->capacity;

    /* Determine the new capacity by doubling until it fits or reaches the max limit */
    while (new_capacity < required_size) {
        if (new_capacity >= max_clipboard_capacity) {
            pr_err("Reached max clipboard capacity of %zu bytes.\n", (size_t)max_clipboard_capacity);
            return -ENOMEM;
        }
        new_capacity *= 2;
        if (new_capacity > max_clipboard_capacity)
            new_capacity = max_clipboard_capacity;
    }

    /* Allocate new buffer with the increased capacity */
    char *new_buf_v = vmalloc(new_capacity);
    if (!new_buf_v) {
        pr_err("Failed to expand clipboard buffer to %zu bytes.\n", (size_t)new_capacity);
        return -ENOMEM;
    }

    /* Copy existing data to the new buffer */
    memcpy(new_buf_v, ucb->buffer, ucb->size);

    /* Zero the newly allocated memory beyond the current size */
    memset(new_buf_v + ucb->size, 0, new_capacity - ucb->size);

    /* Free the old buffer */
    vfree(ucb->buffer);

    /* Update the buffer pointer and capacity */
    ucb->buffer = new_buf_v;
    ucb->capacity = new_capacity;

    return 0;
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

    /* Handle O_APPEND: set ppos to the end if O_APPEND is set */
    if (file->f_flags & O_APPEND) {
        *ppos = ucb->size;
    }

    /* Check if we need to expand the buffer */
    if (*ppos + count > ucb->capacity) {
        size_t required_size = *ppos + count;

        ret = expand_clipboard_buffer(ucb, required_size);
        if (ret < 0)
            goto out;
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

    ret = count;

out:
    mutex_unlock(lock);
    return ret;
}

loff_t clipboard_llseek(struct file *file, loff_t offset, int whence)
{
    loff_t new_pos;
    struct user_clipboard *uc = file->private_data;

    if (!uc)
        return -EFAULT;

    switch (whence) {
    case SEEK_SET: // Absolute offset
        new_pos = offset;
        break;
    case SEEK_CUR: // Relative to current position
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END: // Relative to file size (not relevant for non-regular files)
        return -EINVAL; // Not supported
    default:
        return -EINVAL; // Invalid whence
    }

    if (new_pos < 0)
        return -EINVAL;

    file->f_pos = new_pos;
    return new_pos;
}

ssize_t clipboard_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    // struct file *file = iocb->ki_filp;
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

    /* Acquire the appropriate hash lock based on UID */
    lock = get_hash_lock(uid);
    if (mutex_lock_interruptible(lock))
        return -ERESTARTSYS;

    /* Retrieve or create the user clipboard */
    ucb = get_or_create_user_clipboard(uid);
    if (!ucb) {
        ret = -ENOMEM;
        goto out;
    }

    /* Handle O_APPEND: set ppos to the end if O_APPEND is set */
    if (file->f_flags & O_APPEND) {
        *ppos = ucb->size;
    }

    /* Calculate the maximum possible bytes to copy without expanding */
    to_copy = min_t(size_t, iov_iter_count(from), ucb->capacity - *ppos);

    /* If insufficient space, attempt to expand the buffer */
    if (to_copy < iov_iter_count(from)) {
        size_t required = *ppos + iov_iter_count(from);

        ret = expand_clipboard_buffer(ucb, required);
        if (ret < 0)
            goto out;

        /* Recalculate how much can be copied after expansion */
        to_copy = min_t(size_t, iov_iter_count(from), ucb->capacity - *ppos);
    }

    /* If still no space after expansion, return ENOSPC */
    if (to_copy == 0) {
        pr_err("No space available in clipboard buffer after expansion.\n");
        ret = -ENOSPC;
        goto out;
    }

    /* Perform the copy from user space */
    if (copy_from_iter(ucb->buffer + *ppos, to_copy, from) != to_copy) {
        pr_err("Failed to copy data from user space.\n");
        ret = -EFAULT;
        goto out;
    }

    /* Update the file position */
    *ppos += to_copy;

    /* Update the clipboard size if necessary */
    if (*ppos > ucb->size)
        ucb->size = *ppos;

    ret = to_copy;

out:
    /* Release the mutex lock */
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
            vfree(ucb->buffer); // Use vfree instead of kfree
            kfree(ucb);
        }

        /* Unlock the bucket */
        mutex_unlock(lock);
    }
}

