#include <Arduino.h>
#include <io.h>
#include <mips/asm.h>
#include <mips/cpuregs.h>
#include <sys/isr.h>
#include "wiring_private.h"
#include "emard_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 8 main MIPS interrupts, all initially disabled */
static volatile voidFuncPtr intFunc[8] = 
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, };

/* some interrupts sources are multiplexed on same mips irq, 
   here are callbacks for timer */
static volatile voidFuncPtr timerFunc[VARIANT_ICPN+VARIANT_OCPN] = {
    NULL, NULL, // OCP1, OCP2
    NULL, NULL, // ICP1, ICP2
};
/* todo: join flags with timeFunc with above in a suitable struct
** struct will help programmer that interrupt flags match called functions
*/
static uint32_t timerIFlags[VARIANT_ICPN+VARIANT_OCPN] = {
  1<<TCTRL_IF_OCP1, 1<<TCTRL_IF_OCP2,
  1<<TCTRL_IF_ICP1, 1<<TCTRL_IF_ICP2,
};

static volatile voidFuncPtr gpio_rising_Func[32] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};
static volatile voidFuncPtr gpio_falling_Func[32] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

const struct variant_icp_control_s variant_icp_control[2] = VARIANT_ICP_CONTROL;

static int tsc_next;
int timerInterval = VARIANT_MCK; // default timer interval is 1 second

/* MIPS timer (inside of the core) */
static int tsc_isr(void)
{
  if(intFunc[7])
  {
    intFunc[7]();
    tsc_next += timerInterval;
    mtc0_macro(tsc_next, MIPS_COP_0_COMPARE);
  }
  return 1;
}
static struct isr_link tsc_isr_link = {.handler_fn = &tsc_isr};

/* emard advanced timer, memory mapped I/O (outside of the core) */
static int timer_isr(void)
{
  int8_t i;
  
  for(i = VARIANT_ICPN+VARIANT_OCPN-1; i >= 0; i--)
  {
    if( (EMARD_TIMER[TC_CONTROL] & timerIFlags[i]) != 0 )
    {
      EMARD_TIMER[TC_CONTROL] &= ~timerIFlags[i]; // clear the interrupt flag
      if(timerFunc[i])
        timerFunc[i](); // call the function
    }
  }
  return 1;
}
static struct isr_link timer_isr_link = {.handler_fn = &timer_isr};

/* gpio with interrupts */
static int gpio_isr(void)
{
  int8_t i;
  uint32_t bit;
  volatile uint32_t *if_rising  = (volatile uint32_t *)IO_GPIO_RISING_IF;
  volatile uint32_t *if_falling = (volatile uint32_t *)IO_GPIO_FALLING_IF;
  
  for(i = 31, bit = (1<<31); i >= 0; i--, bit >>= 1)
  {
    if( (bit & *if_rising) != 0 )
    {
      *if_rising = ~bit; // implicit &=
      if(gpio_rising_Func[i])
        gpio_rising_Func[i]();
    }
    if( (bit & *if_falling) != 0 )
    {
      *if_falling = ~bit; // implicit &=
      if(gpio_falling_Func[i])
        gpio_falling_Func[i]();
    }
  }
  return 1;
}
static struct isr_link gpio_isr_link = {.handler_fn = &gpio_isr};

void icpFilter(uint32_t pin, uint32_t icp_start, uint32_t icp_stop)
{
  int8_t icp_channel;
  volatile uint32_t *start, *stop;
  
  if(pin >= variant_pin_map_size)
    return;
  
  icp_channel = variant_pin_map[pin].icp;
  if(icp_channel >= 0)
  {
    EMARD_TIMER[TC_CONTROL] &= variant_icp_control[icp_channel].control_and;
    
    start = &EMARD_TIMER[variant_icp_control[icp_channel].icp_start];
    stop  = &EMARD_TIMER[variant_icp_control[icp_channel].icp_stop];
    
    *start = icp_start;
    *stop = icp_stop;
    
    EMARD_TIMER[TC_CONTROL] &= variant_icp_control[icp_channel].control_and;
    if( start <= stop )
      EMARD_TIMER[TC_CONTROL] |= variant_icp_control[icp_channel].control_and_or;
    EMARD_TIMER[TC_CONTROL] |= variant_icp_control[icp_channel].control_or;
    EMARD_TIMER[TC_APPLY] = variant_icp_control[icp_channel].apply;
  }
}

