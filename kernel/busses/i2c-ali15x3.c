/*
    ali15x3.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring
    Copyright (c) 1999  Frodo Looijaard <frodol@dds.nl> and
    Philip Edelbrock <phil@netroedge.com> and
    Mark D. Studebaker <mds@eng.paradyne.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
    This is the driver for the SMB Host controller on
    Acer Labs Inc. (ALI) M1541 and M1543C South Bridges.

    The M1543C is a South bridge for desktop systems.
    The M1541 is a South bridge for portable systems.
    They are part of the following ALI chipsets:
       "Aladdin Pro 2": Includes the M1621 Slot 1 North bridge
       with AGP and 100MHz CPU Front Side bus
       "Aladdin V": Includes the M1541 Socket 7 North bridge
       with AGP and 100MHz CPU Front Side bus
       "Aladdin IV": Includes the M1541 Socket 7 North bridge
       with host bus up to 83.3 MHz.
    For an overview of these chips see http://www.acerlabs.com

    The M1533/M1543C devices appear as FOUR separate devices
    on the PCI bus. An output of lspci will show something similar
    to the following:

	00:02.0 USB Controller: Acer Laboratories Inc. M5237
	00:03.0 Bridge: Acer Laboratories Inc. M7101
	00:07.0 ISA bridge: Acer Laboratories Inc. M1533
	00:0f.0 IDE interface: Acer Laboratories Inc. M5229

    The SMB controller is part of the 7101 device, which is an
    ACPI-compliant Power Management Unit (PMU).

    The whole 7101 device has to be enabled for the SMB to work.
    You can't just enable the SMB alone.
    The SMB and the ACPI have separate I/O spaces.
    So we have to make sure that both the SMB and the ACPI
    are mapped and enabled.

    This driver controls the SMB Host only.
    The SMB Slave controller on the M15X3 is not enabled.

    This driver requests the I/O space both for the SMB and the ACPI
    registers, just to be safe. It doesn't actually use the ACPI region.
    It will therefore conflict with separate software
    that accesses the ACPI registers?
    To fix this, undefine MAP_ACPI.

    This driver does not use interrupts.
*/

/* Note: we assume there can only be one ALI15X3, with one SMBus interface */

#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include "version.h"
#include "compat.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,1,54))
#include <linux/bios32.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,53)
#include <linux/init.h>
#else
#define __init 
#define __initdata 
#endif

/* undefine this if separate ACPI software is accessing the
   registers at the offset defined at 0x10.
*/
#define MAP_ACPI 1
#undef FORCE_ALI15X3_ENABLE

/* ALI15X3 SMBus address offsets */
#define SMBHSTSTS (0 + ali15x3_smba)
#define SMBHSTCNT (1 + ali15x3_smba)
#define SMBHSTSTART (2 + ali15x3_smba)
#define SMBHSTCMD (7 + ali15x3_smba)
#define SMBHSTADD (3 + ali15x3_smba)
#define SMBHSTDAT0 (4 + ali15x3_smba)
#define SMBHSTDAT1 (5 + ali15x3_smba)
#define SMBBLKDAT (6 + ali15x3_smba)

/* PCI Address Constants */
#define SMBCOM    0x004 
#define ACPIBA     0x010
#define SMBBA     0x014
#define SMBATPC   0x05B		/* used to unlock xxxBA registers */
#define SMBHSTCFG 0x0E0
#define SMBSLVC   0x0E1
#define SMBCLK    0x0E2
#define SMBREV    0x008

/* Other settings */
#define MAX_TIMEOUT 500		/* times 1/100 sec */
#define ALI15X3_ACPI_IOSIZE 64
#define ALI15X3_SMB_IOSIZE 32

/* this is what the Award 1004 BIOS sets them to on a ASUS P5A MB.
   We don't use these here. If the bases aren't set to some value we
   tell user to upgrade BIOS and we fail.
*/
#define ALI15X3_ACPI_DEFAULTBASE 0xEC00
#define ALI15X3_SMB_DEFAULTBASE 0xE800

/* ALI15X3 address lock bits */
#define ALI15X3_LOCK	0x06

