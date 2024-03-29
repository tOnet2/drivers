#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include "scull_main.h"

static int scull_major = 0;
static int scull_minor = 0;
static int scull_nr_devs = 4;
static int scull_quantum = 4000;
static int scull_qset = 1000;
static char *scull_name = "scull";
module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

/* -------------------------------- */

static struct scull_dev d = {
	.node = {
		{ .size = 0, .data = NULL, .general_offset = 0 },
		{ .size = 0, .data = NULL, .general_offset = 0 },
		{ .size = 0, .data = NULL, .general_offset = 0 },
		{ .size = 0, .data = NULL, .general_offset = 0 },
	}
		
};

static struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.llseek = scull_llseek,
	.read = scull_read,
	.write = scull_write,
/*	.ioctl = scull_ioctl,*/
	.open = scull_open,
	.release = scull_release,
};

static int reg_dev(void)
{
	int result;

	if (scull_major) {
		d.dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(d.dev, scull_nr_devs, scull_name);
	} else {
		result = alloc_chrdev_region(&d.dev, scull_minor, scull_nr_devs, scull_name);
		scull_major = MAJOR(d.dev);
	}

	if (result < 0)
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);

	return result;
}

static void reg_cdev(void)
{
	int err;
	int i;

	for (i = 0; i < scull_nr_devs; i++) {
		d.node[i].quantum = scull_quantum;
		d.node[i].qset = scull_qset;
		sema_init(&d.node[i].sem, 1);
		cdev_init(&d.node[i].cdev, &scull_fops);
		d.node[i].cdev.owner = THIS_MODULE;
		err = cdev_add(&d.node[i].cdev, d.dev + i, 1);
		if (err)
			printk(KERN_WARNING "Error %d, adding scull %d", err, scull_nr_devs);
	}
}

static int scull_trim(void)
{
	struct scull_qset *next, *dptr;
	int qset = scull_qset;
	int i, j;

	for (i = 0; i < 4; i++) {
		for (dptr = d.node[i].data; dptr; dptr = next) {
			if (dptr->data) {
				for (j = 0; j < qset; j++)
					kfree(dptr->data[j]);
				kfree(dptr->data);
			}
			next = dptr->next;
			kfree(dptr);
		}
	}

	return 0;
}

static int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_cdev *scd;

	scd = container_of(inode->i_cdev, struct scull_cdev, cdev);
	filp->private_data = scd;

	return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t scull_read(struct file *filp, char __user *buf,
	size_t count, loff_t *f_pos)
{
	struct scull_cdev *scd = filp->private_data;
	struct scull_qset *qsptr;
	int quantum = scd->quantum;
	int qset = scd->qset;
	int item, s_pos, q_pos, rest;
	unsigned long itemsize = quantum * qset;
	
	ssize_t retval = 0;

	if (down_interruptible(&scd->sem))
		return -ERESTARTSYS;
	if (*f_pos >= scd->general_offset)
		goto out;
	if (*f_pos + count > scd->general_offset)
		count = scd->general_offset - *f_pos;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	qsptr = scull_follow(scd, item);

	if (! qsptr || ! qsptr->data || ! qsptr->data[s_pos])
		goto out;

	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buf, qsptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

out:
	up(&scd->sem);
	return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf,
	size_t count, loff_t *f_pos)
{
	struct scull_cdev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;

	*f_pos = dev->general_offset;
	ssize_t retval = -ENOMEM;

	if (count >= itemsize << 1)
		goto out;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow_for_write(dev, item);

	if (!dptr) {
		goto out;
	}

	if (!dptr->data) {
		dptr->data = (void **)kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (! dptr->data)
			goto out;
		memset(dptr->data, 0, qset * sizeof(void *));
	}

	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = (void *)kmalloc(quantum, GFP_KERNEL);
		if (! dptr->data[s_pos])
			goto out;
		memset(dptr->data[s_pos], 0, quantum);
	}

	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}

	retval = count;
	dev->general_offset += count;

	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	up(&dev->sem);
	return retval;
}

static struct scull_qset* scull_follow(struct scull_cdev *cdev, int item)
{
	struct scull_qset *ret = cdev->data;
	while(item-- && ret)
		ret = ret->next;
	return ret;
}

static struct scull_qset* scull_follow_for_write
(struct scull_cdev *cdev, int item)
{
	struct scull_qset *ret = cdev->data;
	if (!ret) {
		ret = cdev->data = kmalloc(sizeof(struct scull_qset),
						GFP_KERNEL);
		if (ret) {
			pr_info("scull: im here\n");
			memset(ret, 0, sizeof(struct scull_qset));
		}
	}

	if (ret) {
		while (item--) {
			if (!ret->next) {
				ret->next = kmalloc(sizeof(struct scull_qset),
							GFP_KERNEL);
				if (!ret->next)
					item = 0;
			}
			ret = ret->next;
		}
	}

	return ret;
}

static loff_t scull_llseek(struct file *f, loff_t off, int whence)
{
	struct scull_cdev *dev = f->private_data;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = off;
		break;
	case SEEK_CUR:
		newpos = f->f_pos + off;
		break;
	case SEEK_END:
		newpos = dev->size + off;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;
	f->f_pos = newpos;
	return newpos;
}

/* deprecated ? use seq_file
static const struct proc_ops read_proc(char *page, char **start, off_t offset, int count,
		int *eof, void *data)
{
	struct scull_dev *sd = &d;
	struct scull_qset *sq = sd->data;
	int j, len = 0;
	int limit = count - 80;

	len += sprintf(page + len, "\nscull: qset %i, q %i, sz %i\n",
			sd->qset, sd->quantum, sd->size);

	for (; sq && len <= limit; sq = sq->next) {
		len += sprintf(page + len, "   item at %p, qset at %p\n",
				sq, sq->data);
		if (sq->data && !sq->next)
			for (j = 0; j < sd->qset; j++) {
				if (sq->data[j])
					len += sprintf(page + len, "    % 4i: %8p\n",
							j, sq->data[j]);
			}
	}
	*eof = 1;
	return len;

}
static ssize_t proc_read(struct file *fl, char __user *ub, size_t ss, loff_t *t)
{
	if (*t == 5)
		return 0;
	pr_info("asd: %lu\n", ss);
	copy_to_user(ub, "asdf\n", 5);
	*t = 5;
	return 5;
}

static struct proc_ops po = {
	.proc_read = proc_read
};
*/

/* seq_file
static void* scull_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos >= scull_nr_devs)
		return NULL;
	return scull_devices + *pos;
}

static void* scull_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos >= scull_nr_devs)
		return NULL;
	return scull_devices + *pos;
}
*/

static int scull_init(void)
{
	int rd = reg_dev();
	if (rd < 0)
		return rd;
	reg_cdev();
	/* seq_file instead proc_create("driver/scullmem", 0, NULL, &po); */
	printk(KERN_DEBUG "scull: i'm here: %s:%i\n", __FILE__, __LINE__);
	PDEBUG("scull: PDEBUG macro test %s\n", "kek");
	pr_info("scull: started");
	return 0;
}

static void scull_exit(void)
{
	for (int i = 0; i < 4; i++)
		cdev_del(&d.node[i].cdev);
	unregister_chrdev_region(d.dev, scull_nr_devs);
	scull_trim();
	remove_proc_entry("driver/scullmem", NULL);
	printk(KERN_NOTICE "scull: closed\n");

}

module_init(scull_init);
module_exit(scull_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ME");
MODULE_DESCRIPTION("SCULL");
MODULE_VERSION("0.0.1");
