/*****************************************************************************/
/*
 * NTSC/CRT - integer-only NTSC video signal encoding / decoding emulation
 * 
 *   by EMMIR 2018-2023
 *   
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/
#include "crt.h"

#include <stdlib.h>
#include <string.h>

/*
 *                      FULL HORIZONTAL LINE SIGNAL (~63500 ns)
 * |---------------------------------------------------------------------------|
 *   HBLANK (~10900 ns)                 ACTIVE VIDEO (~52600 ns)
 * |-------------------||------------------------------------------------------|
 *   
 *   
 *   WITHIN HBLANK PERIOD:
 *   
 *   FP (~1500 ns)  SYNC (~4700 ns)  BW (~600 ns)  CB (~2500 ns)  BP (~1600 ns)
 * |--------------||---------------||------------||-------------||-------------|
 *      BLANK            SYNC           BLANK          BLANK          BLANK
 * 
 */
#define LINE_BEG         0
#define FP_ns            1500      /* front porch */
#define SYNC_ns          4700      /* sync tip */
#define BW_ns            600       /* breezeway */
#define CB_ns            2500      /* color burst */
#define BP_ns            1600      /* back porch */
#define AV_ns            52600     /* active video */
#define HB_ns            (FP_ns + SYNC_ns + BW_ns + CB_ns + BP_ns) /* h blank */
/* line duration should be ~63500 ns */
#define LINE_ns          (FP_ns + SYNC_ns + BW_ns + CB_ns + BP_ns + AV_ns)

/* convert nanosecond offset to its corresponding point on the sampled line */
#define ns2pos(ns)       ((ns) * CRT_HRES / LINE_ns)
/* starting points for all the different pulses */
#define FP_BEG           ns2pos(0)
#define SYNC_BEG         ns2pos(FP_ns)
#define BW_BEG           ns2pos(FP_ns + SYNC_ns)
#define CB_BEG           ns2pos(FP_ns + SYNC_ns + BW_ns)
#define BP_BEG           ns2pos(FP_ns + SYNC_ns + BW_ns + CB_ns)
#define AV_BEG           ns2pos(HB_ns)
#define AV_LEN           ns2pos(AV_ns)

/* somewhere between 7 and 12 cycles */
#define CB_CYCLES   10

/* frequencies for bandlimiting */
#define L_FREQ           1431818 /* full line */
#define Y_FREQ           420000  /* Luma   (Y) 4.2  MHz of the 14.31818 MHz */
#define I_FREQ           150000  /* Chroma (I) 1.5  MHz of the 14.31818 MHz */
#define Q_FREQ           55000   /* Chroma (Q) 0.55 MHz of the 14.31818 MHz */

/* IRE units (100 = 1.0V, -40 = 0.0V) */
#define WHITE_LEVEL      100
#define BURST_LEVEL      20
#define BLACK_LEVEL      7
#define BLANK_LEVEL      0
#define SYNC_LEVEL      -40

/* 227.5 subcarrier cycles per line means every other line has reversed phase */
#define CC_PHASE(ln)     (((ln) & 1) ? -1 : 1)
/* ensure negative values for x get properly modulo'd */
#define POSMOD(x, n)     (((x) % (n) + (n)) % (n))

/*****************************************************************************/
/***************************** FIXED POINT MATH ******************************/
/*****************************************************************************/

#define T14_2PI           16384
#define T14_MASK          (T14_2PI - 1)
#define T14_PI            (T14_2PI / 2)

static int sigpsin15[18] = { /* significant points on sine wave (15-bit) */
    0x0000,
    0x0c88,0x18f8,0x2528,0x30f8,0x3c50,0x4718,0x5130,0x5a80,
    0x62f0,0x6a68,0x70e0,0x7640,0x7a78,0x7d88,0x7f60,0x8000,
    0x7f60
};

static int
sintabil8(int n)
{
    int f, i, a, b;
    
    /* looks scary but if you don't change T14_2PI
     * it won't cause out of bounds memory reads
     */
    f = n >> 0 & 0xff;
    i = n >> 8 & 0xff;
    a = sigpsin15[i];
    b = sigpsin15[i + 1];
    return (a + ((b - a) * f >> 8));
}

