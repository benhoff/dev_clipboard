#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
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

#define NBUCKETS (1U << CLIPBOARD_HASH_BITS)


/* Initialize hash table and bucket locks */
DEFINE_HASHTABLE(clipboard_hash, CLIPBOARD_HASH_BITS);
DEFINE_HASHTABLE(clipboard_fasync_hash, CLIPBOARD_HASH_BITS);

/* sleep‑able reader‑writer semaphores */
struct rw_semaphore clipboard_hash_sems[NBUCKETS];
struct rw_semaphore clipboard_fasync_sems[NBUCKETS];

/* Initial capacity for a new clipboard buffer */
#define INITIAL_CLIPBOARD_CAPACITY 1024

static inline struct rw_semaphore *hash_sem(uid_t uid)
{ return &clipboard_hash_sems[hash_min(uid, CLIPBOARD_HASH_BITS)]; }
static inline struct rw_semaphore *fasync_sem(uid_t uid)
{ return &clipboard_fasync_sems[hash_min(uid, CLIPBOARD_HASH_BITS)]; }

static struct user_clipboard *find_user_clipboard(uid_t uid)
{
    struct user_clipboard *ucb;
    hash_for_each_possible(clipboard_hash, ucb, hash_node, uid) {
        if (ucb->uid == uid)
            return ucb;
    }
    return NULL;
}

int clipboard_fasync_handler(int fd, struct file *file, int on)
{
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct clipboard_fasync_entry *entry;
    struct rw_semaphore *sem = fasync_sem(uid);
    // int hash = hash_min(uid, CLIPBOARD_HASH_BITS);
    int ret = 0;

    down_write(sem);
    /* find existing entry */
    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid) {
            ret = fasync_helper(fd, file, on, &entry->fasync);
            if (!on && !entry->fasync) {
                hash_del(&entry->hash_node);
                kfree(entry);
            }
            up_write(sem);
            return ret;
        }
    }
    /* create new if subscribing */
    if (on) {
        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            ret = -ENOMEM;
            up_write(sem);
            return ret;
        }
        entry->uid = uid;
        entry->fasync = NULL;
        hash_add(clipboard_fasync_hash, &entry->hash_node, uid);
        ret = fasync_helper(fd, file, on, &entry->fasync);
        if (ret < 0) {
            hash_del(&entry->hash_node);
            kfree(entry);
        }
    }
    up_write(sem);
    return ret;
}


int clipboard_release(struct inode *inode, struct file *file)
{
    struct clipboard_file_data *file_data = file->private_data;
    uid_t uid;
    struct clipboard_fasync_entry *entry;
    struct rw_semaphore *sem;

    if (!(file->f_mode & FMODE_WRITE)) {
        kfree(file_data);
        file->private_data = NULL;
        return 0;
    }
    if (!file_data->bytes_written) {
        kfree(file_data);
        file->private_data = NULL;
        return 0;
    }
    uid = from_kuid(current_user_ns(), current_fsuid());
    sem = fasync_sem(uid);
    down_write(sem);
    hash_for_each_possible(clipboard_fasync_hash, entry, hash_node, uid) {
        if (entry->uid == uid && entry->fasync)
            kill_fasync(&entry->fasync, SIGIO, POLL_IN);
    }
    up_write(sem);
    kfree(file_data);
    file->private_data = NULL;
    return 0;
}

static struct user_clipboard *get_or_create_user_clipboard_locked(struct rw_semaphore *sem, uid_t uid)
{
    struct user_clipboard *ucb;

    ucb = find_user_clipboard(uid);
    if (!ucb) {
        ucb = kzalloc(sizeof(*ucb), GFP_KERNEL);
        if (!ucb) goto out;
        ucb->uid = uid;
        ucb->capacity = INITIAL_CLIPBOARD_CAPACITY;
        ucb->buffer = vmalloc(ucb->capacity);
        if (!ucb->buffer) { kfree(ucb); ucb = NULL; goto out; }
        memset(ucb->buffer, 0, ucb->capacity);
        init_waitqueue_head(&ucb->waitq);
        ucb->size = 0;
        hash_add(clipboard_hash, &ucb->hash_node, uid);
    }
out:
    return ucb;
}

ssize_t clipboard_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
    ssize_t ret = 0;
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct rw_semaphore *sem = hash_sem(uid);

    if (!user_buf)
        return -EINVAL;

    down_read(sem);
    ucb = find_user_clipboard(uid);
    if (!ucb || *ppos >= ucb->size) {
        up_read(sem);
        return 0;
    }

    if (count > ucb->size - *ppos)
        count = ucb->size - *ppos;

    if (copy_to_user(user_buf, ucb->buffer + *ppos, count)) {
        ret = -EFAULT;
    } else {
        *ppos += count;
        ret = count;
    }
    up_read(sem);
    return ret;
}

int clipboard_open(struct inode *inode, struct file *file)
{
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct clipboard_file_data *file_data;

	struct rw_semaphore *sem = hash_sem(uid);
	down_write(sem);
    /* Find or allocate this UID’s clipboard buffer */
    ucb = get_or_create_user_clipboard_locked(sem, uid);
    if (!ucb) {
        up_write(sem);
        return -ENOMEM;
    }

    /* If opened with O_TRUNC, clear out existing contents */
    if (file->f_flags & O_TRUNC) {
        memset(ucb->buffer, 0, ucb->capacity);
        ucb->size = 0;
    }

    up_write(sem);

    /* Allocate our per-file state */
    file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);
    if (!file_data)
        return -ENOMEM;

    file_data->bytes_written = false;
    file_data->ucb = ucb;
    file->private_data = file_data;

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
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct rw_semaphore *sem = hash_sem(uid);

    if (!user_buf) {
        return -EINVAL;
	}

	down_write(sem);
    ucb = get_or_create_user_clipboard_locked(sem, uid);
    if (!ucb) {
        ret = -ENOMEM;
        goto out;
    }

    if (file->f_flags & O_APPEND)
        *ppos = ucb->size;

    if (*ppos + count > ucb->capacity) {
        ret = expand_clipboard_buffer(ucb, *ppos + count);
        if (ret)
            goto out;
    }

    if (copy_from_user(ucb->buffer + *ppos, user_buf, count)) {
        ret = -EFAULT;
        goto out;
    }

    *ppos += count;
    if (*ppos > ucb->size)
        ucb->size = *ppos;
    struct clipboard_file_data *file_data = file->private_data;
    if (file_data)
		file_data->bytes_written = true;
    wake_up_interruptible(&ucb->waitq);
    ret = count;

