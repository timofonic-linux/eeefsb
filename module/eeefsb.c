/*
 *  eeefsb.c - Tools for eee PC overclocking
 *
 *  Forked from netbook.c by Olli Vanhoja
 *  Forked from netbook.c by Andrew Wyatt (Fewt)
 *
 *  Copyright (C) 2007 Andrew Tipton
 *  Modifications (C) 2009 Andrew Wyatt
 *  Modifications (C) 2012 Olli Vanhoja
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Ths program is distributed in the hope that it will be useful,
 *  but WITOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTAILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Template Place, Suite 330, Boston, MA  02111-1307 USA
 *  
 *  ---------
 *
 *  This code comes WITHOUT ANY WARRANTY whatsoever.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/i2c.h>
#include <linux/mutex.h>


/* Module info */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Olli Vanhoja / Andrew Wyatt / Andrew Tipton");
MODULE_DESCRIPTION("Tools for eee PC overclocking.");
#define EEEFSB_VERSION "0.1"


/* PLL access functions.
 *
 * Note that this isn't really the "proper" way to use the I2C API... :)
 * I2C_SMBUS_BLOCK_MAX is 32, the maximum size of a block read/write.
 */
static int eeefsb_pll_init(void);
static void eeefsb_pll_read(void);
static void eeefsb_pll_write(void);
static void eeefsb_pll_cleanup(void);


static struct i2c_client eeefsb_pll_smbus_client = {
    .adapter = NULL,
    .addr    = 0x69,
    .flags   = 0,
};
static char eeefsb_pll_data[I2C_SMBUS_BLOCK_MAX];
static int eeefsb_pll_datalen = 0;

static int eeefsb_pll_init(void) {
    int i = 0;
    int found = 0;
    struct i2c_adapter *_adapter;
    
    while ((_adapter = i2c_get_adapter(i)) != NULL) {
        if (strstr(_adapter->name, "I801")) {
            printk("eeefsb: Found i2c adapter %s\n", _adapter->name);
            found = 1;
            break;
        }
        i++;
    }
    
    if (found)
        eeefsb_pll_smbus_client.adapter = _adapter;
    else {
        printk("eeefsb: No i801 adapter found.\n");
        return -1;
    }

    /* Fill the eeefsb_pll_data buffer. */
    eeefsb_pll_read();
    
    return 0;
}

static void eeefsb_pll_read(void) {
    // Takes approx 150ms to execute.
    memset(eeefsb_pll_data, 0, I2C_SMBUS_BLOCK_MAX);
    eeefsb_pll_datalen = i2c_smbus_read_block_data(&eeefsb_pll_smbus_client, 0, eeefsb_pll_data);
}

static void eeefsb_pll_write(void) {
    // Takes approx 150ms to execute ???
    i2c_smbus_write_block_data(&eeefsb_pll_smbus_client, 0, eeefsb_pll_datalen, eeefsb_pll_data);
}

static void eeefsb_pll_cleanup(void) {
    i2c_put_adapter(eeefsb_pll_smbus_client.adapter);
}

/* Embedded controller access functions. **************************************
 *                                                                            *
 * The ENE KB3310 embedded controller has a feature known as "Index IO"       *
 * which allows the entire 64KB address space of the controller to be         *
 * accessed via a set of ISA I/O ports at 0x380-0x384.  This allows us        *
 * direct access to all of the controller's ROM, RAM, SFRs, and peripheral    *
 * registers;  this access bypasses the EC firmware entirely.                 *
 *                                                                            *
 * This is much faster than using ec_transaction(), and it also allows us to  *
 * do things which are not possible through the EC's official interface.      *
 *                                                                            *
 * An Indexed IO write to an EC register takes approx. 90us, while an EC      *
 * transaction takes approx. 2500ms.                                          *
 */
#define EC_IDX_ADDRH 0x381
#define EC_IDX_ADDRL 0x382
#define EC_IDX_DATA 0x383
#define HIGH_BYTE(x) ((x & 0xff00) >> 8)
#define LOW_BYTE(x) (x & 0x00ff)
static DEFINE_MUTEX(eeefsb_ec_mutex);

static unsigned char eeefsb_ec_read(unsigned short addr) {
    unsigned char data;

    mutex_lock(&eeefsb_ec_mutex);
    outb(HIGH_BYTE(addr), EC_IDX_ADDRH);
    outb(LOW_BYTE(addr), EC_IDX_ADDRL);
    data = inb(EC_IDX_DATA);
    mutex_unlock(&eeefsb_ec_mutex);

    return data;
}