/* ALI15X3 command constants */
#define ALI15X3_ABORT      0x02
#define ALI15X3_T_OUT      0x04
#define ALI15X3_QUICK      0x00
#define ALI15X3_BYTE       0x10
#define ALI15X3_BYTE_DATA  0x20
#define ALI15X3_WORD_DATA  0x30
#define ALI15X3_BLOCK_DATA 0x40
#define ALI15X3_BLOCK_CLR  0x80

/* ALI15X3 status register bits */
#define ALI15X3_STS_IDLE	0x04          
#define ALI15X3_STS_BUSY	0x08          
#define ALI15X3_STS_DONE	0x10          
#define ALI15X3_STS_DEV		0x20	/* device error */
#define ALI15X3_STS_COLL	0x40	/* collision or no response */
#define ALI15X3_STS_TERM	0x80	/* terminated by abort */
#define ALI15X3_STS_ERR		0xE0	/* all the bad error bits */


#ifdef MODULE
static
#else
extern
#endif
       int __init i2c_ali15x3_init(void);
static int __init ali15x3_cleanup(void);
static int ali15x3_setup(void);
static s32 ali15x3_access(struct i2c_adapter *adap, u8 addr, 
                          unsigned short flags,char read_write,
                          u8 command, int size, union i2c_smbus_data * data);
static void ali15x3_do_pause( unsigned int amount );
static int ali15x3_transaction(void);
static void ali15x3_inc(struct i2c_adapter *adapter);
static void ali15x3_dec(struct i2c_adapter *adapter);
static u32 ali15x3_func(struct i2c_adapter *adapter);

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

static struct i2c_algorithm smbus_algorithm = {
  /* name */		"Non-I2C SMBus adapter",
  /* id */		I2C_ALGO_SMBUS,
  /* master_xfer */	NULL,
  /* smbus_access */    ali15x3_access,
  /* slave_send */	NULL,
  /* slave_rcv */	NULL,
  /* algo_control */	NULL,
  /* functionality */   ali15x3_func,
};

static struct i2c_adapter ali15x3_adapter = {
  "unset",
  I2C_ALGO_SMBUS | I2C_HW_SMBUS_ALI15X3,
  &smbus_algorithm,
  NULL,
  ali15x3_inc,
  ali15x3_dec,
  NULL,
  NULL,
};

static int __initdata ali15x3_initialized;
#ifdef MAP_ACPI
static unsigned short ali15x3_acpia = 0;
#endif
static unsigned short ali15x3_smba = 0;


/* Detect whether a ALI15X3 can be found, and initialize it, where necessary.
   Note the differences between kernels with the old PCI BIOS interface and
   newer kernels with the real PCI interface. In compat.h some things are
   defined to make the transition easier. */
int ali15x3_setup(void)
{
  int error_return=0;
  unsigned char temp;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54))
  struct pci_dev *ALI15X3_dev;
#else
  unsigned char ALI15X3_bus, ALI15X3_devfn = 0;
  int res;
#endif

  /* First check whether we can access PCI at all */
  if (pci_present() == 0) {
    printk("i2c-ali15x3.o: Error: No PCI-bus found!\n");
    error_return=-ENODEV;
    goto END;
  }

  /* Look for the ALI15X3, M7101 device */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54))
  ALI15X3_dev = NULL;
  ALI15X3_dev = pci_find_device(PCI_VENDOR_ID_AL, 
                                PCI_DEVICE_ID_AL_M7101, ALI15X3_dev);
  if(ALI15X3_dev == NULL) {
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(2,1,54) */
    res = pcibios_find_device(PCI_VENDOR_ID_AL,
                                    PCI_DEVICE_ID_AL_M7101,
                                    0,&ALI15X3_bus, &ALI15X3_devfn);
     
  if (res) {
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54) */
    printk("i2c-ali15x3.o: Error: Can't detect ali15x3!\n");
    error_return=-ENODEV;
    goto END;
  } 

/* Check the following things:
	- ACPI and SMB I/O addresses are initialized
	- Device is enabled
	- We can use the addresses
*/