void attachInterrupt(uint32_t pin, void (*callback)(void), uint32_t mode)
{
  int32_t irq = -1;
  int8_t icp, ocp, bit;
  /* attachInterrupt is ment to assign pin change interrupt
  ** on digital input pins
  ** but we will here misuse it to create timer interrupt.
  ** LED pin 13 is chosen as 'magic' pin which caries
  ** MIPS timer irq7 interrupt and has nothing to do with LED.
  ** 
  ** mode parameter (if nonzero) is used as timer interval
  ** if zero, timer interval is unchanged (default is 1 second) 
  ** (timerInterval = system clock value in Hz) 
  ** it can be changed at any time
  ** but beware of race condition
  */
  if(pin >= variant_pin_map_size)
    return;
  
  icp = variant_pin_map[pin].icp;
  ocp = variant_pin_map[pin].pwm;
  bit = variant_pin_map[pin].bit;
  
  if(bit == 13)
  {
    uint8_t init_required = 0;
    irq = 7;
    if(mode)
      timerInterval = mode;
    if(intFunc[irq] == NULL)
      init_required = 1;
    intFunc[irq] = callback; // todo - set it above, 
    if(init_required)
    {
      isr_register_handler(irq, &tsc_isr_link); // 7 is MIPS timer interrput
      mfc0_macro(tsc_next, MIPS_COP_0_COUNT);
      tsc_next += timerInterval;
      mtc0_macro(tsc_next, MIPS_COP_0_COMPARE);
    }
    asm("ei");
  }

  if(ocp >= 0 || icp >= 0)
  {
    irq = VARIANT_TIMER_INTERRUPT;
    if(intFunc[irq] == NULL)
    {
      isr_register_handler(irq, &timer_isr_link); // 4 is EMARD timer interrput
      intFunc[irq] = NULL+1; // not used as callback, just as non-zero to init only once
    }
    if(ocp >= 0)
    {
      timerFunc[ocp] = callback;
      EMARD_TIMER[TC_CONTROL] |= pwm_enable_bitmask[ocp].ocp_ie;
      EMARD_TIMER[TC_APPLY] = (1<<TC_CONTROL);
    }
    if(icp >= 0)
    {
      timerFunc[VARIANT_OCPN+icp] = callback;
      EMARD_TIMER[TC_CONTROL] |= variant_icp_control[icp].icp_ie;
      EMARD_TIMER[TC_APPLY] = (1<<TC_CONTROL);
    }
    asm("ei");
  }
  if( variant_pin_map[pin].port == (volatile uint32_t *)IO_GPIO_DATA )
  {
    volatile uint32_t *ie_rising  = (volatile uint32_t *)IO_GPIO_RISING_IE;
    volatile uint32_t *ie_falling = (volatile uint32_t *)IO_GPIO_FALLING_IE;
    irq = 5; // VARIANT_GPIO_INTERRUPT
    /* standard GPIO pin */
    if(bit >= 0)
    {
      if(*ie_rising == 0 && *ie_falling == 0)
        isr_register_handler(irq, &gpio_isr_link); // 5 is gpio interrput
      if(mode == RISING)
      {
        gpio_rising_Func[bit] = callback;
        *ie_rising |= (1<<bit);
      }
      if(mode == FALLING)
      {
        gpio_falling_Func[bit] = callback;
        *ie_falling |= (1<<bit);
      }
      asm("ei");
    }
  }
}

void detachInterrupt(uint32_t pin)
{
  int8_t icp, ocp, bit;
  if(pin >= variant_pin_map_size)
    return;
  icp = variant_pin_map[pin].icp;
  ocp = variant_pin_map[pin].pwm;
  bit = variant_pin_map[pin].bit;
  if(bit == 13)
  {
    int irq = 7;
    asm("di");
    #if 1
    isr_remove_handler(irq, &tsc_isr_link); // 7 is MIPS timer interrput
    #endif
    intFunc[irq] = NULL;
    asm("ei");
  }
  if(ocp >= 0 || icp >= 0)
  {
    if(ocp >= 0)
    {
      EMARD_TIMER[TC_CONTROL] &= ~pwm_enable_bitmask[ocp].ocp_ie;
      EMARD_TIMER[TC_APPLY] = (1<<TC_CONTROL);
      timerFunc[icp] = NULL;
    }
    if(icp >= 0)
    {
      EMARD_TIMER[TC_CONTROL] &= ~variant_icp_control[icp].icp_ie;
      EMARD_TIMER[TC_APPLY] = (1<<TC_CONTROL);
      timerFunc[VARIANT_OCPN+icp] = NULL;
    }
    if( (EMARD_TIMER[TC_CONTROL] 
        & ( (1<<TCTRL_IE_OCP1)
          | (1<<TCTRL_IE_OCP2) 
          | (1<<TCTRL_IE_ICP1)
          | (1<<TCTRL_IE_ICP2)
          )
        ) == 0
      )
    {
      #if 1
      int irq = VARIANT_TIMER_INTERRUPT;
      asm("di");
      isr_remove_handler(irq, &timer_isr_link); // 4 is EMARD timer interrput
      intFunc[irq] = NULL;
      asm("ei");
      #endif
    }
  }
  if( variant_pin_map[pin].port == (volatile uint32_t *)IO_GPIO_DATA )
  {
    volatile uint32_t *ie_rising =  (volatile uint32_t *)IO_GPIO_RISING_IE;
    volatile uint32_t *ie_falling = (volatile uint32_t *)IO_GPIO_FALLING_IE;
    int irq = 5; // VARIANT_GPIO_INTERRUPT
    /* standard GPIO pin */
    if(bit >= 0)
    {
      *ie_rising  &= ~(1<<bit);
      *ie_falling &= ~(1<<bit);
      if(*ie_rising == 0 && *ie_falling == 0)
      {
        asm("di");
        isr_remove_handler(irq, &gpio_isr_link); // 5 is gpio interrput
        asm("ei");
      }
    }        
  }
}


#ifdef __cplusplus
}
#endif
