#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>

/* Hash table configuration */
#define CLIPBOARD_HASH_BITS 10  // 2^10 = 1024 buckets

/* Set a reasonable max capacity to prevent memory abuse */
#define MAX_CLIPBOARD_CAPACITY (1024 * 1024) // 1 MB for example

/* IOCTL commands */
#define CLIPBOARD_MAGIC 'C'
#define CLIPBOARD_CLEAR _IO(CLIPBOARD_MAGIC, 1)
#define CLIPBOARD_SUBSCRIBE _IO(CLIPBOARD_MAGIC, 2)
#define CLIPBOARD_UNSUBSCRIBE _IO(CLIPBOARD_MAGIC, 3)

struct user_clipboard {
    uid_t uid;
    char *buffer;
    size_t size;
    size_t capacity;

    /* Optional statistics */
    unsigned long read_count;
    unsigned long write_count;

    struct hlist_node hash_node;
	wait_queue_head_t wait_queue;
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
unsigned int clipboard_poll(struct file *file, poll_table *wait);

#endif // CLIPBOARD_H
