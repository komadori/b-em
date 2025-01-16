#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <zlib.h>

#define GROUP_SIZE  8
#define BLOCK_SIZE (GROUP_SIZE * 2)
#define OUT_SIZE (14+4*BLOCK_SIZE)

static const char xdigs[] = "0123456789ABCDEF";

static char *hex_group(const unsigned char *raw, unsigned bytes, char *out)
{
    if (bytes) {
        do {
            int ch = *raw++;
            *out++ = xdigs[(ch & 0xf0) >> 4];
            *out++ = xdigs[ch & 0x0f];
            out++;
        } while (--bytes);
    }
    return out;
}

static void rest_out(long offset, const unsigned char *raw, size_t bytes, char *out, size_t out_size)
{
    // Offset into data.

    char *outp = out + 7;
    while (outp >= out) {
        *outp-- = xdigs[offset & 0x0f];
        offset >>= 4;
    }

    // ASCII

    outp = out + 13 + BLOCK_SIZE*3;
    while (bytes--) {
        int ch = *raw++;
        if (ch < 0x20 || ch > 0x7e)
            ch = '.';
        *outp++ = ch;
    }

    // Output

    fwrite(out, out_size, 1, stdout);
}

static char *star_pad(size_t count, char *out)
{
    while (count--) {
        *out++ = '*';
        *out++ = '*';
        out++;
    }
    return out;
}

static void dump_hex(char *out, const char *fn, FILE *fp, size_t size)
{
    unsigned char raw[BLOCK_SIZE];

    // Whole blocks.

    long offset = 0;
    while (size >= BLOCK_SIZE) {
        if (fread(raw, BLOCK_SIZE, 1, fp) != 1) {
            fprintf(stderr, "snapdump: unexpected EOF on %s\n", fn);
            return;
        }
        char *outp = hex_group(raw, GROUP_SIZE, out + 11);
        hex_group(raw + GROUP_SIZE, GROUP_SIZE, outp+1);
        rest_out(offset, raw, BLOCK_SIZE, out, OUT_SIZE);
        offset += BLOCK_SIZE;
        size -= BLOCK_SIZE;
    }

    // Partial end block.

    if (size > 0) {
        if (fread(raw, size, 1, fp) != 1) {
            fprintf(stderr, "snapdump: unexpected EOF on %s\n", fn);
            return;
        }
        size_t out_size = 14 + 3*BLOCK_SIZE + size;
        out[out_size-1] = '\n';
        if (size >= GROUP_SIZE) {
            char *outp = hex_group(raw, GROUP_SIZE, out + 11);
            outp = hex_group(raw + GROUP_SIZE, size - GROUP_SIZE, outp+1);
            star_pad(BLOCK_SIZE - size, outp);
        }
        else {
            char *outp = hex_group(raw, size, out + 11);
            outp = star_pad(GROUP_SIZE - size, outp);
            star_pad(GROUP_SIZE, outp+1);
        }
        rest_out(offset, raw, size, out, out_size);
    }
}

typedef struct {
    z_stream zs;
    FILE *fp;
    size_t togo;
    int flush;
    unsigned char buf[BUFSIZ];
} ZFILE;

static void zinit(ZFILE *zfp, FILE *fp, long size)
{
    zfp->zs.zalloc = Z_NULL;
    zfp->zs.zfree = Z_NULL;
    zfp->zs.opaque = Z_NULL;
    inflateInit(&zfp->zs);
    zfp->zs.next_in = Z_NULL;
    zfp->zs.avail_in = 0;
    zfp->togo = size;
    zfp->fp = fp;
}

