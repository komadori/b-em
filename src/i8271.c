/*B-em v2.2 by Tom Walker
  8271 FDC emulation*/

#include <stdio.h>
#include <stdlib.h>
#include "b-em.h"
#include "6502.h"
#include "ddnoise.h"
#include "i8271.h"
#include "disc.h"
#include "led.h"
#include "model.h"

// Output Port bit definitions in i8271.drvout
#define SIDESEL   0x20
#define DRIVESEL0 0x40
#define DRIVESEL1 0x80
#define DRIVESEL (DRIVESEL0 | DRIVESEL1)

struct
{
        uint8_t command, params[5];
        int paramnum, paramreq;
        uint8_t status;
        uint8_t result;
        int curtrack[2], cursector;
        int sectorsleft;
        int bytesleft;
        uint8_t data;
        int phase;
        int written;

        uint8_t drvout;
    uint8_t unload_revs;
    uint32_t step_time;
    uint32_t settle_time;
    uint32_t load_time;
} i8271;

static void short_spindown(void)
{
    log_debug("i8271: short_spindown, motoron=%d", motoron);
    motorspin = 15000;
    fdc_time = 0;
}

static void i8271_NMI(void)
{
    if (i8271.status & 8)
        nmi = 1;
    else
        nmi = 0;
}

static void i8271_spinup(void)
{
    log_debug("i8271: spinup, motoron=%d", motoron);
    motorspin = 0;
    if (!motoron) {
        motoron = 1;
        led_update((curdrive == 0) ? LED_DRIVE_0 : LED_DRIVE_1, true, 0);
        ddnoise_spinup();
        for (int i = 0; i < NUM_DRIVES; i++)
            if (drives[i].spinup)
                drives[i].spinup(i);
    }
}

static void i8271_spindown()
{
    log_debug("i8271: spindown, motoron=%d", motoron);
    if (motoron) {
        motoron = 0;
        led_update(LED_DRIVE_0, false, 0);
        led_update(LED_DRIVE_1, false, 0);
        ddnoise_spindown();
        for (int i = 0; i < NUM_DRIVES; i++)
            if (drives[i].spindown)
                drives[i].spindown(i);
    }
    i8271.drvout &= ~DRIVESEL;
}

void i8271_setspindown(void)
{
    log_debug("i8271: set spindown");
    motorspin = i8271.unload_revs * 3125; /* into units of 2Mhz/128 */
    log_debug("i8271: set motorspin=%'d", motorspin);
}

int params[][2]=
{
        {0x35, 4}, {0x29, 1}, {0x2C, 0}, {0x3D, 1}, {0x3A, 2}, {0x13, 3}, {0x0B, 3}, {0x1B, 3}, {0x1F, 3}, {0x23, 5}, {-1, -1}
};

int i8271_getparams()
{
        int c = 0;
        while (params[c][0] != -1)
        {
                if (params[c][0] == i8271.command)
                   return params[c][1];
                c++;
        }
        return 0;
/*        printf("Unknown 8271 command %02X\n",i8271.command);
        dumpregs();
        exit(-1);*/
}

uint8_t i8271_read(uint16_t addr)
{
//        printf("Read 8271 %04X\n",addr);
        switch (addr & 7)
        {
            case 0: /*Status register*/
                //log_debug("i8271: Read status reg %04X %02X\n",pc,i8271.status);
                return i8271.status;
            case 1: /*Result register*/
                log_debug("i8271: Read result reg %04X %02X\n",pc,i8271.result);
                i8271.status &= ~0x18;
                i8271_NMI();
  //              output=1; timetolive=50;
                return i8271.result;
            case 4: /*Data register*/
                //log_debug("i8271: Read data reg %04X %02X\n",pc,i8271.data);
                i8271.status &= ~0xC;
                i8271_NMI();
//                printf("Read data reg %04X %02X\n",pc,i8271.status);
                return i8271.data;
        }
        return 0;
}

#define track0 (i8271.curtrack[curdrive] ? 0 : 2)

void i8271_seek()
{
    int new_track = i8271.params[0];
    if (new_track)
        disc_seekrelative(curdrive, new_track - i8271.curtrack[curdrive], i8271.step_time, i8271.settle_time);
    else
        disc_seek0(curdrive, i8271.step_time, i8271.settle_time);
}