/* Unlock the register.
   The data sheet says that the address registers are read-only
   if the lock bits are 1, but in fact the address registers
   are zero unless you clear the lock bits.
*/
  pci_read_config_byte_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                              SMBATPC, &temp);
  if(temp & ALI15X3_LOCK)
  {   
    temp &= ~ALI15X3_LOCK;
    pci_write_config_byte_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                                 SMBATPC, temp);
  }

/* Determine the address of the ACPI and SMBus areas */
#ifdef MAP_ACPI
  pci_read_config_word_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                              ACPIBA,&ali15x3_acpia);
  ali15x3_acpia &= (0xffff & ~ (ALI15X3_ACPI_IOSIZE - 1));
  if(ali15x3_acpia == 0) {
    printk("i2c-ali15x3.o: ALI15X3_acpi region uninitialized - upgrade BIOS?\n");
    error_return=-ENODEV;
  }
#endif

  pci_read_config_word_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                              SMBBA,&ali15x3_smba);
  ali15x3_smba &= (0xffff & ~ (ALI15X3_SMB_IOSIZE - 1));
  if(ali15x3_smba == 0) {
    printk("i2c-ali15x3.o: ALI15X3_smb region uninitialized - upgrade BIOS?\n");
    error_return=-ENODEV;
  }

  if(error_return == -ENODEV)
    goto END;

#ifdef MAP_ACPI
  if (check_region(ali15x3_acpia, ALI15X3_ACPI_IOSIZE)) {
    printk("i2c-ali15x3.o: ALI15X3_acpi region 0x%x already in use!\n", ali15x3_acpia);
    printk("i2c-ali15x3.o: If conflicting ACPI software is installed, undefine MAP_ACPI and recompile!\n");
    error_return=-ENODEV;
  }
#endif

  if (check_region(ali15x3_smba, ALI15X3_SMB_IOSIZE)) {
    printk("i2c-ali15x3.o: ALI15X3_smb region 0x%x already in use!\n", ali15x3_smba);
    error_return=-ENODEV;
  }

  if(error_return == -ENODEV)
    goto END;

/* check if whole device is enabled */
    pci_read_config_byte_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                                SMBCOM, &temp);
  if ((temp & 1) == 0) {
    printk("SMBUS: Error: ACPI/SMB device not enabled - upgrade BIOS?\n");     
    error_return=-ENODEV;
    goto END;
  }

/* Is SMB Host controller enabled? */
  pci_read_config_byte_united(ALI15X3_dev, ALI15X3_bus, ALI15X3_devfn,
                              SMBHSTCFG, &temp);
#ifdef FORCE_ALI15X3_ENABLE
/* This should never need to be done.
   NOTE: This assumes I/O space and other allocations WERE
   done by the Bios!  Don't complain if your hardware does weird 
   things after enabling this. :') Check for Bios updates before
   resorting to this.  */
  if ((temp & 1) == 0) {
    pci_write_config_byte_united(ALI15X3_dev, ALI15X3_bus, ALI15X3_devfn,
                                     SMBHSTCFG, temp | 1);
    printk("i2c-ali15x3.o: WARNING: ALI15X3 SMBus interface has been FORCEFULLY "
           "ENABLED!!\n");
  }
#else /* FORCE_ALI15X3_ENABLE */
  if ((temp & 1) == 0) {
    printk("SMBUS: Error: Host SMBus controller not enabled - upgrade BIOS?\n");     
    error_return=-ENODEV;
    goto END;
  }
#endif /* FORCE_ALI15X3_ENABLE */

/* set SMB clock to 74KHz as recommended in data sheet */
  pci_write_config_byte_united(ALI15X3_dev, ALI15X3_bus ,ALI15X3_devfn,
                               SMBCLK, 0x20);

  /* Everything is happy, let's grab the memory and set things up. */
#ifdef MAP_ACPI
  request_region(ali15x3_acpia, ALI15X3_ACPI_IOSIZE, "ali15x3-acpi");       
#endif
  request_region(ali15x3_smba, ALI15X3_SMB_IOSIZE, "ali15x3-smb");       