static int zread(ZFILE *zfp, void *dest, size_t size)
{
    int res;

    zfp->zs.next_out = dest;
    zfp->zs.avail_out = size;
    do {
        if (zfp->zs.avail_in == 0) {
            if (zfp->togo == 0)
                zfp->flush = Z_FINISH;
            else {
                zfp->flush = Z_NO_FLUSH;
                size_t chunk;
                if (zfp->togo > BUFSIZ)
                    chunk = BUFSIZ;
                else
                    chunk = zfp->togo;
                fprintf(stderr, "snapdump: reading %ld bytes from file\n", chunk);
                if (fread(zfp->buf, chunk, 1, zfp->fp) != 1)
                    break;
                zfp->zs.next_in = zfp->buf;
                zfp->zs.avail_in = chunk;
                zfp->togo -= chunk;
            }
        }
        fprintf(stderr, "snapdump: inflating, avail_in=%d, avail_out=%d\n", zfp->zs.avail_in, zfp->zs.avail_out);
        res = inflate(&zfp->zs, zfp->flush);
    } while (res == Z_OK && zfp->zs.avail_out > 0);

    if (res != Z_OK && res != Z_STREAM_END)
        fprintf(stderr, "snapdump: compression error: %d=%s\n", res, zfp->zs.msg);
    fprintf(stderr, "snapdump: return from zread, res=%d\n", res);
    return res;
}

unsigned load_var(FILE *fp)
{
    unsigned var, lshift;
    int      ch;

    var = lshift = 0;
    while ((ch = getc(fp)) != EOF) {
        if (ch & 0x80) {
            var |= ((ch & 0x7f) << lshift);
            break;
        }
        var |= ch << lshift;
        lshift += 7;
    }
    return var;
}

void print_vstr(const char *fn, FILE *fp, const char *prompt)
{
    size_t len = load_var(fp);
    if (len) {
        printf("  %-11s ", prompt);
        char buf[256];
        while (len > sizeof(buf)) {
            if (fread(buf, sizeof(buf), 1, fp) != 1) {
                fprintf(stderr, "snapdump: unexpected EOF on %s\n", fn);
                return;
            }
            fwrite(buf, sizeof(buf), 1, stdout);
            len -= sizeof(buf);
        }
        if (fread(buf, len, 1, fp) != 1) {
            fprintf(stderr, "snapdump: unexpected EOF on %s\n", fn);
            return;
        }
        buf[len++] = '\n';
        fwrite(buf, len, 1, stdout);
    }
    else
        printf("  %-11s <null>\n", prompt);
}

static void print_bool(int value, const char *label)
{
    printf("  %-11s %s\n", label, value ? "Yes" : "No");
}

static void dump_model(const char *fn, FILE *fp)
{
    printf("Model information\n  Model num:  %d\n", load_var(fp));
    print_vstr(fn, fp, "Model name:");
    print_vstr(fn, fp, "OS");
    print_vstr(fn, fp, "CMOS");
    print_vstr(fn, fp, "ROM setup");
    print_vstr(fn, fp, "FDC type");
    unsigned char bytes[7];
    fread(bytes, sizeof(bytes), 1, fp);
    print_bool(bytes[0] & 0x01, "65C02:");
    print_bool(bytes[0] & 0x80, "Integra:");
    print_bool(bytes[1], "B+:");
    print_bool(bytes[2], "Master:");
    print_bool(bytes[3], "Model A:");
    print_bool(bytes[4], "OS 0.1:");
    print_bool(bytes[5], "Compact:");
    if (bytes[6] == 1 || bytes[6] == 2)
        print_vstr(fn, fp, "Tube:");
}

static void dump_6502(const unsigned char *data)
{
    uint_least16_t pc = data[5] | (data[6] << 8);
    uint_least32_t cycles = data[9] | (data[10] << 8) | (data[11] << 16) | (data[12] << 24);
    unsigned flags = data[3];
    printf("6502 state:\n  PC=%04X A=%02X X=%02X Y=%02X S=%02X Flags=%c%c%c%c%c%c%c NMI=%d IRQ=%d cycles=%d\n",
           pc, data[0], data[1], data[2], data[4], flags & 0x80 ? 'N' : '-',
           flags & 0x40 ? 'V' : '-', flags & 0x10 ? 'B' : '-',
           flags & 0x08 ? 'D' : '-', flags & 0x04 ? 'I' : '-',
           flags & 0x02 ? 'Z' : '-', flags & 0x01 ? 'C' : '-',
           data[7], data[8], cycles);
}

