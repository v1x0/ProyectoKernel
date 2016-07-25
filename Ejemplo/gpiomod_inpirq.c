/*
 * Basic Linux Kernel module using GPIO interrupts.
 *
 * Author:
 * 	Interrupt handling part - Stefan Wendler (devnull@kaltpost.de)
 *  Device part :
 *     Copyright (C) 2013, Jack Whitham
 *     Copyright (C) 2009-2010, University of York
 *     Copyright (C) 2004-2006, Advanced Micro Devices, Inc.
 *
 *  Modified by Disk91 (www.disk91.com) for RFRPI Shield
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define GPIO_FOR_RX_SIGNAL	18
#define DEV_NAME 			"rfrpi" 
#define BUFFER_SZ			512 

/* Last Interrupt timestamp */
static struct timespec lastIrq_time;
static unsigned long lastDelta[BUFFER_SZ];
static int  pRead;
static int  pWrite;
static int  wasOverflow;


/* Define GPIOs for RX signal */
static struct gpio signals[] = {
		{ GPIO_FOR_RX_SIGNAL, GPIOF_IN, "RX Signal" },	// Rx signal
};

/* Later on, the assigned IRQ numbers for the buttons are stored here */
static int rx_irqs[] = { -1 };

/*
 * The interrupt service routine called on every pin status change
 */
static irqreturn_t rx_isr(int irq, void *data)
{
   	struct timespec current_time;
    struct timespec delta;
   	unsigned long ns;

   	getnstimeofday(&current_time);
	delta = timespec_sub(current_time, lastIrq_time);
	ns = ((long long)delta.tv_sec * 1000000)+(delta.tv_nsec/1000); 
	lastDelta[pWrite] = ns;
   	getnstimeofday(&lastIrq_time);

	pWrite = ( pWrite + 1 )  & (BUFFER_SZ-1);
	if (pWrite == pRead) {
		// overflow
		pRead = ( pRead + 1 ) & (BUFFER_SZ-1);
		if ( wasOverflow == 0 ) {
	       printk(KERN_ERR "RFRPI - Buffer Overflow - IRQ will be missed");
	       wasOverflow = 1;
	    }
	} else {
		wasOverflow = 0;
	}
	return IRQ_HANDLED;
}


static int rx433_open(struct inode *inode, struct file *file)
{
    return nonseekable_open(inode, file);
}

static int rx433_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t rx433_write(struct file *file, const char __user *buf,
                size_t count, loff_t *pos)
{
	return -EINVAL;
}

static ssize_t rx433_read(struct file *file, char __user *buf,
                size_t count, loff_t *pos)
{
	// returns one of the line with the time between two IRQs
	// return 0 : end of reading
	// return >0 : size
	// return -EFAULT : error
	char tmp[256];
	int _count;
	int _error_count;

	_count = 0;
	if ( pRead != pWrite ) {
		sprintf(tmp,"%ld\n",lastDelta[pRead]);
  	    _count = strlen(tmp);
        _error_count = copy_to_user(buf,tmp,_count+1);
        if ( _error_count != 0 ) {
        	printk(KERN_ERR "RFRPI - Error writing to char device");
            return -EFAULT;
        }
		pRead = (pRead + 1) & (BUFFER_SZ-1);
	}
	return _count;
}

static struct file_operations rx433_fops = {
    .owner = THIS_MODULE,
    .open = rx433_open,
    .read = rx433_read,
    .write = rx433_write,
    .release = rx433_release,
};

static struct miscdevice rx433_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEV_NAME,
    .fops = &rx433_fops,
};



/*
 * Module init function
 */
static int __init rfrpi_init(void)
{
	int ret = 0;
	printk(KERN_INFO "%s\n", __func__);

	// INITIALIZE IRQ TIME AND Queue Management
	getnstimeofday(&lastIrq_time);
	pRead = 0;
	pWrite = 0;
	wasOverflow = 0;

	// register GPIO PIN in use
	ret = gpio_request_array(signals, ARRAY_SIZE(signals));

	if (ret) {
		printk(KERN_ERR "RFRPI - Unable to request GPIOs for RX Signals: %d\n", ret);
		goto fail2;
	}
	
	// Register IRQ for this GPIO
	ret = gpio_to_irq(signals[0].gpio);
	if(ret < 0) {
		printk(KERN_ERR "RFRPI - Unable to request IRQ: %d\n", ret);
		goto fail2;
	}
	rx_irqs[0] = ret;
	printk(KERN_INFO "RFRPI - Successfully requested RX IRQ # %d\n", rx_irqs[0]);
	ret = request_irq(rx_irqs[0], rx_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING ,"rfpi_init", NULL);
	if(ret) {
		printk(KERN_ERR "RFRPI - Unable to request IRQ: %d\n", ret);
		goto fail3;
	}

	// Register a character device for communication with user space
    misc_register(&rx433_misc_device);

	return 0;

	// cleanup what has been setup so far
fail3:
	free_irq(rx_irqs[0], NULL);

fail2: 
	gpio_free_array(signals, ARRAY_SIZE(signals));
	return ret;	
}

/**
 * Module exit function
 */
static void __exit rfrpi_exit(void)
{
	printk(KERN_INFO "%s\n", __func__);

    misc_deregister(&rx433_misc_device);

	// free irqs
	free_irq(rx_irqs[0], NULL);	
	
	// unregister
	gpio_free_array(signals, ARRAY_SIZE(signals));
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Disk91");
MODULE_DESCRIPTION("Linux Kernel Module for rfrpi shield");

module_init(rfrpi_init);
module_exit(rfrpi_exit);