#ifdef DEBUG
/*
  The interrupt routing for SMB is set up in register 0x77 in the
  1533 ISA Bridge device, NOT in the 7101 device.
  Don't bother with finding the 1533 device and reading the register.
  if ((....... & 0x0F) == 1)
     printk("i2c-ali15x3.o: ALI15X3 using Interrupt 9 for SMBus.\n");
*/
  pci_read_config_byte_united(ALI15X3_dev, ALI15X3_bus, ALI15X3_devfn, SMBREV, 
                              &temp);
  printk("i2c-ali15x3.o: SMBREV = 0x%X\n",temp);
  printk("i2c-ali15x3.o: ALI15X3_smba = 0x%X\n",ali15x3_smba);
#endif /* DEBUG */

END:
  return error_return;
}


/* Internally used pause function */
void ali15x3_do_pause( unsigned int amount )
{
      current->state = TASK_INTERRUPTIBLE;
      schedule_timeout(amount);
}

/* Another internally used function */
int ali15x3_transaction(void) 
{
  int temp;
  int result=0;
  int timeout=0;

#ifdef DEBUG
  printk("i2c-ali15x3.o: Transaction (pre): STS=%02x, CNT=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, "
         "DAT1=%02x\n",
         inb_p(SMBHSTSTS), inb_p(SMBHSTCNT),inb_p(SMBHSTCMD),inb_p(SMBHSTADD),inb_p(SMBHSTDAT0),
         inb_p(SMBHSTDAT1));
#endif

  /* get status */
  temp = inb_p(SMBHSTSTS);

  /* Make sure the SMBus host is ready to start transmitting */
  /* Check the busy bit first */
  if (temp & ALI15X3_STS_BUSY) {
/*
   If the host controller is still busy, it may have timed out in the previous transaction,
   resulting in a "SMBus Timeout" printk.
   I've tried the following to reset a stuck busy bit.
	1. Reset the controller with an ABORT command.
	   (this doesn't seem to clear the controller if an external device is hung)
	2. Reset the controller and the other SMBus devices with a T_OUT command.
	   (this clears the host busy bit if an external device is hung,
	   but it comes back upon a new access to a device)
	3. Disable and reenable the controller in SMBHSTCFG
   Worst case, nothing seems to work except power reset.
*/
/* Abort - reset the host controller */
/*
#ifdef DEBUG
    printk("i2c-ali15x3.o: Resetting host controller to clear busy condition\n",temp);
#endif
    outb_p(ALI15X3_ABORT, SMBHSTCNT);
    temp = inb_p(SMBHSTSTS);
    if (temp & ALI15X3_STS_BUSY) {
*/
  
/*
   Try resetting entire SMB bus, including other devices -
   This may not work either - it clears the BUSY bit but
   then the BUSY bit may come back on when you try and use the chip again.
   If that's the case you are stuck.
*/
       printk("i2c-ali15x3.o: Resetting entire SMB Bus to clear busy condition (%02x)\n",temp);
       outb_p(ALI15X3_T_OUT, SMBHSTCNT);
       temp = inb_p(SMBHSTSTS);
     }
/*
  }
*/

  /* now check the error bits and the busy bit */
  if (temp & (ALI15X3_STS_ERR | ALI15X3_STS_BUSY)) {
    /* do a clear-on-write */
    outb_p(0xFF, SMBHSTSTS);
    if ((temp = inb_p(SMBHSTSTS)) & (ALI15X3_STS_ERR | ALI15X3_STS_BUSY)) {
      /* this is probably going to be correctable only by a power reset
         as one of the bits now appears to be stuck */
      /* This may be a bus or device with electrical problems. */
      printk("i2c-ali15x3.o: SMBus reset failed! (0x%02x) - controller or device on bus is probably hung\n",temp);
      return -1;
    }
  } else {
    /* check and clear done bit */
    if (temp & ALI15X3_STS_DONE) {
      outb_p(temp, SMBHSTSTS);
    }
  }

  /* start the transaction by writing anything to the start register */
  outb_p(0xFF, SMBHSTSTART); 

  /* We will always wait for a fraction of a second! */
  timeout = 0;
  do {
    ali15x3_do_pause(1);
    temp=inb_p(SMBHSTSTS);
  } while ((!(temp & (ALI15X3_STS_ERR | ALI15X3_STS_DONE))) && (timeout++ < MAX_TIMEOUT));

  /* If the SMBus is still busy, we give up */
  if (timeout >= MAX_TIMEOUT) {
    result = -1;
    printk("i2c-ali15x3.o: SMBus Timeout!\n"); 
  }

  if (temp & ALI15X3_STS_TERM) {
    result = -1;
#ifdef DEBUG
    printk("i2c-ali15x3.o: Error: Failed bus transaction\n");
#endif
  }

/*
  Unfortunately the ALI SMB controller maps "no response" and "bus collision"
  into a single bit. No reponse is the usual case so don't
  do a printk.
  This means that bus collisions go unreported.
*/
  if (temp & ALI15X3_STS_COLL) {
    result = -1;
#ifdef DEBUG
    printk("i2c-ali15x3.o: Error: no response or bus collision ADD=%02x\n", inb_p(SMBHSTADD));
#endif
  }

/* haven't ever seen this */
  if (temp & ALI15X3_STS_DEV) {
    result = -1;
    printk("i2c-ali15x3.o: Error: device error\n");
  }

#ifdef DEBUG
  printk("i2c-ali15x3.o: Transaction (post): STS=%02x, CNT=%02x, CMD=%02x, ADD=%02x, "
         "DAT0=%02x, DAT1=%02x\n",
         inb_p(SMBHSTSTS), inb_p(SMBHSTCNT),inb_p(SMBHSTCMD),inb_p(SMBHSTADD),inb_p(SMBHSTDAT0),
         inb_p(SMBHSTDAT1));
#endif
  return result;
}