static int dump_region(ZFILE *zfp, unsigned char *mem, uint_least16_t size, uint_least32_t offset, char *hexout)
{
    int res = zread(zfp, mem, size);
    if (res == Z_OK || res == Z_STREAM_END) {
        long bytes = size - zfp->zs.avail_out;
        while (bytes >= BLOCK_SIZE) {
            char *outp = hex_group(mem, GROUP_SIZE, hexout + 11);
            hex_group(mem + GROUP_SIZE, GROUP_SIZE, outp+1);
            rest_out(offset, mem, BLOCK_SIZE, hexout, OUT_SIZE);
            mem += BLOCK_SIZE;
            offset += BLOCK_SIZE;
            bytes -= BLOCK_SIZE;
        }
        if (bytes > 0) {
            size_t out_size = 14 + 3*BLOCK_SIZE + bytes;
            hexout[out_size-1] = '\n';
            if (bytes >= GROUP_SIZE) {
                char *outp = hex_group(mem, GROUP_SIZE, hexout + 11);
                outp = hex_group(mem + GROUP_SIZE, bytes - GROUP_SIZE, outp+1);
                star_pad(BLOCK_SIZE - bytes, outp);
            }
            else {
                char *outp = hex_group(mem, bytes, hexout + 11);
                outp = star_pad(GROUP_SIZE - bytes, outp);
                star_pad(GROUP_SIZE, outp+1);
            }
            rest_out(offset, mem, bytes, hexout, out_size);
        }
    }
    return res;
}

static void dump_iomem(char *hexout, const char *fn, FILE *fp, long size)
{
    ZFILE zf;
    unsigned char mem[0x8000];
    memset(mem, 0, sizeof(mem));
    zinit(&zf, fp, size);
    int res = zread(&zf, mem, 2);
    if (res == Z_OK) {
        printf("I/O Processor memory:\n"
               "  FE30 (ROMSEL)=%02X, FE34 (ACCCON)=%02X\n  Main RAM:\n", mem[0], mem[1]);
        dump_region(&zf, mem, 0x8000, 0xffff0000, hexout);
        fputs("  VDU workspace\n", stdout);
        dump_region(&zf, mem, 0x1000, 0xff808000, hexout);
        fputs("  Hazel Workspace\n", stdout);
        dump_region(&zf, mem, 0x2000, 0xff80c000, hexout);
        fputs("  Shdadow RAM\n", stdout);
        dump_region(&zf, mem, 0x5000, 0xfffd3000, hexout);
        for (int r = 0; r <= 15; ++r) {
            printf("  ROM %d\n", r);
            dump_region(&zf, mem, 0x4000, 0xfff08000|(r<<16), hexout);
        }
    }
    inflateEnd(&zf.zs);
}

static void dump_compressed(char *hexout, const char *fn, FILE *fp, long size)
{
    fprintf(stderr, "snapdump: dump_compressed, size=%ld\n", size);
    ZFILE zf;
    unsigned char mem[0x8000];
    zinit(&zf, fp, size);
    uint_least32_t offset = 0;
    int res;
    while ((res = dump_region(&zf, mem, sizeof(mem), offset, hexout)) == Z_OK)
        offset += sizeof(mem) - zf.zs.avail_out;
    inflateEnd(&zf.zs);
}

static void dump_via(const unsigned char *data)
{
    uint_least32_t t1l = data[13] | (data[14] << 8) | (data[15] << 15) | (data[16] << 24);
    uint_least32_t t2l = data[17] | (data[18] << 8) | (data[19] << 15) | (data[20] << 24);
    uint_least32_t t1c = data[21] | (data[22] << 8) | (data[23] << 15) | (data[24] << 24);
    uint_least32_t t2c = data[25] | (data[26] << 8) | (data[27] << 15) | (data[28] << 24);
    printf("  ORA=%02X IRA=%02X INA=%02X DDRA=%02X\n"
           "  ORB=%02X IRB=%02X INB=%02X DDRB=%02X\n"
           "  SR=%02X ACR=%02X PCR=%02X IFR=%02X IER=%02X\n"
           "  T1L=%04X (%d) T1C=%04X (%d)\n"
           "  T2L=%04X (%d) T2C=%04X (%d)\n"
           "  t1hit=%d t2hit=%d ca1=%d ca2=%d\n",
           data[0], data[2], data[4], data[6], data[1], data[3], data[5],
           data[7], data[8], data[9], data[10], data[11], data[12],
           t1l, t1l, t1c, t1c, t2l, t2l, t2c, t2c,
           data[29], data[30], data[31], data[32]);
}

