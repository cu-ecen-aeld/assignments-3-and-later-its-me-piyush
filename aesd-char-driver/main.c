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

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Piyush Nagpal");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

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

    PDEBUG("open");

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t remaining = count;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    while (remaining > 0) {
        struct aesd_buffer_entry *entry;
        size_t entry_offset = 0;
        size_t available = 0;
        size_t bytes_to_copy = 0;

        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->buffer, *f_pos, &entry_offset);

        if (entry == NULL) {
            break;
        }

        available = entry->size - entry_offset;
        bytes_to_copy = min(remaining, available);

        if (copy_to_user(buf + retval, entry->buffptr + entry_offset, bytes_to_copy)) {
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

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if (count == 0)
        return 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    new_size = dev->pending_write_size + count;

    temp = kmalloc(new_size, GFP_KERNEL);
    if (!temp) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }

    if (dev->pending_write_buffer) {
        memcpy(temp, dev->pending_write_buffer, dev->pending_write_size);
    }

    if (copy_from_user(temp + dev->pending_write_size, buf, count)) {
        kfree(temp);
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    kfree(dev->pending_write_buffer);
    dev->pending_write_buffer = temp;
    dev->pending_write_size = new_size;

    if (memchr(dev->pending_write_buffer, '\n', dev->pending_write_size)) {

        entry.buffptr = dev->pending_write_buffer;
        entry.size = dev->pending_write_size;

        if (dev->buffer.full) {
            oldptr = dev->buffer.entry[dev->buffer.in_offs].buffptr;
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &entry);

        if (oldptr != NULL) {
            kfree(oldptr);
        }

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

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;

    err = cdev_add(&dev->cdev, devno, 1);

    if (err)
        printk(KERN_ERR "Error %d adding aesd cdev", err);

    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);

    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    aesd_device.pending_write_buffer = NULL;
    aesd_device.pending_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if (result) {
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr != NULL) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
            entry->size = 0;
        }
    }

    if (aesd_device.pending_write_buffer != NULL) {
        kfree(aesd_device.pending_write_buffer);
        aesd_device.pending_write_buffer = NULL;
        aesd_device.pending_write_size = 0;
    }

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);