#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>

/* Hash table configuration */
#define CLIPBOARD_HASH_BITS 10  // 2^10 = 1024 buckets

/* Set a reasonable max capacity to prevent memory abuse */
extern unsigned long max_clipboard_capacity;

/* IOCTL commands */
#define CLIPBOARD_MAGIC 'C'
#define CLIPBOARD_CLEAR _IO(CLIPBOARD_MAGIC, 1)

struct user_clipboard {
    uid_t uid;
    char *buffer;
    size_t size;
    unsigned long capacity;

    struct hlist_node hash_node;
};

/* Structure for managing per-user fasync subscriptions */
struct clipboard_fasync_entry {
    uid_t uid;
    struct fasync_struct *fasync;
    struct hlist_node hash_node;
};

/* Declare the hash table and mutex array */
extern struct hlist_head clipboard_hash[];
extern struct mutex clipboard_hash_locks[];

extern struct hlist_head clipboard_fasync_hash[];
extern struct mutex clipboard_fasync_locks[];

/* File operations */
ssize_t clipboard_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos);
ssize_t clipboard_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos);
ssize_t clipboard_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t clipboard_write_iter(struct kiocb *iocb, struct iov_iter *from);
long clipboard_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
void free_clipboard_buffers(void);
void free_clipboard_fasync_entries(void);
int clipboard_fasync_handler(int fd, struct file *file, int on);
int clipboard_open(struct inode *inode, struct file *file);
loff_t clipboard_llseek(struct file *file, loff_t offset, int whence);
int clipboard_release(struct inode *inode, struct file *file);

#endif // CLIPBOARD_H

