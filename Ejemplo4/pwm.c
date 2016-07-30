/*
Constantes que definen el dispositivo
*/
#define pwm_embedded_VERSION "0.1"
#define PWM_CLASS_NAME "pwm-embedded"

/*
Bibliotecas necesarias para el desarrollo
*/
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

/*
Definiciones para el manejo de las direcciones GPIO del chip BCM2708:
BCM2708_PERI_BASE: Direccion inicial para el manejo de GPIO
GPIO_BASE: Direccion del registro GPIO
PWM_BASE: Direccion del registro PWM
CLOCK_BASE: Direccion del registro del ciclo del reloj
Para mas informacion consultar el datasheet BCM2835 FAMILY BCM2708:
https://cdn-shop.adafruit.com/product-files/2885/BCM2835Datasheet.pdf
*/
#define BCM2708_PERI_BASE	0x3F000000
#define GPIO_BASE		(BCM2708_PERI_BASE + 0x200000)
#define PWM_BASE		(BCM2708_PERI_BASE + 0x20C000)
#define CLOCK_BASE		(BCM2708_PERI_BASE + 0x101000)

/*
Definiciones para el manejo  del GPIO
GPIO_REG: Selecciona el GPIO a usar en este caso sera 18
SET_GPIO_ALT: Seleccion de las funciones alternas asignadas al GPIO en este caso sera la funcion 5 que habilita el PWM
*/
#define GPIO_REG(g) (gpio_reg+((g/10)*4))
#define SET_GPIO_ALT(g,a) \
	__raw_writel( 							\
		(((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))		\
		| (__raw_readl(GPIO_REG(g)) & (~(7<<(((g)%10)*3)))),	\
		GPIO_REG(g))
/*
Definicion de los registros para el manejo del PWM, para mas informacion consultar el datasheet BCM2835 FAMILY BCM2708
PWM_CTL: Direccion para el control del pwm
PWM_RNG1: Direccion del rango para el canal 1
PWM_DAT1: Direccion de datos para el canal 1
*/
#define	PWM_CTL  (pwm_reg+(0*4))
#define	PWM_RNG1 (pwm_reg+(4*4))
#define	PWM_DAT1 (pwm_reg+(5*4))
/*
Definicion de los registros para el manejo del PWM, para mas informacion consultar el datasheet BCM2835 FAMILY BCM2708
PWMCLK_CNTL: Direccion para el reloj del control pwm
PWMCLK_DIV: Direccion para el reloj del divisor
PWM_DAT1: Direccion del manejo de datos para el canal 1
*/
#define	PWMCLK_CNTL (clk_reg+(40*4))
#define	PWMCLK_DIV  (clk_reg+(41*4))
/*
Definicion para convertir string a long
*/
#define strict_strtol   kstrtol
/*
Definicion de bloqueo
*/
static DEFINE_MUTEX(sysfs_lock);
/*
Apuntadores de memoria a los registros
pwm_reg
gpio_reg
clk_reg
*/
static void __iomem *pwm_reg;
static void __iomem *gpio_reg;
static void __iomem *clk_reg;

/*
atributos del pwm
duty: el ciclo de trabajo 0 a 50%
frequency: frecuencia 0 - 100 hz
active: activar dispositivo 1 encendido ; 0 apagado
loaded: bandera para saber si esta cargado 1 cargado : 0 descargado
id: identificador de la estructura
divisor: divisor de frequencia
mcf: maxima frecuencia permitida 16 kHz

*/
struct pwm_embedded {
	u32 duty;
	u32 frequency;
	int active:1;
	int loaded:1;
	int id;
	struct device *dev;

	u32 divisor;
	u32 mcf;
};

/*
Arreglo para definir mas de un pwm, en este caso solo se manejara GPIO18
*/
static struct pwm_embedded pwms[] = {
	{
		.duty		= 50,
		.mcf		= 16000,
	},
};

/* 
Funcion para definir el timer para obtener el nuevo divisor de frecuencia
*/
static int pwm_embedded_set_clk(struct pwm_embedded *dev, u32 mcf) {
	/* 
	Se detiene el reloj y esperamos un tiempo ante de detener el reloj
	*/
	__raw_writel(0x5A000000 | (1 << 5), PWMCLK_CNTL);
	udelay(10);  

	if (!dev->mcf) {
		dev_err(dev->dev, "MCF no definido\n");
		return -EINVAL;
	}

	/* 
		Se fija la frecuencia
	*/
	dev->divisor = 19200000 / mcf;
	if (dev->divisor < 1 || dev->divisor > 0x1000) {
		dev_err(dev->dev, "divisor fuera de rango: %x\n", dev->divisor);
		return -ERANGE;
	}
	__raw_writel(0x5A000000 | (dev->divisor<<12), PWMCLK_DIV);
	
	/* 
		Habilitamos el reloj del PWM
	*/
	__raw_writel(0x5A000011, PWMCLK_CNTL);

	return 0;
}

/*
Funcion para definir la frecuencia de salida del PWM
*/
static int pwm_embedded_set_frequency(struct pwm_embedded *dev) {
	unsigned long RNG, DAT;
	int ret;
	/*
		Deshabilitamos el PWM
	*/
	__raw_writel(0, PWM_CTL);

	/* 
		Dejamos un tiempo para que se deshabilite de forma correcta
	*/
	udelay(10);

	ret = pwm_embedded_set_clk(dev, dev->mcf);
	if (ret)
		return ret;

	RNG = dev->mcf/dev->frequency;
	DAT = RNG*dev->duty/100;

	if (RNG < 1) {
		dev_err(dev->dev, "RNG fuera de rango: %ld<1\n", RNG);
		return -ERANGE;
	}

	if (DAT < 1) {
		dev_err(dev->dev, "DAT fuera de rango: %ld<1\n", DAT);
		return -ERANGE;
	}

	__raw_writel(RNG, PWM_RNG1);
	__raw_writel(DAT, PWM_DAT1);

	/* Se inicia PWM */
	__raw_writel(0x81, PWM_CTL);

	return 0;
}

/*
Funcion para activar PWM
*/

static int pwm_embedded_activate(struct pwm_embedded *dev) {
	int ret = 0;

	/* 
		seleccionamos la funcion alternativa del GPIO18 PWM
	*/
	SET_GPIO_ALT(18, 5);

	ret = pwm_embedded_set_frequency(dev);

	dev->active = 1;
	return ret;
}

/*
Funcion para desactivar PWM
*/
static int pwm_embedded_deactivate(struct pwm_embedded *dev) {
	udelay(10);
	SET_GPIO_ALT(18, 0);
	udelay(10);
	dev->active = 1;
	return 0;
}

/*
	Atributo active
*/

static ssize_t active_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct pwm_embedded *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
	//ret = sprintf(buf, "%d\n", !!dev->active);
	ret = sprintf(buf, "%d\n", 1);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t active_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_active;
	struct pwm_embedded *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_active);
	if (ret == 0) {
		if (new_active)
			ret = pwm_embedded_activate(dev);
		else
			ret = pwm_embedded_deactivate(dev);
	}
	else
		ret = -EINVAL;
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}

