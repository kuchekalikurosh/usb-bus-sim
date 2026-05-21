#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/freezer.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <linux/cdev.h>
#include <linux/nospec.h>
#include <linux/vmalloc.h>
#include "../ioctl-defines.h"

static dev_t dev = 0;
static struct class* kmod_class;
static struct cdev kmod_cdev;
// we need to keep track of the current offset per write/read
// so the rw_usb knows where to write/read
static unsigned int current_offset = 0;
// provided in ioctl_defines
struct block_rw_ops rw_request;
struct block_rwoffset_ops rwoffset_request;

// the actual initalization was done in main
// so we use extern struct to use the existing variable from main
extern struct block_device* bdevice;

// prototypes
bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

// actual write read function
long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag) {
	// track how much data was processed
	unsigned int processed = 0;
	// return variable for bio
	int ret = 0;
	
	// while processed is smaller than the size of data passed in
	while (processed < size) {
		// bio and page initalization
		struct bio *bio;
		struct page *page;
		// break up the data in writable chunks of 512
		unsigned int write_chunk = min(size - processed, 512);
		unsigned int page_offset;
		// since bio uses sectors to for offsets,
		// we will calculate the correct offset using 
		// the amount of data processed with the current offset / 512
		sector_t sector;
		
		// depending on the flag, set the bio to either write or read mode
		if (flag) {
			bio = bio_alloc(bdevice, 1, REQ_OP_WRITE, GFP_NOIO);
		} else {
			bio = bio_alloc(bdevice, 1, REQ_OP_READ, GFP_NOIO);
		}
		
		// calculate the sector for the bio in order to line it up with the page
		sector = (offset / 512) + (processed / 512);
		// calculate the page and offset
		// since our buffer was allocated using vmalloc,
		// we'll translate that to a page
		page = vmalloc_to_page(data + processed);
		page_offset = offset_in_page(data + processed);

		// set the sector
		bio->bi_iter.bi_sector = sector;
		
		// write/read
		ret = bio_add_page(bio, page, write_chunk, page_offset);

		// debug
		// printk("bio_add_page sucsess. chunk=%u, offset=%u", write_chunk, page_offset);
		
		// submit and wait
		ret = submit_bio_wait(bio);
		bio_put(bio);
		
		// if the submission failed (value -1) than throw an error
		if (ret < 0) {
			printk("submit failed\n");
			return ret;
		}
		
		// otherwise increment and continue
		processed += write_chunk;
	}
	return 0;
}

// init for the io controller
static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
	char* kernbuf;
	
	// switch structure based off the skeleton
	switch (cmd) {
		case BREAD:
		case BWRITE:
			if (copy_from_user((void*) &rw_request, (void*) arg, sizeof(struct block_rw_ops))) {
				printk("Error: Incorrect Params\n");
				return -1;
			}
			
			kernbuf = (char*) vmalloc(rw_request.size);

			if (cmd == BWRITE) {
				if (copy_from_user(kernbuf, rw_request.data, rw_request.size)) {
					vfree(kernbuf);
					printk("Error: copy fault");
					return -1;
				}

				// IMPORTANT:
				// remember to increment the offset with the size!!
				// that way the rw_usb knowns where to do operations!
				rw_usb(kernbuf, rw_request.size, current_offset, true);
				current_offset += rw_request.size;
			} else {
				rw_usb(kernbuf, rw_request.size, current_offset, false);
				current_offset += rw_request.size;
				if (copy_to_user(rw_request.data, kernbuf, rw_request.size)) {
					vfree(kernbuf);
					printk("Error: copy fault");
					return -1;
				}
			}
			break;

		case BREADOFFSET:
		case BWRITEOFFSET:
			// same thing as regular read/write except with a predefined offset
			if (copy_from_user((void*) &rwoffset_request, (void*) arg, sizeof(struct block_rwoffset_ops))) {
				printk("Error: Incorrect Params\n");
				return -1;
			}

			kernbuf = (char*) vmalloc(rwoffset_request.size);

			if (cmd == BWRITEOFFSET) {
				if (copy_from_user(kernbuf, rwoffset_request.data, rwoffset_request.size)) {
					vfree(kernbuf);
					printk("OFFSET: copy fault\n");
					return -1;
				}
				rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, true);
				// current_offset += rwoffset_request.size;
			} else {
				rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, false);
				// current_offset += rwoffset_request.size;
				if (copy_to_user(rwoffset_request.data, kernbuf, rwoffset_request.size)) {
					vfree(kernbuf);
					printk("OFFSET: copy fault\n");
					return -1;
				}
			}

			break;
		default:
			printk("error\n");
			return -1;
	}

	vfree(kernbuf);
	return 0;
}

static int kmod_open(struct inode *inode, struct file *file) {
	printk(KERN_INFO "kmod: open\n");
	return 0;
}

static int kmod_release(struct inode *inode, struct file *file) {
	printk(KERN_INFO "kmod: released\n");
	return 0;
}

static struct file_operations fops = 
{
	.owner = THIS_MODULE,
	.open = kmod_open,
	.release = kmod_release,
	.unlocked_ioctl = kmod_ioctl
};

bool kmod_ioctl_init(void) {
	if (alloc_chrdev_region(&dev, 0, 1, "usbaccess") < 0) {
		printk("Error: could not allocate usbaccess\n");
		return false;
	}

	cdev_init(&kmod_cdev, &fops);
	if (cdev_add(&kmod_cdev, dev, 1) < 0) {
		printk("Error: kmod_cdev failed\n");
		goto cdevfailed;
	}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(6,2,16)
	if ((kmod_class = class_create(THIS_MODULE, "kmod_class")) == NULL) {
		printk("Could not create kmodclass\n");
		goto cdevfailed;
	}
#else
	if ((kmod_class = class_create("kmod_class")) == NULL) {
		printk("Could not create kmodclass\n");
		goto cdevfailed;
	}
#endif
	if ((device_create(kmod_class, NULL, dev, NULL, "kmod")) == NULL) {
		printk("Could not make device\n");
		goto classfailed;
	}

	printk("Initalization Complete\n");
	return true;

classfailed:
	class_destroy(kmod_class);
cdevfailed:
	unregister_chrdev_region(dev, 1);
	return false;
}

void kmod_ioctl_teardown(void) {
	if (kmod_class) {
		device_destroy(kmod_class, dev);
		class_destroy(kmod_class);
	}
	cdev_del(&kmod_cdev);
	unregister_chrdev_region(dev, 1);

	printk("Teardown Complete\n");
}