static void eeefsb_ec_write(unsigned short addr, unsigned char data) {
    mutex_lock(&eeefsb_ec_mutex);
    outb(HIGH_BYTE(addr), EC_IDX_ADDRH);
    outb(LOW_BYTE(addr), EC_IDX_ADDRL);
    outb(data, EC_IDX_DATA);
    mutex_unlock(&eeefsb_ec_mutex);
}

static void eeefsb_ec_gpio_set(int pin, int value) {
    unsigned short port;
    unsigned char mask;

    port = 0xFC20 + ((pin >> 3) & 0x1f);
    mask = 1 << (pin & 0x07);
    if (value) {
        eeefsb_ec_write(port, eeefsb_ec_read(port) | mask);
    } else {
        eeefsb_ec_write(port, eeefsb_ec_read(port) & ~mask);
    }
}

static int eeefsb_ec_gpio_get(int pin) {
    unsigned short port;
    unsigned char mask;
    unsigned char status;

    port = 0xfc20 + ((pin >> 3) & 0x1f);
    mask = 1 << (pin & 0x07);
    status = eeefsb_ec_read(port) & mask;

    return (status) ? 1 : 0;
}

/*** Fan and temperature functions *******************************************
 * ENE KB3310                                                                */
#define EC_ST00 0xF451          /* Temperature of CPU (C)                    */
#define EC_SC02 0xF463          /* Fan PWM duty cycle (%)                    */
#define EC_SC05 0xF466          /* High byte of fan speed (RPM)              */
#define EC_SC06 0xF467          /* Low byte of fan speed (RPM)               */
#define EC_SFB3 0xF4D3          /* Flag byte containing SF25 (FANctrl)       */

static unsigned int eeefsb_get_temperature(void) {
    return eeefsb_ec_read(EC_ST00);
}

static unsigned int eeefsb_fan_get_rpm(void) {
    return (eeefsb_ec_read(EC_SC05) << 8) | eeefsb_ec_read(EC_SC06);
}

/* Get fan control mode status                                                *
 * Returns 1 if fan is in manual mode, 0 if controlled by the EC             */
static int eeefsb_fan_get_manual(void) {
    return (eeefsb_ec_read(EC_SFB3) & 0x02) ? 1 : 0;
}

static void eeefsb_fan_set_control(int manual) {
    if (manual) {
        /* SF25=1: Prevent the EC from controlling the fan. */
        eeefsb_ec_write(EC_SFB3, eeefsb_ec_read(EC_SFB3) | 0x02);
    } else {
        /* SF25=0: Allow the EC to control the fan. */
        eeefsb_ec_write(EC_SFB3, eeefsb_ec_read(EC_SFB3) & ~0x02);
    }
}

static void eeefsb_fan_set_speed(unsigned int speed) {
    eeefsb_ec_write(EC_SC02, (speed > 100) ? 100 : speed);
}

static unsigned int eeefsb_fan_get_speed(void) {
    return eeefsb_ec_read(EC_SC02);
}


/*** Voltage functions *******************************************************
 * ICS9LPR426A                                                               */

#define EC_VOLTAGE_PIN 0x66
enum eeefsb_voltage { Low=0, High=1 };
static enum eeefsb_voltage eeefsb_get_voltage(void) {
    return eeefsb_ec_gpio_get(EC_VOLTAGE_PIN);
}
static void eeefsb_set_voltage(enum eeefsb_voltage voltage) {
    eeefsb_ec_gpio_set(EC_VOLTAGE_PIN, voltage);
}

/*** FSB functions ************************************************************
 * ICS9LPR426A                                                                *
 * cpuM and cpuN are CPU PLL VDO dividers                                     *
 * f_CPUVCO = 24 * N/M                                                        *
 * PCID is the PCI and PCI-E divisor                                          *
 * f_PCIVCO = 24 * N/M                                                        *
 */
static void eeefsb_get_freq(int *cpuM, int *cpuN, int *PCID) {
    *cpuM = eeefsb_pll_data[11] & 0xFF; // Byte 11: CPU M
    *cpuN = eeefsb_pll_data[12] & 0xFF; // Byte 12: CPU N
    *PCID = eeefsb_pll_data[15] & 0xFF; // Byte 15: PCI M
}

static void eeefsb_set_freq(int cpuM, int cpuN, int PCID) {
    int current_cpuM = 0, current_cpuN = 0, current_PCID = 0;
    eeefsb_get_freq(&current_cpuM, &current_cpuN, &current_PCID);
    if (current_cpuM != cpuM || current_cpuN != cpuN || current_PCID != PCID) {
        eeefsb_pll_data[11] = cpuM;
        eeefsb_pll_data[12] = cpuN;
        eeefsb_pll_data[15] = PCID;
        eeefsb_pll_write();
    }
}

/*** /proc file functions ****************************************************/