static DEVICE_ATTR(active, 0664, active_show, active_store);

/*
	Atributo duty
*/
static ssize_t duty_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct pwm_embedded *dev = dev_get_drvdata(d);
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
	struct pwm_embedded *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_duty);
	if (ret == 0) {
		if (new_duty > 0 && new_duty < 100) {
			dev->duty = new_duty;

			pwm_embedded_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}

static DEVICE_ATTR(duty, 0664, duty_show, duty_store);

/*
Atributo MCF
*/
static ssize_t mcf_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct pwm_embedded *dev = dev_get_drvdata(d);
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
	struct pwm_embedded *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_mcf);
	if (ret == 0) {
		if (new_mcf > 1 && new_mcf < 100000000) {
			dev->mcf = new_mcf;

			pwm_embedded_activate(dev);
		}
		else
			ret = -ERANGE;
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}

static DEVICE_ATTR(mcf, 0664, mcf_show, mcf_store);

/*
Atributo frecuencia
*/
static ssize_t freq_show(struct device *d,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct pwm_embedded *dev = dev_get_drvdata(d);
	mutex_lock(&sysfs_lock);
//	ret = sprintf(buf, "%d\n", dev->frequency);
	ret = sprintf(buf, "%d\n", (int)10);
	mutex_unlock(&sysfs_lock);
	return ret;
}

static ssize_t freq_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = 0;
	long new_freq;
	struct pwm_embedded *dev = dev_get_drvdata(d);

	mutex_lock(&sysfs_lock);
	ret = strict_strtol(buf, 0, &new_freq);
	if (ret == 0) {
		dev->frequency = new_freq;

		pwm_embedded_activate(dev);
	}
	mutex_unlock(&sysfs_lock);
	return ret?ret:count;
}

