/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/kernel.h>

#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Piyush Nagpal");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

/* Helper to compute total size across all valid buffer entries */
static loff_t aesd_total_size(struct aesd_dev *dev)
{
    loff_t size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry->buffptr != NULL) {
            size += entry->size;
        }
    }
    return size;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t remaining = count;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    while (remaining > 0) {
        struct aesd_buffer_entry *entry;
        size_t entry_offset = 0;
        size_t available;
        size_t bytes_to_copy;

        /* Find which entry corresponds to current file position */
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->buffer, *f_pos, &entry_offset);

        if (!entry)
            break;

        available = entry->size - entry_offset;
        bytes_to_copy = min(remaining, available);

        if (copy_to_user(buf + retval,
                         entry->buffptr + entry_offset,
                         bytes_to_copy)) {
            mutex_unlock(&dev->lock);
            return -EFAULT;
        }

        *f_pos += bytes_to_copy;
        retval += bytes_to_copy;
        remaining -= bytes_to_copy;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf,
                   size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    char *temp;
    size_t new_size;
    struct aesd_buffer_entry entry;
    const char *oldptr = NULL;

    if (count == 0)
        return 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Grow temporary buffer to accumulate writes until newline */
    new_size = dev->pending_write_size + count;

    temp = kmalloc(new_size, GFP_KERNEL);
    if (!temp) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }

    if (dev->pending_write_buffer) {
        memcpy(temp,
               dev->pending_write_buffer,
               dev->pending_write_size);
    }

    if (copy_from_user(temp + dev->pending_write_size, buf, count)) {
        kfree(temp);
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    kfree(dev->pending_write_buffer);
    dev->pending_write_buffer = temp;
    dev->pending_write_size = new_size;

    /* Only commit to circular buffer when we see a newline */
    if (memchr(dev->pending_write_buffer, '\n',
               dev->pending_write_size)) {

        entry.buffptr = dev->pending_write_buffer;
        entry.size = dev->pending_write_size;

        /* If full, this slot will be overwritten → save pointer to free */
        if (dev->buffer.full) {
            oldptr = dev->buffer.entry[dev->buffer.in_offs].buffptr;
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &entry);

        if (oldptr)
            kfree(oldptr);

        dev->pending_write_buffer = NULL;
        dev->pending_write_size = 0;
    }

    retval = count;

    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t size;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    size = aesd_total_size(dev);

    mutex_unlock(&dev->lock);

    return fixed_size_llseek(filp, offset, whence, size);
}

static long aesd_ioctl(struct file *filp,
                       unsigned int cmd,
                       unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t new_pos = 0;
    uint8_t i;
    uint8_t entry_count;
    uint8_t entry_index;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;
    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;

    if (copy_from_user(&seekto,
        (struct aesd_seekto __user *)arg,
        sizeof(seekto)))
        return -EFAULT;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Number of valid entries in buffer */
    entry_count = dev->buffer.full ?
        AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED :
        dev->buffer.in_offs;

    if (seekto.write_cmd >= entry_count) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    /* Sum sizes of all commands before target */
    for (i = 0; i < seekto.write_cmd; i++) {
        entry_index = (dev->buffer.out_offs + i) %
            AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        new_pos += dev->buffer.entry[entry_index].size;
    }

    /* Now locate target command */
    entry_index = (dev->buffer.out_offs +
                  seekto.write_cmd) %
        AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    if (seekto.write_cmd_offset >=
        dev->buffer.entry[entry_index].size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    new_pos += seekto.write_cmd_offset;

    /*
     * IMPORTANT: set f_pos while still holding the lock.
     * Prevents race where buffer changes after we compute offset.
     */
    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    return 0;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;

    err = cdev_add(&dev->cdev, devno, 1);
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    if (result < 0)
        return result;

    aesd_major = MAJOR(dev);

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    aesd_device.pending_write_buffer = NULL;
    aesd_device.pending_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);
    if (result)
        unregister_chrdev_region(dev, 1);

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry,
        &aesd_device.buffer, index) {

        if (entry->buffptr) {
            kfree(entry->buffptr);
        }
    }

    if (aesd_device.pending_write_buffer) {
        kfree(aesd_device.pending_write_buffer);
    }

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);