static struct proc_dir_entry *eeefsb_proc_rootdir;
#define EEEFSB_PROC_READFUNC(NAME) \
    void eeefsb_proc_readfunc_##NAME (char *buf, int buflen, int *bufpos)
#define EEEFSB_PROC_WRITEFUNC(NAME) \
    void eeefsb_proc_writefunc_##NAME (const char *buf, int buflen, int *bufpos)
#define EEEFSB_PROC_PRINTF(FMT, ARGS...) \
    *bufpos += snprintf(buf + *bufpos, buflen - *bufpos, FMT, ##ARGS)
#define EEEFSB_PROC_SCANF(COUNT, FMT, ARGS...) \
    do { \
        int len = 0; \
        int cnt = sscanf(buf + *bufpos, FMT "%n", ##ARGS, &len); \
        if (cnt < COUNT) { \
            printk(KERN_DEBUG "eeefsb:  scanf(\"%s\") wanted %d args, but got %d.\n", FMT, COUNT, cnt); \
            return; \
        } \
        *bufpos += len; \
    } while (0)
#define EEEFSB_PROC_MEMCPY(SRC, SRCLEN) \
    do { \
        int len = SRCLEN; \
        if (len > (buflen - *bufpos)) \
            len = buflen - *bufpos; \
        memcpy(buf + *bufpos, SRC, (SRCLEN > (buflen - *bufpos)) ? (buflen - *bufpos) : SRCLEN); \
        *bufpos += len; \
    } while (0)
#define EEEFSB_PROC_FILES_BEGIN \
    static struct eeefsb_proc_file eeefsb_proc_files[] = {
#define EEEFSB_PROC_RW(NAME, MODE) \
    { #NAME, MODE, &eeefsb_proc_readfunc_##NAME, &eeefsb_proc_writefunc_##NAME }
#define EEEFSB_PROC_RO(NAME, MODE) \
    { #NAME, MODE, &eeefsb_proc_readfunc_##NAME, NULL }
#define EEEFSB_PROC_FILES_END \
    { NULL, 0, NULL, NULL } };

struct eeefsb_proc_file {
    char *name;
    int mode;
    void (*readfunc)(char *buf, int buflen, int *bufpos);
    void (*writefunc)(const char *buf, int buflen, int *bufpos);
};


EEEFSB_PROC_READFUNC(bus_control) {
    int cpuM = 24;
    int cpuN = 15;
    int PCID = 143;
    int voltage = 0;

    eeefsb_get_freq(&cpuM, &cpuN, &PCID);
    voltage = (int)eeefsb_get_voltage();
    EEEFSB_PROC_PRINTF("%d %d %d %d\n", cpuM, cpuN, PCID, voltage);
}

EEEFSB_PROC_WRITEFUNC(bus_control) {
    /* Sensible defaults */
    int cpuM = 24;
    int cpuN = 15;
    int PCID = 143;
    int voltage = 0;

    EEEFSB_PROC_SCANF(4, "%i %i %i %i", &cpuM, &cpuN, &PCID, &voltage);
    eeefsb_set_freq(cpuM, cpuN, PCID);
    eeefsb_set_voltage(voltage);
}

EEEFSB_PROC_READFUNC(pll) {
    eeefsb_pll_read();
    EEEFSB_PROC_MEMCPY(eeefsb_pll_data, eeefsb_pll_datalen);
}

EEEFSB_PROC_READFUNC(fan_speed) {
    int speed = eeefsb_fan_get_speed();
    EEEFSB_PROC_PRINTF("%d\n", speed);
}

EEEFSB_PROC_WRITEFUNC(fan_speed) {
    unsigned int speed = 0;
    EEEFSB_PROC_SCANF(1, "%u", &speed);
    eeefsb_fan_set_speed(speed);
}

EEEFSB_PROC_READFUNC(fan_rpm) {
    int rpm = eeefsb_fan_get_rpm();
    EEEFSB_PROC_PRINTF("%d\n", rpm);
}

EEEFSB_PROC_READFUNC(fan_control) {
    EEEFSB_PROC_PRINTF("%d\n", eeefsb_fan_get_manual());
}

EEEFSB_PROC_WRITEFUNC(fan_control) {
    int manual = 0;
    EEEFSB_PROC_SCANF(1, "%i", &manual);
    eeefsb_fan_set_control(manual);
}

#if 0
9LPr426A_PROC_READFUNC(fan_mode) {
    enum eeefsb_fan_mode mode = eeefsb_fan_get_mode();
    switch (mode) {
        case Manual:    EEEFSB_PROC_PRINTF("manual\n");
                        break;
        case Automatic: EEEFSB_PROC_PRINTF("auto\n");
                        break;
        case Embedded:  EEEFSB_PROC_PRINTF("embedded\n");
                        break;
    }
}

EEEFSB_PROC_WRITEFUNC(fan_mode) {
    enum eeefsb_fan_mode mode = Automatic;
    char inputstr[16];
    EEEFSB_PROC_SCANF(1, "%15s", inputstr);
    if (strcmp(inputstr, "manual") == 0) {
        mode = Manual;
    } else if (strcmp(inputstr, "auto") == 0) {
        mode = Automatic;
    } else if (strcmp(inputstr, "embedded") == 0) {
        mode = Embedded;
    }
    eeefsb_fan_set_mode(mode);
}
#endif

EEEFSB_PROC_READFUNC(temperature) {
    unsigned int t = eeefsb_get_temperature();
    EEEFSB_PROC_PRINTF("%d\n", t);
}

EEEFSB_PROC_FILES_BEGIN
    EEEFSB_PROC_RW(bus_control,    0644),
    EEEFSB_PROC_RO(pll,            0400),
    EEEFSB_PROC_RW(fan_speed,      0644),
    EEEFSB_PROC_RO(fan_rpm,        0444),
    EEEFSB_PROC_RW(fan_control,    0644),
    EEEFSB_PROC_RO(temperature,    0444),
EEEFSB_PROC_FILES_END
    

int eeefsb_proc_readfunc(char *buffer, char **buffer_location, off_t offset,
                      int buffer_length, int *eof, void *data)
{
    struct eeefsb_proc_file *procfile = (struct eeefsb_proc_file *)data;
    int bufpos = 0;

    if (!procfile || !procfile->readfunc) {
        return -EIO;
    }

    *eof = 1;
    if (offset > 0) {
        return 0;
    }

    (*procfile->readfunc)(buffer, buffer_length, &bufpos);
    return bufpos;
}

int eeefsb_proc_writefunc(struct file *file, const char *buffer,
                       unsigned long count, void *data)
{
    char userdata[129];
    int bufpos = 0;
    struct eeefsb_proc_file *procfile = (struct eeefsb_proc_file *)data;

    if (!procfile || !procfile->writefunc) {
        return -EIO;
    }

    if (copy_from_user(userdata, buffer, (count > 128) ? 128 : count)) {
        printk(KERN_DEBUG "eeefsb: copy_from_user() failed\n");
        return -EIO;
    }
    userdata[128] = 0;      /* So that sscanf() doesn't overflow... */

    (*procfile->writefunc)(userdata, count, &bufpos);
    return count;
}

int eeefsb_proc_init(void) {
    int i;

    /* Create the /proc/eeefsb directory. */
    eeefsb_proc_rootdir = proc_mkdir("eeefsb", NULL);
    if (!eeefsb_proc_rootdir) {
        printk(KERN_ERR "eeefsb: Unable to create /proc/eeefsb\n");
        return false;
    }

    /* Create the individual proc files. */
    for (i=0; eeefsb_proc_files[i].name; i++) {
        struct proc_dir_entry *proc_file;
        struct eeefsb_proc_file *f = &eeefsb_proc_files[i];

        proc_file = create_proc_entry(f->name, f->mode, eeefsb_proc_rootdir);
        if (!proc_file) {
            printk(KERN_ERR "eeefsb: Unable to create /proc/eeefsb/%s", f->name);
            goto proc_init_cleanup;
        }
        proc_file->read_proc = &eeefsb_proc_readfunc;
        if (f->writefunc) {
            proc_file->write_proc = &eeefsb_proc_writefunc;
        }
        proc_file->data = f;
        proc_file->mode = S_IFREG | f->mode;
        proc_file->uid = 0;
        proc_file->gid = 0;
    }
    return true;

    /* We had an error, so cleanup all of the proc files... */
    proc_init_cleanup:
    for (; i >= 0; i--) {
        remove_proc_entry(eeefsb_proc_files[i].name, eeefsb_proc_rootdir);
    }
    remove_proc_entry("eeefsb", NULL);
    return false;
}

void eeefsb_proc_cleanup(void) {
    int i;
    for (i = 0; eeefsb_proc_files[i].name; i++) {
        remove_proc_entry(eeefsb_proc_files[i].name, eeefsb_proc_rootdir);
    }
    remove_proc_entry("eeefsb", NULL);
}



/*** Module initialization ***/
int init_module(void) {
    int retVal;
    
    retVal = eeefsb_pll_init();
    if (retVal) return retVal;
    eeefsb_proc_init();
    printk(KERN_NOTICE "Tools for eee PC overclocking, version %s\n", EEEFSB_VERSION);
    
    return 0;
}

/*** Module cleanup ***/
void cleanup_module(void) {
    eeefsb_pll_cleanup();
    eeefsb_proc_cleanup();
}


