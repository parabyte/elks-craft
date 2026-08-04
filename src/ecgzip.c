/*
 * elks-craft - level streaming
 *
 * A joining client expects the whole map as gzip(4 byte big endian block
 * count + raw block array), cut into 1024 byte pieces inside Level Data
 * Chunk packets.  ClassiCube really does run it through a gzip header
 * parser and an inflate implementation, so this has to be a valid stream.
 *
 * Rather than link a compressor, this emits a single fixed Huffman deflate
 * block and finds matches by run length only: a run of N identical bytes
 * becomes one literal plus length/distance-1 matches.  Terrain is almost
 * entirely long runs of air and stone, so it compresses hundreds to one for
 * a couple hundred lines of code and no tables to speak of.
 *
 * Everything streams.  The world is generated a row at a time, compressed
 * as it goes, and pushed out as each 1024 byte chunk fills, so a 256K block
 * world never exists in memory.
 */

#include <string.h>
#include "elkscraft.h"

/* deflate length codes 257..285 */
static const unsigned int len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};

static unsigned long crc_table[256];
static int crc_ready;

struct gz {
    int             fd;
    unsigned char   pkt[1028];  /* a whole Level Data Chunk packet */
    int             cpos;       /* bytes filled in the 1024 byte payload */
    unsigned long   crc;
    unsigned long   isize;      /* uncompressed bytes consumed */
    unsigned long   total;      /* expected total, for the percent byte */
    unsigned long   bitbuf;
    int             bitcnt;
    int             runbyte;    /* -1 when no run is open */
    unsigned long   runlen;
    int             err;
    int             emitted;    /* a chunk went out on this pump */
};

/*
 * Longest run we let build up before flushing it.  flush_run() turns a run
 * into one literal plus a match every 258 bytes and cannot be interrupted
 * part way, so an unbounded run means an unbounded burst of writes inside a
 * single call - which on ELKS means an unbounded stretch of blocking socket
 * writes with nobody else being served.  At 258 input bytes per match and
 * about 13 bits per match, this ceiling keeps one flush under a kilobyte of
 * output.  Splitting a run costs one extra literal and nothing else; the
 * stream stays valid deflate either way.
 */
#define RUN_MAX         77400UL