static void dump_sysvia(const unsigned char *data)
{
    fputs("System VIA state:\n", stdout);
    dump_via(data);
    printf("  IC32=%02X\n", data[33]);
}

static void dump_uservia(const unsigned char *data)
{
    fputs("User VIA state:\n", stdout);
    dump_via(data);
}

static const char *nula_names[] = {
    "palette mode",
    "horizontal offset",
    "left blank",
    "disable",
    "attribute mode",
    "attribute text"
};

static void dump_vula(const unsigned char *data)
{
    printf("Video ULA state:\n  ULA CTRL=%02X\n  Original Palette:\n", data[0]);
    for (int c = 0; c < 4; ++c) {
        unsigned v1 = data[c+1];
        unsigned v2 = data[c+5];
        unsigned v3 = data[c+9];
        unsigned v4 = data[c+13];
        printf("    %2d: %02X (%3d)  %2d: %02X (%3d)  %2d: %02X (%3d)  %2d: %02X (%3d)\n", c, v1, v1, c+4, v2, v2, c+8, v3, v3, c+12, v4, v4);
    }
    fputs("  NuLA palette (RGBA):\n", stdout);
    const unsigned char *ptr = data+14;
    for (int c= 0; c < 16; ++c) {
        uint_least32_t v = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
        printf("    %2d: %08X %4d,%4d,%4d,%4d\n", c, v, ptr[0], ptr[1], ptr[2], ptr[3]);
        ptr += 4;
    }
    printf("  NuLA palette write flag: %02X\n  NuLA palette first byte: %02X\n  NuLA flash:", ptr[0], ptr[1]);
    ptr += 2;
    for (int c = 0; c < 8; ++c)
        printf(" %02X", *ptr++);
    putchar('\n');
    for (int c = 0; c < 6; ++c) {
        unsigned v = *ptr++;
        printf("  NuLA %s: %02X (%u)\n", nula_names[c], v, v);
    }
}

static void dump_crtc(const unsigned char *data)
{
    fputs("CRTC state:\n  ", stdout);
    for (int c = 0; c <= 5; ++c) {
        unsigned v = *data++;
        printf("R%d=%02X (%3d) ", c, v, v);
    }
    fputs("\n  ", stdout);
    for (int c = 6; c <= 11; ++c) {
        unsigned v = *data++;
        printf("R%d=%02X (%3d) ", c, v, v);
    }
    unsigned r12 = data[0] | (data[1] << 8);
    data += 2;
    unsigned r14 = data[0] | (data[1] << 8);
    data += 2;
    unsigned r16 = data[0] | (data[1] << 8);
    data += 2;
    uint_least8_t vc = data[0];
    uint_least8_t sc = data[1];
    uint_least8_t hc = data[2];
    uint_least16_t ma = data[3] | (data[4] << 8);
    uint_least16_t mab = data[5] | (data[6] << 8);
    printf("\n  R12/13=%04X (%d)  R14/15=%04X (%d)  R16/17=%04X (%d)\n"
           "  VC=%02X (%d)  SC=%02X (%d)  HC=%02X (%d)\n"
           "  MA=%04X (%d)  MABACK=%04X (%d)\n",
           r12, r12, r14, r14, r16, r16, vc, vc, sc, sc, hc, hc, ma, ma, mab, mab);

}

static void dump_video(const unsigned char *data)
{
    uint_least16_t scrx = data[0] | (data[1] << 8);
    uint_least16_t scry = data[2] | (data[3] << 8);
    uint_least32_t vidclk = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
    printf("Other video state:\n"
           "  scrx=%04X (%d), scry=%04X (%d), oddclock=%d, vidclocks=%d\n",
           scrx, scrx, scry, scry, data[4], vidclk);
}