static DEVICE_ATTR(frequency, 0664, freq_show, freq_store);

/*
Estructura que contiene los atributos de sysfs
*/
static struct attribute *pwm_embedded_sysfs_entries[] = {
	&dev_attr_active.attr,
	&dev_attr_duty.attr,
	&dev_attr_mcf.attr,
	&dev_attr_frequency.attr,
	NULL
};

/*
Estructura que define el grupo
*/
static struct attribute_group pwm_embedded_attribute_group = {
	.name = NULL,
	.attrs = pwm_embedded_sysfs_entries,
};

/*
Estructura que define a la clase
*/
static struct class pwm_class = {
	.name =		PWM_CLASS_NAME,
	.owner =	THIS_MODULE,
};

/*
Funcion para poder llamar el script de usuario
*/
static void iniciarPWM(void){ 
  char *argv[] = { "/bin/pwm", NULL };
  static char *envp[] = {
	"HOME=/",
        "TERM=linux",
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
  call_usermodehelper( argv[0], argv, envp, UMH_WAIT_PROC );
}



/*
Funcion inicial
*/
int __init pwm_embedded_init(void)
{
	int ret = 0;
	int pwm = 0;

	pr_info("Driver PWM v%s\n", pwm_embedded_VERSION);

	ret = class_register(&pwm_class);
	if (ret < 0) {
		pr_err("%s: No se pudo registrar la clase\n", pwm_class.name);
		goto out0;
	}


	/* 
	Crea un dispositivo para cada pwm definido
	*/
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		pwms[pwm].id = pwm;
		pwms[pwm].dev = device_create(&pwm_class, &platform_bus,
				MKDEV(0, 0), &pwms[pwm], "pwm%u", pwm);
		if (IS_ERR(pwms[pwm].dev)) {
			pr_err("%s: No se pudo crear dispositivo\n", pwm_class.name);
			ret = PTR_ERR(pwms[pwm].dev);
			goto out1;
		}
	}

	/* 
		Crear sysfs para cada uno de los pwm definidos
	 */
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		ret = sysfs_create_group(&pwms[pwm].dev->kobj,
						&pwm_embedded_attribute_group);
		if (ret < 0) {
			dev_err(pwms[pwm].dev, 
				"No se pudo crear los atributos del dispositivo\n");
			goto out2;
		}
		pwms[pwm].loaded = 1;
	}

	/*
	 Se definen los apuntadores a la direccion de memoria para:
	 clk_reg
	 pwm_reg
	 gpio_reg
	*/
	clk_reg = ioremap(CLOCK_BASE, 1024);
	pwm_reg = ioremap(PWM_BASE, 1024);
	gpio_reg = ioremap(GPIO_BASE, 1024);

	udelay(1);  
	iniciarPWM();
	return 0;

/*
En caso de obtener un error en el flujo de la creacion del pwm se dara roll back segun sea el caso
*/	
out2:
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++)
		if (pwms[pwm].loaded)
			sysfs_remove_group(&pwms[pwm].dev->kobj,
					 &pwm_embedded_attribute_group);

out1:
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++)
		if (pwms[pwm].dev)
			device_unregister(pwms[pwm].dev);
	class_unregister(&pwm_class);
out0:
	return ret;
}

/*
funcion de salida 
Se descarga los direccionamientos de memoria y el sysfs creado
*/
void __exit pwm_embedded_cleanup(void)
{
	int pwm;
	for (pwm=0; pwm<ARRAY_SIZE(pwms); pwm++) {
		if (pwms[pwm].loaded) {
			pwm_embedded_deactivate(&pwms[pwm]);
			sysfs_remove_group(&pwms[pwm].dev->kobj,
						 &pwm_embedded_attribute_group);
		}
		if (pwms[pwm].dev)
			device_unregister(pwms[pwm].dev);
	}

	iounmap(gpio_reg);
	iounmap(pwm_reg);
	iounmap(clk_reg);

	class_unregister(&pwm_class);

/*	free_irq(irq_any_gpio,GPIO_ANY_GPIO_DEVICE_DESC);
	gpio_free(GPIO_ANY_GPIO);*/
}

module_init(pwm_embedded_init);
module_exit(pwm_embedded_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Led RGB");
MODULE_ALIAS("bcm2708_pwm");