/* 14-bit interpolated sine/cosine */
static void
sincos14(int *s, int *c, int n)
{
    int h;
    
    n &= T14_MASK;
    h = n & ((T14_2PI >> 1) - 1);
    
    if (h > ((T14_2PI >> 2) - 1)) {
        *c = -sintabil8(h - (T14_2PI >> 2));
        *s = sintabil8((T14_2PI >> 1) - h);
    } else {
        *c = sintabil8((T14_2PI >> 2) - h);
        *s = sintabil8(h);
    }
    if (n > ((T14_2PI >> 1) - 1)) {
        *c = -*c;
        *s = -*s;
    }
}

#define EXP_P         11
#define EXP_ONE       (1 << EXP_P)
#define EXP_MASK      (EXP_ONE - 1)
#define EXP_PI        6434
#define EXP_MUL(x, y) (((x) * (y)) >> EXP_P)
#define EXP_DIV(x, y) (((x) << EXP_P) / (y))

static int e11[] = {
    EXP_ONE,
    5567,  /* e   */
    15133, /* e^2 */
    41135, /* e^3 */
    111817 /* e^4 */
}; 

/* fixed point e^x */
static int
expx(int n)
{
    int neg, idx, res;
    int nxt, acc, del;
    int i;

    if (n == 0) {
        return EXP_ONE;
    }
    neg = n < 0;
    if (neg) {
        n = -n;
    }
    idx = n >> EXP_P;
    res = EXP_ONE;
    for (i = 0; i < idx / 4; i++) {
        res = EXP_MUL(res, e11[4]);
    }
    idx &= 3;
    if (idx > 0) {
        res = EXP_MUL(res, e11[idx]);
    }
    
    n &= EXP_MASK;
    nxt = EXP_ONE;
    acc = 0;
    del = 1;
    for (i = 1; i < 17; i++) {
        acc += nxt / del;
        nxt = EXP_MUL(nxt, n);
        del *= i;
        if (del > nxt || nxt <= 0 || del <= 0) {
            break;
        }
    }
    res = EXP_MUL(res, acc);

    if (neg) {
        res = EXP_DIV(EXP_ONE, res);
    }
    return res;
}

/*****************************************************************************/
/********************************* FILTERS ***********************************/
/*****************************************************************************/

#define HISTLEN     3
#define HISTOLD     (HISTLEN - 1) /* oldest entry */
#define HISTNEW     0             /* newest entry */

#define EQ_P        16 /* if changed, the gains will need to be adjusted */
#define EQ_R        (1 << (EQ_P - 1)) /* rounding */
/* three band equalizer */
static struct EQF {
    int lf, hf; /* fractions */
    int g[3]; /* gains */
    int fL[4];
    int fH[4];
    int h[HISTLEN]; /* history */
} eqY, eqI, eqQ;

/* f_lo - low cutoff frequency
 * f_hi - high cutoff frequency
 * rate - sampling rate
 * g_lo, g_mid, g_hi - gains
 */
static void
init_eq(struct EQF *f,
        int f_lo, int f_hi, int rate,
        int g_lo, int g_mid, int g_hi)
{
    int sn, cs;
    
    memset(f, 0, sizeof(struct EQF));
        
    f->g[0] = g_lo;
    f->g[1] = g_mid;
    f->g[2] = g_hi;
    
    sincos14(&sn, &cs, T14_PI * f_lo / rate);
    if (EQ_P >= 15) {
        f->lf = 2 * (sn << (EQ_P - 15));
    } else {
        f->lf = 2 * (sn >> (15 - EQ_P));
    }
    sincos14(&sn, &cs, T14_PI * f_hi / rate);
    if (EQ_P >= 15) {
        f->hf = 2 * (sn << (EQ_P - 15));
    } else {
        f->hf = 2 * (sn >> (15 - EQ_P));
    }
}

