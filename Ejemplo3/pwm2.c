/* dafruit PWM driver for Raspberry Pi
 *
 * Provides a sysfs interface to the PWM port present on pin 18 of the
 * Raspberry Pi expansion header.  Allows for driving a servo, or variable
 * frequency waveforms.
 *
 * It tends to have problems locking on to frequencies above 100 kHz, and
 * with indivisible duty cycles.
 *
 * - Written by Sean Cross for Adafruit Industries (www.adafruit.com)
 */

#define RPI_PWM_VERSION "1.0"
#define PWM_CLASS_NAME "rpi-pwm"

#include <linux/module.h>

#include <asm/io.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>
#include <linux/sysfs.h>

#define BCM2708_PERI_BASE	0x20000000
#define GPIO_BASE		(BCM2708_PERI_BASE + 0x200000)
#define PWM_BASE		(BCM2708_PERI_BASE + 0x20C000)
#define CLOCK_BASE		(BCM2708_PERI_BASE + 0x101000)

#define GPIO_REG(g) (gpio_reg+((g/10)*4))
#define SET_GPIO_ALT(g,a) \
	__raw_writel( 							\
		(((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))		\
		| (__raw_readl(GPIO_REG(g)) & (~(7<<(((g)%10)*3)))),	\
		GPIO_REG(g))

#define	PWM_CTL  (pwm_reg+(0*4))
#define	PWM_RNG1 (pwm_reg+(4*4))
#define	PWM_DAT1 (pwm_reg+(5*4))

#define	PWMCLK_CNTL (clk_reg+(40*4))
#define	PWMCLK_DIV  (clk_reg+(41*4))

#define strict_strtol   kstrtol

static DEFINE_MUTEX(sysfs_lock);
static void __iomem *pwm_reg;
static void __iomem *gpio_reg;
static void __iomem *clk_reg;

enum device_mode {
	MODE_PWM,
	MODE_SERVO,
	MODE_AUDIO,
};

static char *device_mode_str[] = {
	"pwm",
	"servo",
	"audio",
};

struct rpi_pwm {
	u32 duty;
	u32 frequency;
	u32 servo_val;
	u32 servo_max;
	int active:1;
	int immediate:1;
	int loaded:1;
	int id;
	enum device_mode mode;	/* Servo, PWM, or Audio */
	struct device *dev;

	u32 divisor;
	u32 mcf; /* Maximum common frequency (desired) */
	u32 real_mcf;
};


static struct rpi_pwm pwms[] = {
	{
		.immediate	= 1,
		.duty		= 100,
		.servo_max	= 32,
		.mcf		= 16000,  /* 16 kHz is a good common number */
	},
};


/* Sets the system timer to have the new divisor */
static int rpi_pwm_set_clk(struct rpi_pwm *dev, u32 mcf) {
	/* Stop clock and waiting for busy flag doesn't work, so kill clock */
	__raw_writel(0x5A000000 | (1 << 5), PWMCLK_CNTL);
	udelay(10);  

	if (!dev->mcf) {
		dev_err(dev->dev, "no MCF specified\n");
		return -EINVAL;
	}

	/* Set frequency
	 * DIVI is the integer part of the divisor
	 * the fractional part (DIVF) drops clock cycles to get the
	 * output frequency, bad for servo motors
	 * 320 bits for one cycle of 20 milliseconds = 62.5 us per bit = 16 kHz
	 */
	dev->divisor = 19200000 / mcf;
	if (dev->divisor < 1 || dev->divisor > 0x1000) {
		dev_err(dev->dev, "divisor out of range: %x\n", dev->divisor);
		return -ERANGE;
	}
	__raw_writel(0x5A000000 | (dev->divisor<<12), PWMCLK_DIV);
	
	/* Enable the PWM clock */
	__raw_writel(0x5A000011, PWMCLK_CNTL);

	/* Calculate the real maximum common frequency */
	dev->real_mcf = 19200000 / dev->divisor;

	return 0;
}


static int rpi_pwm_set_servo(struct rpi_pwm *dev) {
	unsigned long RNG, DAT;
	unsigned long mcf = 16000, frequency=50;
	int ret;

	/* Disable PWM */
	__raw_writel(0, PWM_CTL);

	/* Wait for the PWM to be disabled, otherwise PWM block hangs */
	udelay(10);

	ret = rpi_pwm_set_clk(dev, mcf);
	if (ret)
		return ret;

	RNG = mcf/frequency;
	DAT = (mcf*2*dev->servo_val/dev->servo_max/frequency/20)
	    + (mcf/frequency/40);

	if (RNG < 1) {
		dev_err(dev->dev, "RNG is out of range: %ld<1\n", RNG);
		return -ERANGE;
	}

	if (DAT < 1) {
		dev_err(dev->dev, "DAT is out of range: %ld<1\n", DAT);
		return -ERANGE;
	}

	__raw_writel(RNG, PWM_RNG1);
	__raw_writel(DAT, PWM_DAT1);

	/* Enable MSEN mode, and start PWM */
	__raw_writel(0x81, PWM_CTL);

	return 0;
}


static int rpi_pwm_set_frequency(struct rpi_pwm *dev) {
	unsigned long RNG, DAT;
	int ret;
	/* Disable PWM */
	__raw_writel(0, PWM_CTL);

	/* Wait for the PWM to be disabled, otherwise PWM block hangs */
	udelay(10);

	ret = rpi_pwm_set_clk(dev, dev->mcf);
	if (ret)
		return ret;

	RNG = dev->mcf/dev->frequency;
	DAT = RNG*dev->duty/100;

	if (RNG < 1) {
		dev_err(dev->dev, "RNG is out of range: %ld<1\n", RNG);
		return -ERANGE;
	}

	if (DAT < 1) {
		dev_err(dev->dev, "DAT is out of range: %ld<1\n", DAT);
		return -ERANGE;
	}

	__raw_writel(RNG, PWM_RNG1);
	__raw_writel(DAT, PWM_DAT1);

	/* Enable MSEN mode, and start PWM */
	__raw_writel(0x81, PWM_CTL);

	return 0;
}


static int rpi_pwm_activate(struct rpi_pwm *dev) {
	int ret = 0;

	/* Set PWM alternate function for GPIO18 */
	SET_GPIO_ALT(15, 5);

	if (dev->mode == MODE_SERVO)
		ret = rpi_pwm_set_servo(dev);

	else if (dev->mode == MODE_PWM)
		ret = rpi_pwm_set_frequency(dev);

	else if (dev->mode == MODE_AUDIO) {
		/* Nothing to do */
		;
	}

	dev->active = 1;
	return ret;
}


static int rpi_pwm_deactivate(struct rpi_pwm *dev) {
	if (dev->mode != MODE_AUDIO)
		__raw_writel(0, PWM_CTL);
	udelay(10);
	SET_GPIO_ALT(15, 0);
	udelay(10);
	dev->active = 0;
	return 0;
}



static ssize_t active_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d\n", !!dev->active);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t active_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_active;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_active);
	if (ret == 0) {
		if (new_active)
			ret = rpi_pwm_activate(dev);
		else
			ret = rpi_pwm_deactivate(dev);
	}
	else
		ret = -EINVAL;
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(active, 0666, active_show, active_store);
static DEVICE_ATTR(active, 0664, active_show, active_store);



static ssize_t mode_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	char tmp_bfr[512];
	char *tmp_bfr_ptr = tmp_bfr;
	int offset;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	for (offset=0; offset < ARRAY_SIZE(device_mode_str); offset++) {
		if (dev->mode == offset)
			*tmp_bfr_ptr++ = '[';
		strcpy(tmp_bfr_ptr, device_mode_str[offset]);
		tmp_bfr_ptr += strlen(device_mode_str[offset]);
		if (dev->mode == offset)
			*tmp_bfr_ptr++ = ']';
		*tmp_bfr_ptr++ = ' ';
	}
	*tmp_bfr_ptr++ = 0;
	ret = sprintf(buf, "%s\n", tmp_bfr);
	mutex_unlock(&sysfs_lock);
	return ret;
}