static void crc_init(void)
{
    unsigned long c;
    int i, k;

    for (i = 0; i < 256; i++) {
        c = (unsigned long)i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

/* ---------------------------------------------------------------- output */

static void flush_chunk(struct gz *g)
{
    int pct;

    if (g->err) {
        g->cpos = 0;            /* or out_byte walks off the end of pkt[] */
        return;
    }
    /* short chunks are zero padded; the length field tells the client how
     * much of the 1024 bytes is real */
    if (g->cpos < 1024)
        memset(g->pkt + 3 + g->cpos, 0, 1024 - g->cpos);

    g->pkt[0] = S_LEVEL_DATA;
    g->pkt[1] = (unsigned char)(g->cpos >> 8);
    g->pkt[2] = (unsigned char)(g->cpos & 0xff);

    pct = (g->total == 0) ? 100 : (int)((g->isize * 100) / g->total);
    if (pct > 100)
        pct = 100;
    g->pkt[1027] = (unsigned char)pct;

    if (write_all(g->fd, g->pkt, 1028) < 0)
        g->err = 1;
    g->cpos = 0;
    g->emitted = 1;
}

/*
 * Once the socket has failed there is nothing left worth encoding, and
 * carrying on is not merely wasteful: flush_run() emits a whole pending run
 * in one burst with no error poll inside it, so without this guard the
 * encoder keeps calling out_byte() after cpos has stopped advancing and
 * writes off the end of pkt[] into the rest of the data segment.
 */
static void out_byte(struct gz *g, unsigned char b)
{
    if (g->err)
        return;
    g->pkt[3 + g->cpos] = b;
    if (++g->cpos >= 1024)
        flush_chunk(g);
}

static void putbits(struct gz *g, unsigned int val, int n)
{
    g->bitbuf |= (unsigned long)val << g->bitcnt;
    g->bitcnt += n;
    while (g->bitcnt >= 8) {
        out_byte(g, (unsigned char)(g->bitbuf & 0xff));
        g->bitbuf >>= 8;
        g->bitcnt -= 8;
    }
}

/* Huffman codes go out most significant bit first, the bit stream is least
 * significant bit first, so codes get reversed on the way in */
static unsigned int revbits(unsigned int v, int n)
{
    unsigned int r = 0;

    while (n--) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/* literal/length alphabet under the fixed Huffman table */
static void emit_sym(struct gz *g, int sym)
{
    if (sym < 144)
        putbits(g, revbits(0x30 + sym, 8), 8);
    else if (sym < 256)
        putbits(g, revbits(0x190 + sym - 144, 9), 9);
    else if (sym < 280)
        putbits(g, revbits(sym - 256, 7), 7);
    else
        putbits(g, revbits(0xc0 + sym - 280, 8), 8);
}

static void emit_match(struct gz *g, int len)
{
    int i = 28;

    while (i > 0 && len < (int)len_base[i])
        i--;
    emit_sym(g, 257 + i);
    if (len_extra[i])
        putbits(g, (unsigned int)(len - len_base[i]), len_extra[i]);
    putbits(g, 0, 5);       /* distance code 0 == distance 1, no extra bits */
}

static void flush_run(struct gz *g)
{
    unsigned long rem;
    int l;

    if (g->runbyte < 0)
        return;

    emit_sym(g, g->runbyte);
    rem = g->runlen - 1;            /* the rest is a copy of what precedes */

    while (rem >= 3) {
        if (rem > 258)
            l = (rem - 258 < 3) ? (int)(rem - 3) : 258;
        else
            l = (int)rem;
        emit_match(g, l);
        rem -= l;
    }
    while (rem--)
        emit_sym(g, g->runbyte);

    g->runbyte = -1;
    g->runlen = 0;
}

static void gz_write(struct gz *g, const unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        g->crc = crc_table[(g->crc ^ buf[i]) & 0xff] ^ (g->crc >> 8);
        if (g->runbyte == (int)buf[i]) {
            if (++g->runlen >= RUN_MAX)
                flush_run(g);
        } else {
            flush_run(g);
            g->runbyte = buf[i];
            g->runlen = 1;
        }
    }
    g->isize += len;
}

/* ------------------------------------------------------------------ send */

/*
 * Input bytes chewed through per pump.  Small enough that the select loop
 * keeps turning over for everyone else, large enough that a join does not
 * take all day.  The output side is nowhere near the bottleneck: terrain is
 * mostly long runs, so a 256K block world leaves here as a handful of
 * packets.  What costs time is the CRC and run scan over every block.
 */
#define PUMP_BYTES      4096

static struct gz g;             /* one at a time: too big to give each player */
static int gz_active;
static int cur_y, cur_z;

int level_active(void)
{
    return gz_active;
}

/* the client we were streaming to disconnected: drop the whole stream */
void level_abort(void)
{
    gz_active = 0;
}

int level_start(int fd)
{
    unsigned char hdr[4];

    if (!crc_ready)
        crc_init();

    memset(&g, 0, sizeof(g));
    g.fd = fd;
    g.crc = 0xffffffffUL;
    g.runbyte = -1;
    g.total = 4 + world_volume();
    cur_y = 0;
    cur_z = 0;
    gz_active = 1;

    /* gzip header: deflate, no flags, no mtime, unknown OS */
    out_byte(&g, 0x1f);
    out_byte(&g, 0x8b);
    out_byte(&g, 0x08);
    out_byte(&g, 0x00);
    out_byte(&g, 0x00);
    out_byte(&g, 0x00);
    out_byte(&g, 0x00);
    out_byte(&g, 0x00);
    out_byte(&g, 0x00);
    out_byte(&g, 0xff);

    /* one fixed Huffman block covering the whole level */
    putbits(&g, 1, 1);          /* BFINAL */
    putbits(&g, 1, 2);          /* BTYPE = fixed Huffman */

    /* the block count the client reads before the block array */
    hdr[0] = (unsigned char)((g.total - 4) >> 24);
    hdr[1] = (unsigned char)((g.total - 4) >> 16);
    hdr[2] = (unsigned char)((g.total - 4) >> 8);
    hdr[3] = (unsigned char)(g.total - 4);
    gz_write(&g, hdr, 4);

    if (g.err) {
        gz_active = 0;
        return -1;
    }
    return 0;
}

/*
 * The two gzip trailer words go out through this static, one byte at a time,
 * shifting only ever by 8.
 *
 * That is deliberate and the shape matters.  ia16-gcc 6.3 at -Os gets 32 bit
 * values wrong in this function in three different ways: held in a local
 * ("unsigned long crc = g.crc ^ ~0UL") it reserved the stack slot, never
 * stored to it and emitted whatever was already there; passed as a function
 * argument it corrupted both words; and read straight from the struct it
 * still produced the wrong byte for the >> 16 term.  A file scope static
 * shifted down 8 bits at a time keeps every step in memory and comes out
 * right.  Levels used to ship with a checksum that changed from run to run -
 * ClassiCube does not verify it, so this was invisible in play.
 */
static unsigned long trailer;

static void out_trailer(struct gz *g)
{
    int i;

    for (i = 0; i < 4; i++) {
        out_byte(g, (unsigned char)trailer);
        trailer >>= 8;
    }
}

static int level_finish(void)
{
    flush_run(&g);
    emit_sym(&g, 256);          /* end of block */
    if (g.bitcnt > 0)           /* pad the last byte out */
        putbits(&g, 0, 8 - g.bitcnt);

    g.crc ^= 0xffffffffUL;
    trailer = g.crc;
    out_trailer(&g);
    trailer = g.isize;
    out_trailer(&g);

    if (g.cpos > 0)
        flush_chunk(&g);

    gz_active = 0;
    return g.err ? -1 : 0;
}

int level_pump(void)
{
    unsigned char row[WORLD_MAX];
    int budget = PUMP_BYTES;

    if (!gz_active)
        return 0;

    g.emitted = 0;
    while (budget > 0) {
        if (cur_y >= world_h)
            return level_finish();

        world_row(cur_y, cur_z, row);
        gz_write(&g, row, world_w);
        budget -= world_w;

        if (g.err) {
            gz_active = 0;
            return -1;
        }
        if (++cur_z >= world_l) {
            cur_z = 0;
            cur_y++;
        }
        /*
         * At most one 1028 byte chunk leaves per turn round the select loop.
         * Writes on ELKS sockets cannot be made non-blocking, so the only
         * lever on how long a joining client can hold up everyone else is
         * how much we try to push at once.
         */
        if (g.emitted)
            break;
    }
    return 1;
}