static void
reset_eq(struct EQF *f)
{
    memset(f->fL, 0, sizeof(f->fL));
    memset(f->fH, 0, sizeof(f->fH));
    memset(f->h, 0, sizeof(f->h));
}

static int
eqf(struct EQF *f, int s)
{    
    int i, r[3];

    f->fL[0] += (f->lf * (s - f->fL[0]) + EQ_R) >> EQ_P;
    f->fH[0] += (f->hf * (s - f->fH[0]) + EQ_R) >> EQ_P;
    
    for (i = 1; i < 4; i++) {
        f->fL[i] += (f->lf * (f->fL[i - 1] - f->fL[i]) + EQ_R) >> EQ_P;
        f->fH[i] += (f->hf * (f->fH[i - 1] - f->fH[i]) + EQ_R) >> EQ_P;
    }
    
    r[0] = f->fL[3];
    r[1] = f->fH[3] - f->fL[3];
    r[2] = f->h[HISTOLD] - f->fH[3];

    for (i = 0; i < 3; i++) {
        r[i] = (r[i] * f->g[i]) >> EQ_P;
    }
  
    for (i = HISTOLD; i > 0; i--) {
        f->h[i] = f->h[i - 1];
    }
    f->h[HISTNEW] = s;
    
    return (r[0] + r[1] + r[2]);
}

/* infinite impulse response low pass filter for bandlimiting YIQ */
static struct IIRLP {
    int c;
    int h; /* history */
} iirY, iirI, iirQ;

/* freq  - total bandwidth
 * limit - max frequency
 */
static void
init_iir(struct IIRLP *f, int freq, int limit)
{
    int rate; /* cycles/pixel rate */
    
    memset(f, 0, sizeof(struct IIRLP));
    rate = (freq << 9) / limit;
    f->c = EXP_ONE - expx(-((EXP_PI << 9) / rate));
}

static void
reset_iir(struct IIRLP *f)
{
    f->h = 0;
}

/* hi-pass for debugging */
#define HIPASS 0

static int
iirf(struct IIRLP *f, int s)
{
    f->h += EXP_MUL(s - f->h, f->c);
#if HIPASS
    return s - f->h;
#else
    return f->h;
#endif
}

/*****************************************************************************/
/***************************** PUBLIC FUNCTIONS ******************************/
/*****************************************************************************/

extern void
crt_resize(struct CRT *v, int w, int h, int *out)
{    
    v->outw = w;
    v->outh = h;
    v->out = out;
}

extern void
crt_reset(struct CRT *v)
{
    v->saturation = 18;
    v->brightness = 0;
    v->contrast = 179;
    v->black_point = 0;
    v->white_point = 100;
    v->hsync = 0;
    v->vsync = 0;
}

extern void
crt_init(struct CRT *v, int w, int h, int *out)
{
    memset(v, 0, sizeof(struct CRT));
    crt_resize(v, w, h, out);
    crt_reset(v);
            
    /* kilohertz to line sample conversion */
#define kHz2L(kHz) (CRT_HRES * (kHz * 100) / L_FREQ)
    
    /* band gains are pre-scaled as 16-bit fixed point
     * if you change the EQ_P define, you'll need to update these gains too
     */
    init_eq(&eqY, kHz2L(1500), kHz2L(3000), CRT_HRES, 65536, 8192, 9175);
    init_eq(&eqI, kHz2L(80),   kHz2L(1150), CRT_HRES, 65536, 65536, 1311);
    init_eq(&eqQ, kHz2L(80),   kHz2L(1000), CRT_HRES, 65536, 65536, 0);
    
    init_iir(&iirY, L_FREQ, Y_FREQ);
    init_iir(&iirI, L_FREQ, I_FREQ);
    init_iir(&iirQ, L_FREQ, Q_FREQ);
}