static void dump_sn76489(const unsigned char *data)
{
    fputs("Sound chip state:\n  Melodic channels:\n", stdout);
    uint32_t *latch = (uint32_t *)data;
    uint32_t *count = (uint32_t *)(data + 16);
    uint32_t *cstat = (uint32_t *)(data + 32);
    const unsigned char *vptr = data + 48;
    for (int c = 0; c < 4; ++c) {
        uint32_t lv = latch[c];
        uint32_t cv = count[c];
        uint32_t sv = cstat[c];
        uint_least8_t vol = vptr[c];
        printf("    Ch %d: latch %04X (%5d) count %04X (%5d) stat %04X (%5d) vol %02X (%3d)\n", c, lv, lv, cv, cv, sv, sv, vol, vol);
    }
    vptr += 4;
    uint_least8_t noise = vptr[0];
    uint_least16_t nshift = vptr[1] | (vptr[2] << 8);
    printf("  Noise channel:\n    Noise %02X (%3d)  shift %04X (%5d)\n", noise, noise, nshift, nshift);
}

static void dump_adc(const unsigned char *data)
{
    uint_least16_t value = data[1] | (data[2] << 8);
    uint_least8_t atime = data[4];
    printf("ADC state:\n  status=%02X value=%04X (%5d) latch=%02X time=%02X (%3d)\n", data[0], value, value, data[3], atime, atime);
}

static void dump_acia(const unsigned char *data)
{
    printf("ACIA state:\n  control register=%02X, status_register=%02X\n", data[0], data[1]);
}

static void dump_serial_ula(const unsigned char *data)
{
    printf("Serial ULA state:\n  register=%02X\n", data[0]);
}

static void print_dir(const char *fn, FILE *fp, const char *desc)
{
    int flag = getc(fp);
    if (flag == 'N')
        printf("  %s <undefined>\n", desc);
    else if (flag == 'R')
        printf("  %s <root>\n", desc);
    else if (flag == 'S')
        print_vstr(fn, fp, desc);
    else
        printf("  %s <invalid>\n", desc);
}

static void dump_vdfs(const char *fn, FILE *fp)
{
    fputs("VDFS state:\n  Enabled: ", stdout);
    int ch = getc(fp);
    if (ch == 'V') {
        fputs("yes\n  Directories:\n", stdout);
        print_dir(fn, fp, "  Current:");
        print_dir(fn, fp, "  Library:");
        print_dir(fn, fp, "  Previous:");
        print_dir(fn, fp, "  Catalog: ");
    }
    else if (ch == 'v')
        fputs("No\n", stdout);
    else
        fputs("invalid\n", stdout);
}

static void small_section(const char *fn, FILE *fp, size_t size, void (*func)(const unsigned char *data))
{
    unsigned char data[256];
    if (size > sizeof(data))
        fprintf(stderr, "snapdump: in %s, section of %ld bytes too big\n", fn, size);
    else if (fread(data, size, 1, fp) == 1)
        func(data);
    else
        fprintf(stderr, "snapdump: unexpected EOF on %s\n", fn);
}

static void dump_one(char *hexout, const char *fn, FILE *fp)
{
    printf("Version 1 dump, model = %d\n", getc(fp));
    small_section(fn, fp, 13, dump_6502);
    puts("I/O processor memory");
    dump_iomem(hexout, fn, fp, 327682);
    small_section(fn, fp, 34, dump_sysvia);
    small_section(fn, fp, 33, dump_uservia);
    small_section(fn, fp, 97, dump_vula);
    small_section(fn, fp, 25, dump_crtc);
    small_section(fn, fp, 9, dump_video);
    small_section(fn, fp, 55, dump_sn76489);
    small_section(fn, fp, 5, dump_adc);
    small_section(fn, fp, 2, dump_acia);
    small_section(fn, fp, 1, dump_serial_ula);
/*
    vdfs_loadstate(fp);
    music5000_loadstate(fp);}*/
}

