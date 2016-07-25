#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
 
#include <linux/interrupt.h>
#include <linux/gpio.h>
 
 
#define DRIVER_AUTHOR "Igor <hardware.coder@gmail.com>"
#define DRIVER_DESC   "Tnterrupt Test"
 
// we want GPIO_17 (pin 11 on P5 pinout raspberry pi rev. 2 board)
// to generate interrupt
#define GPIO_ANY_GPIO                18
 
// text below will be seen in 'cat /proc/interrupt' command
#define GPIO_ANY_GPIO_DESC           "Some gpio pin description"
 
// below is optional, used in more complex code, in our case, this could be
// NULL
#define GPIO_ANY_GPIO_DEVICE_DESC    "some_device"
 
 
/****************************************************************************/
/* Interrupts variables block                                               */
/****************************************************************************/
short int irq_any_gpio    = 0;


/****************************************************************************/
/* Gpio for direcction start                                                */
/****************************************************************************/
 
static struct gpio leds[]={
	{27, GPIOF_OUT_INIT_HIGH, "Led 1"}
};

short int ret; 
short int power=0;

/****************************************************************************/
/* IRQ handler - fired on interrupt                                         */
/****************************************************************************/
static irqreturn_t r_irq_handler(int irq, void *dev_id, struct pt_regs *regs) {
 
   unsigned long flags;
   
   // disable hard interrupts (remember them in flag 'flags')
   local_irq_save(flags);
 
   // NOTE:
   // Anonymous Sep 17, 2013, 3:16:00 PM:
   // You are putting printk while interupt are disabled. printk can block.
   // It's not a good practice.
   // 
   // hardware.coder:
   // http://stackoverflow.com/questions/8738951/printk-inside-an-interrupt-handler-is-it-really-that-bad
   
   printk(KERN_NOTICE "Interrupt power (%d) [%d] for device %s was triggered !.\n",power,
          irq, (char *) dev_id);
 
   //GPIO
   if(power){
   	gpio_set_value(leds[0].gpio, 1); 
	power=0;
   } else {
   	gpio_set_value(leds[0].gpio, 0); 
	power=1;
   }
	
   // restore hard interrupts
   local_irq_restore(flags);
 
   return IRQ_HANDLED;
}
 
 
/****************************************************************************/
/* This function configures interrupts.                                     */
/****************************************************************************/
void r_int_config(void) {
 
   if (gpio_request(GPIO_ANY_GPIO, GPIO_ANY_GPIO_DESC)) {
      printk("GPIO request faiure: %s\n", GPIO_ANY_GPIO_DESC);
      return;
   }
 
   if ( (irq_any_gpio = gpio_to_irq(GPIO_ANY_GPIO)) < 0 ) {
      printk("GPIO to IRQ mapping faiure %s\n", GPIO_ANY_GPIO_DESC);
      return;
   }
 
   printk(KERN_NOTICE "Mapped int %d\n", irq_any_gpio);
 
   if (request_irq(irq_any_gpio,
                   (irq_handler_t ) r_irq_handler,
                   IRQF_TRIGGER_FALLING,
                   GPIO_ANY_GPIO_DESC,
                   GPIO_ANY_GPIO_DEVICE_DESC)) {
      printk("Irq Request failure\n");
      return;
   }

   ret = gpio_request_array(leds,ARRAY_SIZE(leds));
 
   if(ret){
	printk(KERN_ERR "Unable request GIPO %d\n",ret);
   }
   return;
}
 
 
/****************************************************************************/
/* This function releases interrupts.                                       */
/****************************************************************************/
void r_int_release(void) {
 
   free_irq(irq_any_gpio, GPIO_ANY_GPIO_DEVICE_DESC);
   gpio_free(GPIO_ANY_GPIO);
 
   return;
}
 
 
/****************************************************************************/
/* Module init / cleanup block.                                             */
/****************************************************************************/
int r_init(void) {
 
   printk(KERN_NOTICE "Hello !\n");
   r_int_config();
 
   return 0;
}
 
void r_cleanup(void) {
   printk(KERN_NOTICE "Goodbye\n");
   r_int_release();
 
   return;
}
 
 
module_init(r_init);
module_exit(r_cleanup);
 
 
/****************************************************************************/
/* Module licensing/description block.                                      */
/****************************************************************************/
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