static ssize_t mode_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	int offset;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = -ENOENT;
	for (offset=0; offset < ARRAY_SIZE(device_mode_str); offset++) {
		if (!strncmp(buf,
				 device_mode_str[offset],
				 strlen(device_mode_str[offset]))) {
			dev->mode = offset;

			if (dev->immediate)
				rpi_pwm_activate(dev);

			/* If switching to audio mode, switch out of
			 * immediate mode.  This protects us from locking
			 * up the audio system by altering PWM values while
			 * audio playback is occurring.
			 */
			if (offset == MODE_AUDIO)
				dev->immediate = 0;

			ret = 0;
			break;
		}
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(mode, 0666, mode_show, mode_store);
static DEVICE_ATTR(mode, 0664, mode_show, mode_store);


static ssize_t duty_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d%%\n", dev->duty);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t duty_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_duty;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_duty);
	if (ret == 0) {
		if (new_duty > 0 && new_duty < 100) {
			dev->duty = new_duty;
			dev->mode = MODE_PWM;
			if (dev->immediate)
				rpi_pwm_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(duty, 0666, duty_show, duty_store);
static DEVICE_ATTR(duty, 0664, duty_show, duty_store);



static ssize_t mcf_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d\n", dev->mcf);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t mcf_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_mcf;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_mcf);
	if (ret == 0) {
		if (new_mcf > 1 && new_mcf < 100000000) {
			dev->mcf = new_mcf;
			dev->mode = MODE_PWM;
			if (dev->immediate)
				rpi_pwm_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
static DEVICE_ATTR(mcf, 0664, mcf_show, mcf_store);
//static DEVICE_ATTR(mcf, 0666, mcf_show, mcf_store);



static ssize_t real_freq_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	unsigned long RNG;
	mutex_lock(&sysfs_lock);
	if (dev->frequency) {
		RNG = dev->mcf/dev->frequency;
		if (RNG < 1)
			ret = -EINVAL;
		else
			ret = sprintf(buf, "%ld\n", dev->real_mcf/RNG);
	}
	else
		ret = -EINVAL;
	mutex_unlock(&sysfs_lock);
	return ret;
}
//static DEVICE_ATTR(real_frequency, 0666, real_freq_show, NULL);
static DEVICE_ATTR(real_frequency, 0664, real_freq_show, NULL);


static ssize_t servo_val_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d\n", dev->servo_val);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t servo_val_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_servo_val;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_servo_val);
	if (ret == 0) {
		if (new_servo_val >= 0 && new_servo_val <= dev->servo_max) {
			dev->servo_val = new_servo_val;
			dev->mode = MODE_SERVO;
			if (dev->immediate)
				rpi_pwm_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(servo, 0666, servo_val_show, servo_val_store);
static DEVICE_ATTR(servo, 0664, servo_val_show, servo_val_store);


static ssize_t servo_max_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d\n", dev->servo_max);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t servo_max_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_servo_max;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_servo_max);
	if (ret == 0) {
		if (new_servo_max > 0) {
			/* Scale the rotation to match new max */
			dev->servo_val = dev->servo_val
				       *new_servo_max / dev->servo_max;

			dev->servo_max = new_servo_max;
			dev->mode = MODE_SERVO;
			if (dev->immediate)
				rpi_pwm_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(servo_max, 0666, servo_max_show, servo_max_store);
static DEVICE_ATTR(servo_max, 0664, servo_max_show, servo_max_store);


static ssize_t freq_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, "%d\n", dev->frequency);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t freq_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_freq;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_freq);
	if (ret == 0) {
		dev->frequency = new_freq;
		dev->mode = MODE_PWM;
		if (dev->immediate)
			rpi_pwm_activate(dev);
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(frequency, 0666, freq_show, freq_store);
static DEVICE_ATTR(frequency, 0664, freq_show, freq_store);



static ssize_t delayed_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct rpi_pwm *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	ret = sprintf(buf, dev->immediate?"immediate\n":"delayed\n");
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t delayed_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	struct rpi_pwm *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	if (!strcasecmp(buf, "immediate") || buf[0] == '0')
		dev->immediate = 1;
	else if (!strcasecmp(buf, "delayed") || buf[0] == '1')
		dev->immediate = 0;
	else
		ret = -EINVAL;
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}
//static DEVICE_ATTR(delayed, 0666, delayed_show, delayed_store);
static DEVICE_ATTR(delayed, 0664, delayed_show, delayed_store);



static struct attribute *rpi_pwm_sysfs_entries[] = {
	&dev_attr_active.attr,
	&dev_attr_delayed.attr,
	&dev_attr_servo.attr,
	&dev_attr_servo_max.attr,
	&dev_attr_duty.attr,
	&dev_attr_mode.attr,
	&dev_attr_mcf.attr,
	&dev_attr_real_frequency.attr,
	&dev_attr_frequency.attr,
	NULL
};

static struct attribute_group rpi_pwm_attribute_group = {
	.name = NULL,
	.attrs = rpi_pwm_sysfs_entries,
};

static struct class pwm_class = {
	.name =		PWM_CLASS_NAME,
	.owner =	THIS_MODULE,
};


int __init rpi_pwm_init(void)
{
	int ret = 0;
	int pwm = 0;

	pr_info("Adafruit Industries' Raspberry Pi PWM driver v%s\n", RPI_PWM_VERSION);

	ret = class_register(&pwm_class);
	if (ret < 0) {
		pr_err("%s: Unable to register class\n", pwm_class.name);
		goto out0;
	}


	/* Create devices for each PWM present */
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		pwms[pwm].id = pwm;
		pwms[pwm].dev = device_create(&pwm_class, &platform_bus,
				MKDEV(0, 0), &pwms[pwm], "pwm%u", pwm);
		if (IS_ERR(pwms[pwm].dev)) {
			pr_err("%s: device_create failed\n", pwm_class.name);
			ret = PTR_ERR(pwms[pwm].dev);
			goto out1;
		}
	}

	/* Create sysfs entries for each PWM */
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		ret = sysfs_create_group(&pwms[pwm].dev->kobj,
						&rpi_pwm_attribute_group);
		if (ret < 0) {
			dev_err(pwms[pwm].dev, 
				"failed to create sysfs device attributes\n");
			goto out2;
		}
		pwms[pwm].loaded = 1;
	}


	clk_reg = ioremap(CLOCK_BASE, 1024);
	pwm_reg = ioremap(PWM_BASE, 1024);
	gpio_reg = ioremap(GPIO_BASE, 1024);
	return 0;

out2:
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++)
		if (pwms[pwm].loaded)
			sysfs_remove_group(&pwms[pwm].dev->kobj,
					 &rpi_pwm_attribute_group);

out1:
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++)
		if (pwms[pwm].dev)
			device_unregister(pwms[pwm].dev);
	class_unregister(&pwm_class);
out0:
	return ret;
}

void __exit rpi_pwm_cleanup(void)
{
	int pwm;
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		if (pwms[pwm].loaded) {
			rpi_pwm_deactivate(&pwms[pwm]);
			sysfs_remove_group(&pwms[pwm].dev->kobj,
						 &rpi_pwm_attribute_group);
		}
		if (pwms[pwm].dev)
			device_unregister(pwms[pwm].dev);
	}

	iounmap(gpio_reg);
	iounmap(pwm_reg);
	iounmap(clk_reg);

	class_unregister(&pwm_class);
}

module_init(rpi_pwm_init);
module_exit(rpi_pwm_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sean Cross <xobs@xoblo.gs> for Adafruit Industries <www.adafruit.com>");
MODULE_ALIAS("platform:bcm2708_pwm");