static void dump_section(char *hexout, const char *fn, FILE *fp, int key, long size)
{
    long start = ftell(fp);
    switch(key) {
        case 'm':
            dump_model(fn, fp);
            break;
        case '6':
            small_section(fn, fp, size, dump_6502);
            break;
        case 'M':
            dump_iomem(hexout, fn, fp, size);
            break;
        case 'S':
            small_section(fn, fp, size, dump_sysvia);
            break;
        case 'U':
            small_section(fn, fp, size, dump_uservia);
            break;
        case 'V':
            small_section(fn, fp, size, dump_vula);
            break;
        case 'C':
            small_section(fn, fp, size, dump_crtc);
            break;
        case 'v':
            small_section(fn, fp, size, dump_video);
            break;
        case 's':
            small_section(fn, fp, 55, dump_sn76489);
            break;
        case 'A':
            small_section(fn, fp, 5, dump_adc);
            break;
        case 'a':
            small_section(fn, fp, 2, dump_acia);
            break;
        case 'r':
            small_section(fn, fp, 1, dump_serial_ula);
            break;
        case 'F':
            dump_vdfs(fn, fp);
            break;
        case '5':
            fputs("Music 5000\n", stdout);
            dump_hex(hexout, fn, fp, size);
            break;
        case 'T':
            fputs("Tube ULA\n", stdout);
            dump_hex(hexout, fn, fp, size);
            break;
        case 'P':
            fputs("Tube Processor\n", stdout);
            dump_compressed(hexout, fn, fp, size);
            break;
        case 'p':
            fputs("Paula Sound\n", stdout);
            dump_hex(hexout, fn, fp, size);
            break;
        case 'J':
            fputs("JIM memory\n", stdout);
            dump_compressed(hexout, fn, fp, size);
    }
    fseek(fp, start+size, SEEK_SET);
}

static void dump_two(char *hexout, const char *fn, FILE *fp)
{
    unsigned char hdr[4];

    while (fread(hdr, sizeof hdr, 1, fp) == 1) {
        long size = hdr[1] | (hdr[2] << 8) | (hdr[3] << 16);
        dump_section(hexout, fn, fp, hdr[0], size);
    }
}

static void dump_three(char *hexout, const char *fn, FILE *fp)
{
    unsigned char hdr[3];

    while (fread(hdr, sizeof hdr, 1, fp) == 1) {
        int key = hdr[0];
        long size = hdr[1] | (hdr[2] << 8);
        if (key & 0x80) {
            if (fread(hdr, 2, 1, fp) != 1) {
                fprintf(stderr, "snapdump: unexpected EOF on file %s", fn);
                return;
            }
            size |= (hdr[0] << 16) | (hdr[1] << 24);
            key &= 0x7f;
        }
        dump_section(hexout, fn, fp, key, size);
    }
}

static bool snapdump(char *hexout, const char *fn)
{
    FILE *fp = fopen(fn, "rb");
    if (fp) {
        char magic[8];
        if (fread(magic, 8, 1, fp) == 1 && memcmp(magic, "BEMSNAP", 7) == 0) {
            switch(magic[7]) {
                case '1':
                    dump_one(hexout, fn, fp);
                    break;
                case '2':
                    dump_two(hexout, fn, fp);
                    break;
                case '3':
                    dump_three(hexout, fn, fp);
                    break;
                default:
                    fprintf(stderr, "snapdump: file %s: unrecognised B-Em snapshot file version %c\n", fn, magic[7]);
            }
        }
        else
            fprintf(stderr, "snapdump: file %s is not a B-Em snapshot file", fn);
        fclose(fp);
        return true;
    }
    fprintf(stderr, "snapdump: unable to open file %s for reading: %s\n", fn, strerror(errno));
    return false;
}

int main(int argc, char **argv)
{
    if (--argc) {
        int status = 1;
        char hexout[OUT_SIZE];
        memset(hexout, ' ', OUT_SIZE);
        hexout[9] = '-';
        hexout[OUT_SIZE-1] = '\n';

        while (argc--) {
            if (!snapdump(hexout, *++argv))
                status = 2;
        }
        return status;
    }
    else {
        fputs("Usage: snapdump [file] ...\n", stderr);
        return 1;
    }
}
