#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/blkpg.h>
#include <linux/namei.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kurosh Kuchekali");

char* device = "/dev/sdb";
module_param(device, charp, S_IRUGO);

static struct file *usb_file = NULL;
struct block_device *bdevice = NULL;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static bool open_usb(void) {
	usb_file = bdev_file_open_by_path(device, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(usb_file)) {
		pr_err("Failed to open file\n");
		usb_file = NULL;
		return false;
	}

	bdevice = file_bdev(usb_file);
	if (!bdevice) {
		pr_err("Failed to get block\n");
		filp_close(usb_file, NULL);
		usb_file = NULL;
		return false;
	}

	return true;
}

static void close_usb(void) {
	if (usb_file) {
		filp_close(usb_file, NULL);
		usb_file = NULL;
	}

	bdevice = NULL;
}

static int __init kmod_init(void) {
	if (!open_usb()) {
		pr_err("Failed to open USB block device\n");
		return -ENODEV;
	}
	kmod_ioctl_init();
	return 0;
}

static void __exit kmod_exit(void) {
	close_usb();
	kmod_ioctl_teardown();
}

module_init(kmod_init);
module_exit(kmod_exit);