out:
    up_write(sem);
    return ret;
}

loff_t clipboard_llseek(struct file *file, loff_t offset, int whence)
{
    loff_t new_pos;
    struct clipboard_file_data *file_data = file->private_data;
    struct user_clipboard *ucb;
        if (!file_data)
        return -EFAULT;

    ucb = file_data->ucb;
    if (!ucb)
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

__poll_t clipboard_poll(struct file *file, poll_table *wait)
{
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    struct rw_semaphore *sem = hash_sem(uid);
    __poll_t mask = 0;

    down_read(sem);
    ucb = find_user_clipboard(uid);
    if (!ucb) {
        up_read(sem);
        return POLLOUT | POLLERR;      /* always writable but invalid for read */
    }

    /* Arm the waitqueue */
    poll_wait(file, &ucb->waitq, wait);

    /* Data ready?  (anything beyond current f_pos) */
    if (file->f_pos < ucb->size)
        mask |= POLLIN | POLLRDNORM;

    /* Device is never “full” → writable at any time */
    mask |= POLLOUT | POLLWRNORM;

    up_read(sem);
    return mask;
}

ssize_t clipboard_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    loff_t *ppos = &iocb->ki_pos;
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    size_t avail, to_copy;
    ssize_t ret = 0;
    struct rw_semaphore *sem = hash_sem(uid);

    down_read(sem);
    ucb = find_user_clipboard(uid);
    if (!ucb || *ppos >= ucb->size) goto out;
    avail = ucb->size - *ppos;
    to_copy = min(avail, iov_iter_count(to));
    if (to_copy && copy_to_iter(ucb->buffer + *ppos, to_copy, to) != to_copy) {
        ret = -EFAULT; goto out;
    }
    *ppos += to_copy;
    ret = to_copy;
out:
    up_read(sem);
    return ret;
}

ssize_t clipboard_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    loff_t *ppos = &iocb->ki_pos;
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    size_t count = iov_iter_count(from), to_copy;
    ssize_t ret = 0;
    struct rw_semaphore *sem = hash_sem(uid);

    down_write(sem);
    ucb = get_or_create_user_clipboard_locked(sem, uid);
    if (!ucb) { ret = -ENOMEM; goto out; }
    if (file->f_flags & O_APPEND) *ppos = ucb->size;
    to_copy = min(count, ucb->capacity - *ppos);
    if (to_copy < count) {
        ret = expand_clipboard_buffer(ucb, *ppos + count);
        if (ret) goto out;
        to_copy = min(count, ucb->capacity - *ppos);
        if (!to_copy) { ret = -ENOSPC; goto out; }
    }
    if (copy_from_iter(ucb->buffer + *ppos, to_copy, from) != to_copy) {
        ret = -EFAULT; goto out; }
    *ppos += to_copy;
    if (*ppos > ucb->size) ucb->size = *ppos;
    struct clipboard_file_data *file_data = file->private_data;
    if (file_data)
		file_data->bytes_written = true;
    ret = to_copy;
out:
    up_write(sem);
    return ret;
}

/* IOCTL handler to clear the clipboard */
long clipboard_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    uid_t uid = from_kuid(current_user_ns(), current_fsuid());
    struct user_clipboard *ucb;
    ssize_t ret = 0;
    struct rw_semaphore *sem = hash_sem(uid);

    down_write(sem);
    ucb = get_or_create_user_clipboard_locked(sem, uid);
    if (ucb) {
        if (cmd == CLIPBOARD_CLEAR) {
            memset(ucb->buffer, 0, ucb->capacity);
            ucb->size = 0;
        } else ret = -ENOTTY;
    }
    up_write(sem);
    return ret;
}

void free_clipboard_fasync_entries(void)
{
    struct clipboard_fasync_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    for (bkt = 0; bkt < (1 << CLIPBOARD_HASH_BITS); bkt++) {
        struct rw_semaphore *sem = &clipboard_fasync_sems[bkt];
        down_write(sem);

        hash_for_each_safe(clipboard_fasync_hash, bkt, tmp, entry, hash_node) {
            if (entry->fasync) {
                /* Notify subscriber and clean up */
                kill_fasync(&entry->fasync, POLL_HUP, POLL_HUP);
            }
            hash_del(&entry->hash_node);
            kfree(entry);
        }

        up_write(sem);
    }
}

void free_clipboard_buffers(void)
{
    struct user_clipboard *ucb;
    struct hlist_node *tmp;
    int bkt;

    for (bkt = 0; bkt < (1 << CLIPBOARD_HASH_BITS); bkt++) {
        struct rw_semaphore *sem = &clipboard_hash_sems[bkt];
        down_write(sem);

        hash_for_each_safe(clipboard_hash, bkt, tmp, ucb, hash_node) {
            hash_del(&ucb->hash_node);
            vfree(ucb->buffer);
            kfree(ucb);
        }

        up_write(sem);
    }
}