/* Return -1 on error. See smbus.h for more information */
s32 ali15x3_access(struct i2c_adapter *adap, u8 addr, unsigned short flags,
                   char read_write, u8 command, int size, 
                   union i2c_smbus_data * data)
{
  int i,len;
  int temp;
  int timeout;

/* clear all the bits (clear-on-write) */
  outb_p(0xFF, SMBHSTSTS); 
/* make sure SMBus is idle */
  temp = inb_p(SMBHSTSTS);
  for(timeout = 0; (timeout < MAX_TIMEOUT) && !(temp & ALI15X3_STS_IDLE); timeout++)
  {
    ali15x3_do_pause(1);
    temp=inb_p(SMBHSTSTS);
  }
  if (timeout >= MAX_TIMEOUT) {
    printk("i2c-ali15x3.o: Idle wait Timeout! STS=0x%02x\n", temp); 
  }

  switch(size) {
    case I2C_SMBUS_PROC_CALL:
      printk("i2c-ali15x3.o: I2C_SMBUS_PROC_CALL not supported!\n");
      return -1;
    case I2C_SMBUS_QUICK:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      size = ALI15X3_QUICK;
      break;
    case I2C_SMBUS_BYTE:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      if (read_write == I2C_SMBUS_WRITE)
        outb_p(command, SMBHSTCMD);
      size = ALI15X3_BYTE;
      break;
    case I2C_SMBUS_BYTE_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE)
        outb_p(data->byte,SMBHSTDAT0);
      size = ALI15X3_BYTE_DATA;
      break;
    case I2C_SMBUS_WORD_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE) {
        outb_p(data->word & 0xff,SMBHSTDAT0);
        outb_p((data->word & 0xff00) >> 8,SMBHSTDAT1);
      }
      size = ALI15X3_WORD_DATA;
      break;
    case I2C_SMBUS_BLOCK_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE) {
        len = data->block[0];
        if (len < 0) {
          len = 0;
          data->block[0] = len;
        }	
        if (len > 32) {
          len = 32;
          data->block[0] = len;
        }	
        outb_p(len,SMBHSTDAT0);
        outb_p(inb_p(SMBHSTCNT) | ALI15X3_BLOCK_CLR, SMBHSTCNT); /* Reset SMBBLKDAT */
        for (i = 1; i <= len; i++)
          outb_p(data->block[i],SMBBLKDAT);
      }
      size = ALI15X3_BLOCK_DATA;
      break;
  }

  outb_p(size, SMBHSTCNT);	/* output command */

  if (ali15x3_transaction()) /* Error in transaction */ 
    return -1; 
  
  if ((read_write == I2C_SMBUS_WRITE) || (size == ALI15X3_QUICK))
    return 0;
  

  switch(size) {
    case ALI15X3_BYTE: /* Result put in SMBHSTDAT0 */
      data->byte = inb_p(SMBHSTDAT0);
      break;
    case ALI15X3_BYTE_DATA:
      data->byte = inb_p(SMBHSTDAT0);
      break;
    case ALI15X3_WORD_DATA:
      data->word = inb_p(SMBHSTDAT0) + (inb_p(SMBHSTDAT1) << 8);
      break;
    case ALI15X3_BLOCK_DATA:
      len = inb_p(SMBHSTDAT0);
      if(len > 32)	
        len = 32;
      data->block[0] = len;
      outb_p(inb_p(SMBHSTCNT) | ALI15X3_BLOCK_CLR, SMBHSTCNT); /* Reset SMBBLKDAT */
      for (i = 1; i <= data->block[0]; i++) {
        data->block[i] = inb_p(SMBBLKDAT);
#ifdef DEBUG
        printk("i2c-ali15x3.o: Blk: len=%d, i=%d, data=%02x\n", len, i, data->block[i]);
#endif DEBUG
      }
      break;
  }
  return 0;
}