extern void
crt_2ntsc(struct CRT *v, struct NTSC_SETTINGS *s)
{
    int cc[4] = { 0, 1, 0, -1 }; /* color carrier phase */
    int destw = (AV_LEN * 55500) >> 16;
    int desth = (CRT_LINES * 63500) >> 16;
    int x, y, xo, yo;
    int n;
    
    xo = AV_BEG  + 4 + (AV_LEN    - destw) / 2;
    yo = CRT_TOP + 4 + (CRT_LINES - desth) / 2;
    
    s->field &= 1;
    
    /* align signal */
    xo = (xo & ~3);
    
    for (n = 0; n < CRT_VRES; n++) {
        int t; /* time */
        signed char *line = &v->analog[n * CRT_HRES];
        
        t = LINE_BEG;

        if (n <= 3 || (n >= 7 && n <= 9)) {
            /* equalizing pulses - small blips of sync, mostly blank */
            while (t < (4   * CRT_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (50  * CRT_HRES / 100)) line[t++] = BLANK_LEVEL;
            while (t < (54  * CRT_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (100 * CRT_HRES / 100)) line[t++] = BLANK_LEVEL;
        } else if (n >= 4 && n <= 6) {
            int even[4] = { 46, 50, 96, 100 };
            int odd[4] =  { 4, 50, 96, 100 };
            int *offs = even;
            if (s->field == 1) {
                offs = odd;
            }
            /* vertical sync pulse - small blips of blank, mostly sync */
            while (t < (offs[0] * CRT_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (offs[1] * CRT_HRES / 100)) line[t++] = BLANK_LEVEL;
            while (t < (offs[2] * CRT_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (offs[3] * CRT_HRES / 100)) line[t++] = BLANK_LEVEL;
        } else {
            /* video line */
            while (t < SYNC_BEG) line[t++] = BLANK_LEVEL; /* FP */
            while (t < BW_BEG)   line[t++] = SYNC_LEVEL;  /* SYNC */
            while (t < AV_BEG)   line[t++] = BLANK_LEVEL; /* BW + CB + BP */
            if (n < CRT_TOP) {
                while (t < CRT_HRES) line[t++] = BLANK_LEVEL;
            }
            if (s->as_color) {
                /* CB_CYCLES of color burst at 3.579545 Mhz */
                for (t = CB_BEG; t < CB_BEG + (CB_CYCLES * CRT_CB_FREQ); t++) {
                    line[t] = BLANK_LEVEL + (cc[(t + 0) & 3] * BURST_LEVEL);
                }
            }
        }
    }

    for (y = 0; y < desth; y++) {
        int field_offset;
        int syA, syB;
        
        field_offset = (s->field * s->h + desth) / desth / 2;
        syA = (y * s->h) / desth;
        syB = (y * s->h + desth / 2) / desth;
    
        syA += field_offset;
        syB += field_offset;

        if (syA >= s->h) syA = s->h;
        if (syB >= s->h) syB = s->h;
        
        syA *= s->w;
        syB *= s->w;
        
        reset_iir(&iirY);
        reset_iir(&iirI);
        reset_iir(&iirQ);
        
        for (x = 0; x < destw; x++) {
            int fy, fi, fq;
            int pA, pB;
            int rA, gA, bA;
            int rB, gB, bB;
            int sx, ph;
            int ire; /* composite signal */

            sx = (x * s->w) / destw;
            pA = s->rgb[sx + syA];
            pB = s->rgb[sx + syB];
            rA = (pA >> 16) & 0xff;
            gA = (pA >>  8) & 0xff;
            bA = (pA >>  0) & 0xff;
            rB = (pB >> 16) & 0xff;
            gB = (pB >>  8) & 0xff;
            bB = (pB >>  0) & 0xff;

            /* RGB to YIQ blend with potential pixel below */
            fy = (19595 * rA + 38470 * gA +  7471 * bA
                + 19595 * rB + 38470 * gB +  7471 * bB) >> 15;
            fi = (39059 * rA - 18022 * gA - 21103 * bA
                + 39059 * rB - 18022 * gB - 21103 * bB) >> 15;
            fq = (13894 * rA - 34275 * gA + 20382 * bA
                + 13894 * rB - 34275 * gB + 20382 * bB) >> 15;
            ph = CC_PHASE(y + yo);
            ire = BLACK_LEVEL + v->black_point;
            /* bandlimit Y,I,Q */
            fy = iirf(&iirY, fy);
            fi = iirf(&iirI, fi) * ph * cc[(x + 0) & 3];
            fq = iirf(&iirQ, fq) * ph * cc[(x + 3) & 3];
            ire += (fy + fi + fq) * (WHITE_LEVEL * v->white_point / 100) >> 10;
            if (ire < 0)   ire = 0;
            if (ire > 110) ire = 110;

            v->analog[(x + xo) + (y + yo) * CRT_HRES] = ire;
        }
    }
}

/* search windows, in samples */
#define HSYNC_WINDOW 8
#define VSYNC_WINDOW 8

extern void
crt_draw(struct CRT *v, int noise)
{
    struct {
        int y, i, q;
    } out[AV_LEN + 1], *yiqA, *yiqB;
    int i, j, line;
    int prev_e; /* filtered beam energy per scan line */
    int max_e; /* approx maximum energy in a scan line */
    int bright = v->brightness - (BLACK_LEVEL + v->black_point);
    signed char *sig;
    int s = 0;
    int field, ratio;
    int ccref[4]; /* color carrier signal */
   
    memset(ccref, 0, sizeof(ccref));
    
    for (i = 0; i < CRT_INPUT_SIZE; i++) {
        static int rn = 194; /* 'random' noise */

        rn = (214019 * rn + 140327895);

        /* signal + noise */
        s = v->analog[i] + (((((rn >> 16) & 0xff) - 0x7f) * noise) >> 8);
        if (s >  127) { s =  127; }
        if (s < -127) { s = -127; }
        v->inp[i] = s;
    }

    /* Look for vertical sync.
     * 
     * This is done by integrating the signal and
     * seeing if it exceeds a threshold. The threshold of
     * the vertical sync pulse is much higher because the
     * vsync pulse is a lot longer than the hsync pulse.
     * The signal needs to be integrated to lessen
     * the noise in the signal.
     */
    for (i = -VSYNC_WINDOW; i < VSYNC_WINDOW; i++) {
        line = POSMOD(v->vsync + i, CRT_VRES);
        sig = v->inp + line * CRT_HRES;
        s = 0;
        for (j = 0; j < CRT_HRES; j++) {
            s += sig[j];
            /* increase the multiplier to make the vsync
             * more stable when there is a lot of noise
             */
            if (s <= (100 * SYNC_LEVEL)) {
                goto vsync_found;
            }
        }
    }
vsync_found:
    v->vsync = line; /* vsync found (or gave up) at this line */
    /* if vsync signal was in second half of line, odd field */
    field = (j > (CRT_HRES / 2));
    
    max_e = (128 + (noise / 2)) * AV_LEN;
    prev_e = (16384 / 8);
    /* ratio of output height to active video lines in the signal */
    ratio = (v->outh << 16) / CRT_LINES;
    ratio = (ratio + 32768) >> 16;
    
    field = (field * (ratio / 2));

    for (line = CRT_TOP; line < CRT_BOT; line++) {
        unsigned pos, ln;
        int scanL, scanR, dx;
        int L, R;
        int line_w;
        int *cL, *cR;
        int wave[4];
        int dci, dcq; /* decoded I, Q */
        int xpos, ypos;
        int beg, end;
        int phasealign;
  
        beg = (line - CRT_TOP + 0) * v->outh / CRT_LINES + field;
        end = (line - CRT_TOP + 1) * v->outh / CRT_LINES + field;

        if (beg >= v->outh) { continue; }
        if (end > v->outh) { end = v->outh; }

        /* Look for horizontal sync.
         * See comment above regarding vertical sync.
         */
        ln = (POSMOD(line + v->vsync, CRT_VRES)) * CRT_HRES;
        sig = v->inp + ln + v->hsync;
        s = 0;
        for (i = -HSYNC_WINDOW; i < HSYNC_WINDOW; i++) {
            s += sig[SYNC_BEG + i];
            if (s <= (4 * SYNC_LEVEL)) {
                break;
            }
        }
        v->hsync = POSMOD(i + v->hsync, CRT_HRES);
       
        sig = v->inp + ln + (v->hsync & ~3); /* burst @ 1/CB_FREQ sample rate */
        for (i = CB_BEG; i < CB_BEG + (CB_CYCLES * CRT_CB_FREQ); i++) {
            int p = ccref[i & 3] * 127 / 128; /* fraction of the previous */
            int n = sig[i];                   /* mixed with the new sample */
            ccref[i & 3] = p + n;
        }
        
        xpos = POSMOD(AV_BEG + v->hsync, CRT_HRES);
        ypos = POSMOD(line + v->vsync, CRT_VRES);
        pos = xpos + ypos * CRT_HRES;
        phasealign = pos & 3;
        
        /* amplitude of carrier = saturation, phase difference = hue */
        dci = ccref[(phasealign + 1) & 3] - ccref[(phasealign + 3) & 3];
        dcq = ccref[(phasealign + 2) & 3] - ccref[(phasealign + 0) & 3];

        wave[0] = -dcq * v->saturation;
        wave[1] =  dci * v->saturation;
        wave[2] =  dcq * v->saturation;
        wave[3] = -dci * v->saturation;

        sig = v->inp + pos;
        s = 0;
        for (i = 0; i < AV_LEN; i++) {
            s += sig[i]; /* sum up the scan line */
        }
        /* bloom emulation */
        prev_e = (prev_e * 123 / 128) + ((((max_e >> 1) - s) << 10) / max_e);
        line_w = (AV_LEN * 112 / 128) + (prev_e >> 9);

        dx = (line_w << 12) / v->outw;
        scanL = ((AV_LEN / 2) - (line_w >> 1) + 8) << 12;
        scanR = (AV_LEN - 1) << 12;
        
        L = (scanL >> 12);
        R = (scanR >> 12);
 
        reset_eq(&eqY);
        reset_eq(&eqI);
        reset_eq(&eqQ);
        
        for (i = L; i < R; i++) {
            out[i].y = eqf(&eqY, sig[i] + bright) << 4;
            out[i].i = eqf(&eqI, sig[i] * wave[(i + 0) & 3] >> 9) >> 3;
            out[i].q = eqf(&eqQ, sig[i] * wave[(i + 3) & 3] >> 9) >> 3;
        }

        cL = v->out + beg * v->outw;
        cR = cL + v->outw;

        for (pos = scanL; pos < scanR && cL < cR; pos += dx) {
            int y, i, q;
            int r, g, b;
            int aa, bb;

            R = pos & 0xfff;
            L = 0xfff - R;
            s = pos >> 12;
            
            yiqA = out + s;
            yiqB = out + s + 1;
            
            /* interpolate between samples if needed */
            y = ((yiqA->y * L) >>  2) + ((yiqB->y * R) >>  2);
            i = ((yiqA->i * L) >> 14) + ((yiqB->i * R) >> 14);
            q = ((yiqA->q * L) >> 14) + ((yiqB->q * R) >> 14);
            
            /* YIQ to RGB */
            r = (((y + 3879 * i + 2556 * q) >> 12) * v->contrast) >> 8;
            g = (((y - 1126 * i - 2605 * q) >> 12) * v->contrast) >> 8;
            b = (((y - 4530 * i + 7021 * q) >> 12) * v->contrast) >> 8;
          
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            
            aa = (r << 16 | g << 8 | b);
            bb = *cL;
            /* blend with previous color there */
            *cL++ = (((aa & 0xfefeff) >> 1) + ((bb & 0xfefeff) >> 1));
        }
        
        /* duplicate extra lines */
        ln = v->outw * sizeof(int);
        for (s = beg + 1; s < end; s++) {
            memcpy(v->out + s * v->outw, v->out + (s - 1) * v->outw, ln);
        }
    }
}