static uint32_t time_to_2Mhz(unsigned value, unsigned multiplier, const char *name)
{
    uint32_t cycles = value * multiplier;
    log_info("i8271: %s set to %u native units, %'u 2Mhz cycles", name, value, cycles);
    return cycles;
}

void i8271_write(uint16_t addr, uint8_t val)
{
//        printf("Write 8271 %04X %02X\n",addr,val);
        switch (addr&7)
        {
            case 0: /*Command register*/
                if (i8271.status & 0x80) {
                   log_debug("i8271: command register written while busy");
                   return;
                }
                i8271.command = val & 0x3F;
                log_debug("i8271: command %02X", i8271.command);
                if (i8271.command == 0x17) i8271.command = 0x13;
                // Only commands < ReadDriveStatus actually change the drive select signals
                // We use this later to generate RDY in the ReadDriveStatus command
                if (i8271.command < 0x2C)
                {
                        i8271.drvout &= ~DRIVESEL;
                        i8271.drvout |= val & DRIVESEL;
                }
                curdrive = (val & 0x80) ? 1 : 0;
                if (motoron) {
                    led_update((curdrive == 0) ? LED_DRIVE_0 : LED_DRIVE_1, true, 0);
                    led_update((curdrive == 0) ? LED_DRIVE_1 : LED_DRIVE_0, false, 0);
                }
                i8271.paramnum = 0;
                i8271.paramreq = i8271_getparams();
                i8271.status = 0x80;
                if (!i8271.paramreq)
                {
                        switch (i8271.command)
                        {
                            case 0x2C: /*Read drive status*/
                                i8271.status = 0x10;
                                i8271.result = 0x80 | 8 | track0;
                                if (i8271.drvout & DRIVESEL0) i8271.result |= 0x04;
                                if (i8271.drvout & DRIVESEL1) i8271.result |= 0x40;
//                                printf("Status %02X\n",i8271.result);
                                break;

                            default:
                                i8271.result = 0x18;
                                i8271.status = 0x18;
                                i8271_NMI();
                                fdc_time = 0;
                                break;
//                                printf("Unknown 8271 command %02X 3\n",i8271.command);
//                                dumpregs();
//                                exit(-1);
                        }
                }
                break;
            case 1: /*Parameter register*/
                log_debug("i8271: parameter %02X", val);
                if (i8271.paramnum < 5)
                   i8271.params[i8271.paramnum++] = val;
                if (i8271.paramnum == i8271.paramreq)
                {
                        switch (i8271.command)
                        {
                            case 0x0B: /*Write sector*/
                                i8271.sectorsleft = i8271.params[2] & 31;
                                i8271.cursector = i8271.params[1];
                                i8271_spinup();
                                i8271.phase = 0;
                                if (i8271.curtrack[curdrive] != i8271.params[0]) i8271_seek();
                                else                                             fdc_time = 200;
                                break;
                            case 0x13: /*Read sector*/
                                i8271.sectorsleft = i8271.params[2] & 31;
                                i8271.bytesleft = 128 << ((i8271.params[2] >> 5) & 0x07);
                                i8271.cursector = i8271.params[1];
                                i8271_spinup();
                                i8271.phase = 0;
                                if (i8271.curtrack[curdrive] != i8271.params[0])
                                    i8271_seek();
                                fdc_time = 200;
                                break;
                            case 0x1F: /*Verify sector*/
                                i8271.sectorsleft = i8271.params[2] & 31;
                                i8271.bytesleft = 0;
                                i8271.cursector = i8271.params[1];
                                i8271_spinup();
                                i8271.phase = 0;
                                if (i8271.curtrack[curdrive] != i8271.params[0]) i8271_seek();
                                else                                             fdc_time = 200;
                                break;
                            case 0x1B: /*Read ID*/
//                                printf("8271 : Read ID start\n");
                                i8271.sectorsleft = i8271.params[2] & 31;
                                i8271.bytesleft = 4;
                                i8271_spinup();
                                i8271.phase = 0;
                                if (i8271.curtrack[curdrive] != i8271.params[0]) i8271_seek();
                                else                                             fdc_time = 200;
                                break;
                            case 0x23: /*Format track*/
                                i8271_spinup();
                                i8271.phase = 0;
                                if (i8271.curtrack[curdrive] != i8271.params[0]) i8271_seek();
                                else                                             fdc_time = 200;
                                break;
                                break;
                            case 0x29: /*Seek*/
//                                fdc_time=10000;
                                i8271_seek();
                                i8271_spinup();
                                break;
                            case 0x35: /*Specify*/
                                if (i8271.params[0] == 0x0d) {
                                    i8271.step_time   = time_to_2Mhz(i8271.params[1], 2000, "step time");
                                    i8271.settle_time = time_to_2Mhz(i8271.params[2], 2000, "head settle time");
                                    i8271.unload_revs = i8271.params[3] >> 4;
                                    log_debug("i8271: unload revs set to %d", i8271.unload_revs);
                                    i8271.load_time = time_to_2Mhz(i8271.params[3] & 0x0f, 4000, "head load time");
                                }
                                i8271.status = 0;
                                break;
                            case 0x3A: /*Write special register*/
                                i8271.status = 0;
//                                printf("Write special %02X\n",i8271.params[0]);
                                switch (i8271.params[0])
                                {
                                    case 0x12: i8271.curtrack[0] = val; /*printf("Write real track now %i\n",val);*/ break;
                                    case 0x17: break; /*Mode register*/
                                    case 0x1A: i8271.curtrack[1] = val; /*printf("Write real track now %i\n",val);*/ break;
                                    case 0x23: i8271.drvout = i8271.params[1]; break;
                                    default:
                                        i8271.result = 0x18;
                                        i8271.status = 0x18;
                                        i8271_NMI();
                                        fdc_time = 0;
                                        break;
//                                        default:
//                                        printf("8271 Write bad special register %02X\n",i8271.params[0]);
//                                        dumpregs();
//                                        exit(-1);
                                }
                                break;
                            case 0x3D: /*Read special register*/
                                i8271.status = 0x10;
                                i8271.result = 0;
                                switch (i8271.params[0])
                                {
                                        case 0x06: i8271.result = 0; break;
                                        case 0x12: i8271.result = i8271.curtrack[0]; break;
                                        case 0x1A: i8271.result = i8271.curtrack[1]; break;
                                        case 0x23: i8271.result = i8271.drvout; break;
                                        default:
                                        i8271.result = 0x18;
                                        i8271.status = 0x18;
                                        i8271_NMI();
                                        fdc_time = 0;
                                        break;
//                                        default:
//                                        printf("8271 Read bad special register %02X\n",i8271.params[0]);
//                                        dumpregs();
//                                        exit(-1);
                                }
                                break;

                            default:
                                i8271.result = 0x18;
                                i8271.status = 0x18;
                                i8271_NMI();
                                fdc_time = 0;
                                break;
//                                printf("Unknown 8271 command %02X 2\n",i8271.command);
//                                dumpregs();
//                                exit(-1);
                        }
                }
                break;
            case 2: /*Reset register*/
                log_debug("i8271: reset %02X", val);
                if (val & 1) i8271_reset();

                break;
            case 4: /*Data register*/
                i8271.data = val;
                i8271.written = 1;
                i8271.status &= ~0xC;
                i8271_NMI();
                break;
        }
}

