/**
    Network Driver for Beckhoff CCAT communication controller
    Copyright (C) 2014  Beckhoff Automation GmbH
    Author: Patrick Bruenn <p.bruenn@beckhoff.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include "module.h"

#define CCAT_DEVICES_MAX 5
#define CCAT_DATA_IN_4 0x038
#define CCAT_DATA_IN_N 0x7F0
#define CCAT_DATA_OUT_4 0x030
#define CCAT_DATA_BLOCK_SIZE (size_t)((CCAT_DATA_IN_N - CCAT_DATA_IN_4)/8)
#define CCAT_WRITE_BLOCK_SIZE 128
#define CCAT_FLASH_SIZE (size_t)0xE0000

/**     FUNCTION_NAME            CMD,  CLOCKS          */
#define CCAT_BULK_ERASE          0xE3, 8
#define CCAT_GET_PROM_ID         0xD5, 40
#define CCAT_READ_FLASH          0xC0, 32
#define CCAT_READ_STATUS         0xA0, 16
#define CCAT_WRITE_ENABLE        0x60, 8
#define CCAT_WRITE_FLASH         0x40, 32

/* from http://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith32Bits */
#define SWAP_BITS(B) \
	((((B) * 0x0802LU & 0x22110LU) | ((B) * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16)

static dev_t update_basedev;
static struct class *update_class;

/**
 * struct ccat_update - CCAT Update function (update)
 * @in_use: reference counter
 * @ioaddr: PCI base address of the CCAT Update function
 * dev: device number for this update function
 * cdev: character device used for the CCAT Update function
 * class: pointer to a device class used when registering the CCAT Update device
 */
struct ccat_update {
	atomic_t in_use;
	void __iomem *ioaddr;
	dev_t dev;
	struct cdev cdev;
};

static struct ccat_update dev_table[CCAT_DEVICES_MAX];

/**
 * struct update_buffer - keep track of a CCAT FPGA update
 * @update: pointer to a valid ccat_update object
 * @data: buffer used for write operations
 * @size: number of bytes written to the data buffer, if 0 on ccat_update_release() no data will be written to FPGA
 */
struct update_buffer {
	struct ccat_update *update;
	char data[CCAT_FLASH_SIZE];
	size_t size;
};

/**
 * wait_until_busy_reset() - wait until the busy flag was reset
 * @ioaddr: address of the CCAT Update function in PCI config space
 */
static inline void wait_until_busy_reset(void __iomem * const ioaddr)
{
	wmb();
	while (ioread8(ioaddr + 1)) {
		schedule();
	}
}

/**
 * __ccat_update_cmd() - Helper to issue a FPGA flash command
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @cmd: the command identifier
 * @clocks: the number of clocks associated with the specified command
 *
 * no write memory barrier is called and the busy flag is not evaluated
 */
static inline void __ccat_update_cmd(void __iomem * const ioaddr, u8 cmd,
				     u16 clocks)
{
	iowrite8((0xff00 & clocks) >> 8, ioaddr);
	iowrite8(0x00ff & clocks, ioaddr + 0x8);
	iowrite8(cmd, ioaddr + 0x10);
}

/**
 * ccat_update_cmd() - Helper to issue a FPGA flash command
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @cmd: the command identifier
 * @clocks: the number of clocks associated with the specified command
 *
 * Triggers a full flash command cycle with write memory barrier and
 * command activate. This call blocks until the busy flag is reset.
 */
static inline void ccat_update_cmd(void __iomem * const ioaddr, u8 cmd,
				   u16 clocks)
{
	__ccat_update_cmd(ioaddr, cmd, clocks);
	wmb();
	iowrite8(0xff, ioaddr + 0x7f8);
	wait_until_busy_reset(ioaddr);
}

/**
 * ccat_update_cmd_addr() - Helper to issue a FPGA flash command with address parameter
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @cmd: the command identifier
 * @clocks: the number of clocks associated with the specified command
 * @addr: 24 bit address associated with the specified command
 *
 * Triggers a full flash command cycle with write memory barrier and
 * command activate. This call blocks until the busy flag is reset.
 */
static inline void ccat_update_cmd_addr(void __iomem * const ioaddr,
					u8 cmd, u16 clocks, u32 addr)
{
	const u8 addr_0 = SWAP_BITS(addr & 0xff);
	const u8 addr_1 = SWAP_BITS((addr & 0xff00) >> 8);
	const u8 addr_2 = SWAP_BITS((addr & 0xff0000) >> 16);

	__ccat_update_cmd(ioaddr, cmd, clocks);
	iowrite8(addr_2, ioaddr + 0x18);
	iowrite8(addr_1, ioaddr + 0x20);
	iowrite8(addr_0, ioaddr + 0x28);
	wmb();
	iowrite8(0xff, ioaddr + 0x7f8);
	wait_until_busy_reset(ioaddr);
}

/**
 * ccat_get_status() - Read CCAT Update status
 * @ioaddr: address of the CCAT Update function in PCI config space
 *
 * Return: the current status of the CCAT Update function
 */
static u8 ccat_get_status(void __iomem * const ioaddr)
{
	ccat_update_cmd(ioaddr, CCAT_READ_STATUS);
	return ioread8(ioaddr + 0x20);
}

/**
 * ccat_read_flash_block() - Read a block of CCAT configuration data from flash
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @addr: 24 bit address of the block to read
 * @len: number of bytes to read from this block, len <= CCAT_DATA_BLOCK_SIZE
 * @buf: output buffer in user space
 *
 * Copies one block of configuration data from the CCAT FPGA's flash to
 * the user space buffer.
 * Note that the size of the FPGA's firmware is not known exactly so it
 * is very possible that the overall buffer ends with a lot of 0xff.
 *
 * Return: the number of bytes copied
 */
static int ccat_read_flash_block(void __iomem * const ioaddr,
				 const u32 addr, const u16 len,
				 char __user * const buf)
{
	u16 i;
	const u16 clocks = 8 * len;

	ccat_update_cmd_addr(ioaddr, CCAT_READ_FLASH + clocks, addr);
	for (i = 0; i < len; i++) {
		put_user(ioread8(ioaddr + CCAT_DATA_IN_4 + 8 * i), buf + i);
	}
	return len;
}

/**
 * ccat_read_flash() - Read a chunk of CCAT configuration data from flash
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @buf: output buffer in user space
 * @len: number of bytes to read
 * @off: offset in the configuration data
 *
 * Copies multiple blocks of configuration data from the CCAT FPGA's
 * flash to the user space buffer.
 *
 * Return: the number of bytes copied
 */
static int ccat_read_flash(void __iomem * const ioaddr, char __user * buf,
			   u32 len, loff_t * off)
{
	const loff_t start = *off;

	while (len > CCAT_DATA_BLOCK_SIZE) {
		*off +=
		    ccat_read_flash_block(ioaddr, *off, CCAT_DATA_BLOCK_SIZE,
					  buf);
		buf += CCAT_DATA_BLOCK_SIZE;
		len -= CCAT_DATA_BLOCK_SIZE;
	}
	*off += ccat_read_flash_block(ioaddr, *off, len, buf);
	return *off - start;
}

/**
 * ccat_wait_status_cleared() - wait until CCAT status is cleared
 * @ioaddr: address of the CCAT Update function in PCI config space
 *
 * Blocks until bit 7 of the CCAT Update status is reset
 */
static void ccat_wait_status_cleared(void __iomem * const ioaddr)
{
	u8 status;

	do {
		status = ccat_get_status(ioaddr);
	} while (status & (1 << 7));
}

/**
 * ccat_write_flash_block() - Write a block of CCAT configuration data to flash
 * @ioaddr: address of the CCAT Update function in PCI config space
 * @addr: 24 bit start address in the CCAT FPGA's flash
 * @len: number of bytes to write in this block, len <= CCAT_WRITE_BLOCK_SIZE
 * @buf: input buffer
 *
 * Copies one block of configuration data to the CCAT FPGA's flash
 *
 * Return: the number of bytes copied
 */
static int ccat_write_flash_block(void __iomem * const ioaddr,
				  const u32 addr, const u16 len,
				  const char *const buf)
{
	const u16 clocks = 8 * len;
	u16 i;

	ccat_update_cmd(ioaddr, CCAT_WRITE_ENABLE);
	for (i = 0; i < len; i++) {
		iowrite8(buf[i], ioaddr + CCAT_DATA_OUT_4 + 8 * i);
	}
	ccat_update_cmd_addr(ioaddr, CCAT_WRITE_FLASH + clocks, addr);
	ccat_wait_status_cleared(ioaddr);
	return len;
}

/**
 * ccat_write_flash() - Write a new CCAT configuration to FPGA's flash
 * @update: a CCAT Update buffer containing the new FPGA configuration
 */
static void ccat_write_flash(const struct update_buffer *const update)
{
	const char *buf = update->data;
	u32 off = 0;
	size_t len = update->size;

	while (len > CCAT_WRITE_BLOCK_SIZE) {
		ccat_write_flash_block(update->update->ioaddr, off,
				       (u16) CCAT_WRITE_BLOCK_SIZE, buf);
		off += CCAT_WRITE_BLOCK_SIZE;
		buf += CCAT_WRITE_BLOCK_SIZE;
		len -= CCAT_WRITE_BLOCK_SIZE;
	}
	ccat_write_flash_block(update->update->ioaddr, off, (u16) len, buf);
}

static int ccat_update_open(struct inode *const i, struct file *const f)
{
	struct ccat_update *update =
	    container_of(i->i_cdev, struct ccat_update, cdev);
	struct update_buffer *buf;

	if (!atomic_dec_and_test(&update->in_use)) {
		atomic_inc(&update->in_use);
		return -EBUSY;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		atomic_inc(&update->in_use);
		return -ENOMEM;
	}

	buf->update = update;
	f->private_data = buf;
	return 0;
}

static int ccat_update_release(struct inode *const i, struct file *const f)
{
	const struct update_buffer *const buf = f->private_data;
	struct ccat_update *const update = buf->update;

	if (buf->size > 0) {
		ccat_update_cmd(update->ioaddr, CCAT_WRITE_ENABLE);
		ccat_update_cmd(update->ioaddr, CCAT_BULK_ERASE);
		ccat_wait_status_cleared(update->ioaddr);
		ccat_write_flash(buf);
	}
	kfree(f->private_data);
	atomic_inc(&update->in_use);
	return 0;
}

/**
 * ccat_update_read() - Read CCAT configuration data from flash
 * @f: file handle previously initialized with ccat_update_open()
 * @buf: buffer in user space provided for our data
 * @len: length of the user space buffer
 * @off: current offset of our file operation
 *
 * Copies data from the CCAT FPGA's configuration flash to user space.
 * Note that the size of the FPGA's firmware is not known exactly so it
 * is very possible that the overall buffer ends with a lot of 0xff.
 *
 * Return: the number of bytes written, or 0 if EOF reached
 */
static ssize_t ccat_update_read(struct file *const f, char __user * buf,
				size_t len, loff_t * off)
{
	struct update_buffer *update = f->private_data;

	if (*off >= CCAT_FLASH_SIZE) {
		return 0;
	}
	if (*off + len >= CCAT_FLASH_SIZE) {
		len = CCAT_FLASH_SIZE - *off;
	}
	return ccat_read_flash(update->update->ioaddr, buf, len, off);
}

/**
 * ccat_update_write() - Write data to the CCAT FPGA's configuration flash
 * @f: file handle previously initialized with ccat_update_open()
 * @buf: buffer in user space providing the new configuration data (from *.rbf)
 * @len: length of the user space buffer
 * @off: current offset in the configuration data
 *
 * Copies data from user space (possibly a *.rbf) to the CCAT FPGA's
 * configuration flash.
 *
 * Return: the number of bytes written, or 0 if flash end is reached
 */
static ssize_t ccat_update_write(struct file *const f, const char __user * buf,
				 size_t len, loff_t * off)
{
	struct update_buffer *const update = f->private_data;

	if (*off + len > sizeof(update->data))
		return 0;

	if (copy_from_user(update->data + *off, buf, len)) {
		return -EFAULT;
	}

	*off += len;
	update->size = *off;
	return len;
}

static struct file_operations update_ops = {
	.owner = THIS_MODULE,
	.open = ccat_update_open,
	.release = ccat_update_release,
	.read = ccat_update_read,
	.write = ccat_update_write,
};

static void ccat_update_free(struct ccat_update *update)
{
	update->dev = 0;
}

static struct ccat_update *ccat_update_alloc(void)
{
	int i = 0;

	for (i = 0; i < CCAT_DEVICES_MAX; ++i) {
		if (dev_table[i].dev == 0) {
			dev_table[i].dev = MKDEV(MAJOR(update_basedev), i);
			return &dev_table[i];
		}
	}
	return NULL;
}

/**
 * ccat_update_init() - Initialize the CCAT Update function
 */
static int ccat_update_probe(struct ccat_function *func)
{
	struct ccat_update *const update = ccat_update_alloc();

	if (!update) {
		pr_warn("exceeding max. number of update devices (%d)\n",
			CCAT_DEVICES_MAX);
		return -ENOMEM;
	}

	update->ioaddr = func->ccat->bar_0.ioaddr + func->info.addr;
	atomic_set(&update->in_use, 1);

	if (0x00 != func->info.rev) {
		pr_warn("CCAT Update rev. %d not supported\n", func->info.rev);
		goto cleanup;
	}

	if (NULL ==
	    device_create(update_class, NULL, update->dev, NULL,
			  "ccat_update%d", MINOR(update->dev))) {
		pr_warn("device_create() failed\n");
		goto cleanup;
	}

	cdev_init(&update->cdev, &update_ops);
	update->cdev.owner = THIS_MODULE;
	update->cdev.ops = &update_ops;
	if (cdev_add(&update->cdev, update->dev, 1)) {
		pr_warn("add update device failed\n");
		goto cleanup_device;
	}

	pr_info("registered %s%d.\n", update_class->name, MINOR(update->dev));
	func->private_data = update;
	return 0;
cleanup_device:
	device_destroy(update_class, update->dev);
cleanup:
	ccat_update_free(update);
	return -1;
}

/**
 * ccat_update_remove() - Prepare the CCAT Update function for removal
 */
static void ccat_update_remove(struct ccat_function *func)
{
	struct ccat_update *const update = func->private_data;

	cdev_del(&update->cdev);
	device_destroy(update_class, update->dev);
	ccat_update_free(update);
}

int __init ccat_update_init(void)
{
	if (alloc_chrdev_region
	    (&update_basedev, 0, CCAT_DEVICES_MAX, KBUILD_MODNAME)) {
		pr_warn("alloc_chrdev_region() failed\n");
		return -1;
	}

	update_class = class_create(THIS_MODULE, "ccat_update");
	if (NULL == update_class) {
		pr_warn("Create device class failed\n");
		unregister_chrdev_region(update_basedev, CCAT_DEVICES_MAX);
		return -1;
	}
	return 0;
}

void __exit ccat_update_exit(void)
{
	class_destroy(update_class);
	unregister_chrdev_region(update_basedev, CCAT_DEVICES_MAX);
}

struct ccat_driver update_driver = {
	.type = CCATINFO_EPCS_PROM,
	.probe = ccat_update_probe,
	.remove = ccat_update_remove,
};