void ali15x3_inc(struct i2c_adapter *adapter)
{
	MOD_INC_USE_COUNT;
}

void ali15x3_dec(struct i2c_adapter *adapter)
{

	MOD_DEC_USE_COUNT;
}

u32 ali15x3_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
               I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
               I2C_FUNC_SMBUS_BLOCK_DATA;
}

int __init i2c_ali15x3_init(void)
{
  int res;
  printk("ali15x3.o version %s (%s)\n",LM_VERSION,LM_DATE);
#ifdef DEBUG
/* PE- It might be good to make this a permanent part of the code! */
  if (ali15x3_initialized) {
    printk("i2c-ali15x3.o: Oops, ali15x3_init called a second time!\n");
    return -EBUSY;
  }
#endif
  ali15x3_initialized = 0;
  if ((res = ali15x3_setup())) {
    printk("i2c-ali15x3.o: ALI15X3 not detected, module not inserted.\n");
    ali15x3_cleanup();
    return res;
  }
  ali15x3_initialized ++;
#ifdef MAP_ACPI
  sprintf(ali15x3_adapter.name,"ACPI ALI15X3 at %04x",ali15x3_acpia);
#endif
  sprintf(ali15x3_adapter.name,"SMBus ALI15X3 adapter at %04x",ali15x3_smba);
  if ((res = i2c_add_adapter(&ali15x3_adapter))) {
    printk("i2c-ali15x3.o: Adapter registration failed, module not inserted.\n");
    ali15x3_cleanup();
    return res;
  }
  ali15x3_initialized++;
  printk("i2c-ali15x3.o: ALI15X3 SMBus Controller detected and initialized\n");
  return 0;
}

int __init ali15x3_cleanup(void)
{
  int res;
  if (ali15x3_initialized >= 2)
  {
    if ((res = i2c_del_adapter(&ali15x3_adapter))) {
      printk("i2c-ali15x3.o: i2c_del_adapter failed, module not removed\n");
      return res;
    } else
      ali15x3_initialized--;
  }
  if (ali15x3_initialized >= 1) {
#ifdef MAP_ACPI
    release_region(ali15x3_acpia, ALI15X3_ACPI_IOSIZE);
#endif
    release_region(ali15x3_smba, ALI15X3_SMB_IOSIZE);
    ali15x3_initialized--;
  }
  return 0;
}

EXPORT_NO_SYMBOLS;

#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>, Philip Edelbrock <phil@netroedge.com>, and Mark D. Studebaker <mds@eng.paradyne.com>");
MODULE_DESCRIPTION("ALI15X3 SMBus driver");

int init_module(void)
{
  return i2c_ali15x3_init();
}

int cleanup_module(void)
{
  return ali15x3_cleanup();
}

#endif /* MODULE */