static void i8271_callback(void)
{
        fdc_time = 0;
        log_debug("i8271: fdc_callback, cmd=%02X", i8271.command);
        switch (i8271.command)
        {
            case 0x0B: /*Write*/
                if (!i8271.phase)
                {
                        i8271.curtrack[curdrive] = i8271.params[0];
                        disc_writesector(curdrive, i8271.cursector, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                        i8271.phase = 1;

                        i8271.status = 0x8C;
                        i8271.result = 0;
                        i8271_NMI();
                        return;
                }
                i8271.sectorsleft--;
                if (!i8271.sectorsleft)
                {
                        i8271.status = 0x18;
                        i8271.result = 0;
                        i8271_NMI();
                        i8271_setspindown();
                        return;
                }
                i8271.cursector++;
                disc_writesector(curdrive, i8271.cursector, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                i8271.status = 0x8C;
                i8271.result = 0;
                i8271_NMI();
                break;

            case 0x13: /*Read*/
            case 0x1F: /*Verify*/
                if (!i8271.phase)
                {
//                        printf("Seek to %i\n",i8271.params[0]);
                        i8271.curtrack[curdrive] = i8271.params[0];
                        disc_readsector(curdrive, i8271.cursector, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                        i8271.phase = 1;
                        return;
                }
                i8271.sectorsleft--;
                if (!i8271.sectorsleft)
                {
                        i8271.status = 0x18;
                        i8271.result = 0;
                        i8271_NMI();
                        i8271_setspindown();
                        return;
                }
                i8271.cursector++;
                if (i8271.command != 0x1f)
                    i8271.bytesleft = 128 << ((i8271.params[2] >> 5) & 0x07);
                disc_readsector(curdrive, i8271.cursector, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                break;

            case 0x1B: /*Read ID*/
//                printf("Read ID callback %i\n",i8271.phase);
                if (!i8271.phase)
                {
                        i8271.curtrack[curdrive] = i8271.params[0];
                        disc_readaddress(curdrive, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                        i8271.phase = 1;
                        return;
                }
//                printf("Read ID track %i %i\n",i8271.params[0],i8271.sectorsleft);
                i8271.sectorsleft--;
                if (!i8271.sectorsleft)
                {
                        i8271.status = 0x18;
                        i8271.result = 0;
                        i8271_NMI();
//                        printf("8271 : ID read done!\n");
                        i8271_setspindown();
                        return;
                }
                i8271.cursector++;
                i8271.bytesleft = 4;
                disc_readaddress(curdrive, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, 0);
                break;

            case 0x23: /*Format*/
                if (!i8271.phase) {
                    log_debug("i8271: callback for format, initiate");
                    i8271.curtrack[curdrive] = i8271.params[0];
                    disc_format(curdrive, i8271.params[0], (i8271.drvout & SIDESEL) ? 1 : 0, i8271.params[2]);
                    i8271.status = 0x8C;
                    i8271.phase = 1;
                }
                else {
                    log_debug("i8271: callback for format, completed");
                    i8271.status = 0x18;
                    i8271.result = 0;
                }
                i8271_NMI();
                break;

            case 0x29: /*Seek*/
                i8271.curtrack[curdrive] = i8271.params[0];
                i8271.status = 0x18;
                i8271.result = 0;
                i8271_NMI();
                i8271_setspindown();
                break;

            case 0xFF: break;

            default: break;
                /* TODO: DB: check is this the intent?
                printf("Unknown 8271 command %02X 3\n", i8271.command);
                dumpregs();
                exit(-1);
                */
        }
}

static void i8271_data(uint8_t dat)
{
    if (i8271.bytesleft) {
        --i8271.bytesleft;
        i8271.data = dat;
        i8271.status = 0x8C;
        i8271.result = 0;
        i8271_NMI();
    }
}

static void i8271_finishread(bool deleted)
{
    log_debug("i8271: finishread, deleted=%d", deleted);
    fdc_time = 200;
    if (deleted)
        i8271.result |= 0x20;
}

static void i8271_notfound(void)
{
    log_debug("i8271: not found");
    i8271.result = 0x18;
    i8271.status = 0x18;
    i8271_NMI();
    short_spindown();
}

static void i8271_datacrcerror(bool deleted)
{
    log_debug("i8271: data CRC error");
    i8271.result = 0x0E;
    if (deleted)
        i8271.result |= 0x20;
    i8271.status = 0x18;
    i8271_NMI();
    short_spindown();
}

static void i8271_headercrcerror(void)
{
    log_debug("i8271: header CRC error");
    i8271.result = 0x0C;
    i8271.status = 0x18;
    i8271_NMI();
    short_spindown();
}

static void i8271_writeprotect(void)
{
    log_debug("i8271: write-protect");
    i8271.result = 0x12;
    i8271.status = 0x18;
    i8271_NMI();
    short_spindown();
}

int i8271_getdata(int last)
{
//        printf("Disc get data %i\n",bytenum);
        if (!i8271.written) return -1;
        if (!last)
        {
                i8271.status = 0x8C;
                i8271.result = 0;
                i8271_NMI();
        }
        i8271.written = 0;
        return i8271.data;
}

void i8271_reset()
{
    if (fdc_type == FDC_I8271) {
        fdc_callback       = i8271_callback;
        fdc_data           = i8271_data;
        fdc_spindown       = i8271_spindown;
        fdc_finishread     = i8271_finishread;
        fdc_notfound       = i8271_notfound;
        fdc_datacrcerror   = i8271_datacrcerror;
        fdc_headercrcerror = i8271_headercrcerror;
        fdc_writeprotect   = i8271_writeprotect;
        fdc_getdata        = i8271_getdata;
        motorspin = 45000;
        i8271.paramnum = i8271.paramreq = 0;
        i8271.status = 0;
        log_debug("i8271: reset 8271");
        fdc_time = 0;
        i8271.curtrack[0] = i8271.curtrack[1] = 0;
        i8271.command = 0xFF;
    }
}
