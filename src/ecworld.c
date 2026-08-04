/*
 * elks-craft - world storage, generation and persistence
 *
 * Two storage modes:
 *
 *   far    The whole block array lives in main memory outside our 64K data
 *          segment, via fmemalloc().  Every block is individually editable,
 *          reads and writes are a couple of instructions, and the world can
 *          be saved to and restored from disk.  This is what an XT buys us
 *          over the 2K of SRAM the AVR original had to live in.
 *
 *   gen    Fallback when there is not enough main memory.  Terrain becomes a
 *          pure function of (x,z) again and player edits go in a bounded
 *          table sized from whatever heap is left.
 *
 * Disk is deliberately kept out of the gameplay path.  The target is an MFM
 * drive where a seek costs tens of milliseconds, so the world is read once
 * at startup, before the listening socket accepts anyone, and written back
 * in small bounded slices by the idle tick.  No packet handler ever waits on
 * the disk.
 */

#include <stdio.h>          /* rename(), for the atomic world save */
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include "elkscraft.h"

#define FP_SEG(fp)      ((unsigned int)((unsigned long)(void __far *)(fp) >> 16))
#define MK_FP(seg, off) ((void __far *)((((unsigned long)(seg)) << 16) | \
                                        ((unsigned int)(off))))

#define SAVE_MAGIC      "DCW1"
#define SAVE_HDRLEN     10
/*
 * Bytes written per idle tick.  This buffer is static rather than automatic
 * on purpose: the program links with -maout-stack=3072, so a 4K array on the
 * stack - which is what this used to be - overran the whole stack the first
 * time a save ran.
 */
#define SAVE_SLICE      1024

static unsigned int wseg;           /* base segment of the block array, 0 = gen mode */
static unsigned char *hmap;         /* heightmap, kept only in gen mode */
static unsigned long wseed;
static int sea_level;

/* gen mode edit table, sorted by linear block index */
struct delta {
    unsigned char x, y, z, b;
};
static struct delta *deltas;
static int ndeltas, maxdeltas;

/* background save state */
static const char *save_path;
static int save_fd = -1;
static long save_idx;
static int world_dirty;

/* ------------------------------------------------------------ addressing */

static long block_index(int x, int y, int z)
{
    return ((long)y * world_l + z) * world_w + x;
}

/*
 * A far pointer to one block.  Index is folded into the segment so the
 * offset stays tiny, which keeps a whole row (255 bytes at most) addressable
 * from the same pointer without wrapping.  Real mode only, which is what
 * this target is.
 */
static unsigned char __far *far_at(long idx)
{
    return (unsigned char __far *)MK_FP(wseg + (unsigned int)(idx >> 4),
                                        (unsigned int)(idx & 15));
}

long world_volume(void)
{
    return (long)world_w * world_h * world_l;
}

const char *world_mode(void)
{
    return wseg ? "far memory" : "generated";
}

/* ------------------------------------------------------------ generation */

static unsigned int noise(int gx, int gz)
{
    unsigned long n;

    n  = (unsigned long)(unsigned int)gx * 73856093UL;
    n ^= (unsigned long)(unsigned int)gz * 19349663UL;
    n ^= wseed * 83492791UL;
    n ^= n >> 13;
    n *= 1274126177UL;
    n ^= n >> 16;
    return (unsigned int)(n & 0xff);
}

/* value noise on a `grid` spaced lattice, bilinearly interpolated, 0..255 */
static int octave(int x, int z, int grid)
{
    int x0 = x / grid, z0 = z / grid;
    int fx = x - x0 * grid, fz = z - z0 * grid;
    long v00, v10, v01, v11, a, b;

    v00 = noise(x0, z0);
    v10 = noise(x0 + 1, z0);
    v01 = noise(x0, z0 + 1);
    v11 = noise(x0 + 1, z0 + 1);
    a = v00 * (grid - fx) + v10 * fx;
    b = v01 * (grid - fx) + v11 * fx;
    return (int)((a * (grid - fz) + b * fz) / ((long)grid * grid));
}

static int height_at(int x, int z)
{
    int n, h, amp;

    n = (octave(x, z, 32) * 2 + octave(x, z, 8)) / 3;
    amp = world_h / 6;
    if (amp < 2)
        amp = 2;
    h = sea_level + ((n - 128) * amp) / 128;
    if (h < 1)
        h = 1;
    if (h > world_h - 2)
        h = world_h - 2;
    return h;
}

static int col_height(int x, int z)
{
    if (hmap)
        return hmap[z * world_w + x];
    return height_at(x, z);
}

static unsigned char gen_block(int x, int y, int z)
{
    int h = col_height(x, z);

    if (y == 0)
        return BLK_BEDROCK;
    if (y > h)
        return (y <= sea_level) ? BLK_WATER_STILL : BLK_AIR;
    if (y == h)
        return (h < sea_level) ? BLK_SAND : BLK_GRASS;
    if (y >= h - 3)
        return BLK_DIRT;
    return BLK_STONE;
}

/* ------------------------------------------------------- far mode filling */

static void far_put(long idx, unsigned char b)
{
    *far_at(idx) = b;
}

static unsigned char far_get(long idx)
{
    return *far_at(idx);
}

/*
 * Trees.  Only worth doing in far mode: they write into neighbouring
 * columns, which a pure (x,z) terrain function cannot express cheaply.
 */
static void plant_tree(int x, int y, int z)
{
    int dx, dy, dz, trunk = 4 + (int)(noise(x * 7, z * 13) & 1);

    if (x < 2 || z < 2 || x >= world_w - 2 || z >= world_l - 2)
        return;
    if (y + trunk + 2 >= world_h)
        return;

    for (dy = trunk - 2; dy <= trunk + 1; dy++) {
        int r = (dy >= trunk) ? 1 : 2;
        for (dz = -r; dz <= r; dz++)
            for (dx = -r; dx <= r; dx++) {
                if (dx == 0 && dz == 0 && dy < trunk)
                    continue;
                if (r == 2 && (dx & dz & 1) && (dx * dx + dz * dz) == 8)
                    continue;           /* clip the corners a little */
                far_put(block_index(x + dx, y + dy, z + dz), BLK_LEAVES);
            }
    }
    for (dy = 0; dy < trunk; dy++)
        far_put(block_index(x, y + dy, z), BLK_LOG);
}

static void world_generate(void)
{
    int x, y, z;
    unsigned char __far *row;
    long idx;

    /* one mark per layer: a minute of silence on an XT reads as a hang */
    errmsg("elkscraft: generating ");
    for (y = 0; y < world_h; y++) {
        errmsg(".");
        for (z = 0; z < world_l; z++) {
            idx = block_index(0, y, z);
            row = far_at(idx);
            for (x = 0; x < world_w; x++)
                row[x] = gen_block(x, y, z);
        }
    }

    errmsg(" trees");
    for (z = 2; z < world_l - 2; z++) {
        for (x = 2; x < world_w - 2; x++) {
            int h = col_height(x, z);
            if (h < sea_level)
                continue;                       /* no trees on the beach */
            if ((noise(x * 3 + 1, z * 5 + 7) & 0x3f) != 0)
                continue;                       /* roughly 1 column in 64 */
            plant_tree(x, h + 1, z);
        }
    }
    errmsg("\n");
}

/* ------------------------------------------------------------- gen mode */

static int delta_find(long idx, int *ins)
{
    int lo = 0, hi = ndeltas - 1, mid;
    long m;

    while (lo <= hi) {
        mid = (lo + hi) / 2;
        m = block_index(deltas[mid].x, deltas[mid].y, deltas[mid].z);
        if (m == idx)
            return mid;
        if (m < idx)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (ins)
        *ins = lo;
    return -1;
}

/* --------------------------------------------------------------- public */

unsigned char world_block(int x, int y, int z)
{
    int i;

    if (x < 0 || y < 0 || z < 0 || x >= world_w || y >= world_h || z >= world_l)
        return BLK_AIR;
    if (wseg)
        return far_get(block_index(x, y, z));

    i = delta_find(block_index(x, y, z), (int *)0);
    if (i >= 0)
        return deltas[i].b;
    return gen_block(x, y, z);
}

int world_setblock(int x, int y, int z, unsigned char blk)
{
    long idx;
    int i, ins = 0;

    if (x < 0 || y < 0 || z < 0 || x >= world_w || y >= world_h || z >= world_l)
        return 0;

    idx = block_index(x, y, z);
    world_dirty = 1;

    if (wseg) {
        far_put(idx, blk);
        return 1;
    }

    i = delta_find(idx, &ins);
    if (blk == gen_block(x, y, z)) {
        if (i >= 0) {
            memmove(&deltas[i], &deltas[i + 1],
                    (ndeltas - i - 1) * sizeof(struct delta));
            ndeltas--;
        }
        return 1;
    }
    if (i >= 0) {
        deltas[i].b = blk;
        return 1;
    }
    if (ndeltas >= maxdeltas)
        return 0;
    memmove(&deltas[ins + 1], &deltas[ins],
            (ndeltas - ins) * sizeof(struct delta));
    deltas[ins].x = (unsigned char)x;
    deltas[ins].y = (unsigned char)y;
    deltas[ins].z = (unsigned char)z;
    deltas[ins].b = blk;
    ndeltas++;
    return 1;
}

/* one row of blocks at (y,z), in ascending index order */
void world_row(int y, int z, unsigned char *row)
{
    int x, i;
    long base = block_index(0, y, z);

    if (wseg) {
        unsigned char __far *src = far_at(base);
        for (x = 0; x < world_w; x++)
            row[x] = src[x];
        return;
    }

    for (x = 0; x < world_w; x++)
        row[x] = gen_block(x, y, z);

    i = delta_find(base, &x);
    if (i < 0)
        i = x;
    for (; i < ndeltas; i++) {
        if (deltas[i].y != y || deltas[i].z != z)
            break;
        row[deltas[i].x] = deltas[i].b;
    }
}

void world_spawn(int *x, int *y, int *z)
{
    int bx = world_w / 2, bz = world_l / 2;
    int h;

    if (wseg) {
        /* scan down the real column so we land on top of whatever is there */
        for (h = world_h - 1; h > 0; h--)
            if (far_get(block_index(bx, h, bz)) != BLK_AIR)
                break;
    } else {
        h = col_height(bx, bz);
        if (h < sea_level)
            h = sea_level;
    }
    /*
     * Feet go on top of block h, and Classic positions carry a fixed 51/32
     * model offset on top of that.  Leaving the offset out puts every other
     * player's model a block and a half into the ground from where they
     * think they are standing.
     */
    *x = bx * 32 + 16;
    *y = (h + 1) * 32 + 51;
    *z = bz * 32 + 16;
}

/* ---------------------------------------------------------- persistence */

/*
 * Load a saved world.  One sequential pass through a small bounce buffer:
 * read() only takes near pointers, so bytes land in the data segment first
 * and are copied out to the far array.  Runs before the server accepts
 * anyone, so the MFM drive can take as long as it likes.
 */
static int world_load(const char *path)
{
    unsigned char buf[512];
    unsigned char hdr[SAVE_HDRLEN];
    unsigned char __far *dst;
    long left, idx = 0;
    int fd, n, i;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    if (read(fd, hdr, SAVE_HDRLEN) != SAVE_HDRLEN ||
        memcmp(hdr, SAVE_MAGIC, 4) != 0) {
        close(fd);
        return 0;
    }
    if ((hdr[4] | (hdr[5] << 8)) != world_w ||
        (hdr[6] | (hdr[7] << 8)) != world_h ||
        (hdr[8] | (hdr[9] << 8)) != world_l) {
        close(fd);
        return 0;               /* saved with different dimensions */
    }

    errmsg("elkscraft: loading ");
    errmsg(path);
    errmsg(" ");
    left = world_volume();
    while (left > 0) {
        n = (left > (long)sizeof(buf)) ? (int)sizeof(buf) : (int)left;
        n = read(fd, buf, n);
        if (n <= 0)
            break;
        dst = far_at(idx);
        for (i = 0; i < n; i++)
            dst[i] = buf[i];
        idx += n;
        left -= n;
        if ((idx & 0x1fff) == 0)        /* a mark every 8K off the drive */
            errmsg(".");
    }
    close(fd);
    if (left != 0)
        errmsg(" short read, regenerating");
    errmsg("\n");
    return (left == 0);
}

/*
 * The world is written to a sibling file and renamed into place at the end.
 * A save takes many idle ticks, so truncating the real file up front would
 * mean that anything interrupting it - a power cut, a full disk, a SIGKILL -
 * left the only copy of the world half written.
 */
static void save_tmp_path(char *out, int size)
{
    int n = 0;

    while (save_path[n] && n < size - 5) {
        out[n] = save_path[n];
        n++;
    }
    out[n++] = '.';
    out[n++] = 'n';
    out[n++] = 'e';
    out[n++] = 'w';
    out[n] = '\0';
}

void world_save_begin(void)
{
    unsigned char hdr[SAVE_HDRLEN];
    char tmp[64];

    if (!save_path || !wseg || !world_dirty || save_fd >= 0)
        return;

    save_tmp_path(tmp, (int)sizeof(tmp));
    save_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (save_fd < 0)
        return;

    memcpy(hdr, SAVE_MAGIC, 4);
    hdr[4] = (unsigned char)world_w;
    hdr[5] = (unsigned char)(world_w >> 8);
    hdr[6] = (unsigned char)world_h;
    hdr[7] = (unsigned char)(world_h >> 8);
    hdr[8] = (unsigned char)world_l;
    hdr[9] = (unsigned char)(world_l >> 8);
    if (write(save_fd, hdr, SAVE_HDRLEN) != SAVE_HDRLEN) {
        close(save_fd);
        save_fd = -1;
        return;
    }
    save_idx = 0;
    /*
     * world_dirty stays set until the rename succeeds.  Clearing it here
     * would mean a save that failed half way through was never retried, and
     * the edits it was meant to persist would be lost for good.
     */
}

/*
 * Write one slice of the world.  Returns 1 while there is more to do.  Only
 * ever called from the idle path, and only a few KB at a time, so a slow
 * drive shows up as a save that takes a while rather than as a stall.
 */
int world_save_pump(void)
{
    static unsigned char buf[SAVE_SLICE];
    unsigned char __far *src;
    long left;
    int n, i;

    if (save_fd < 0)
        return 0;

    left = world_volume() - save_idx;
    if (left <= 0) {
        char tmp[64];

        close(save_fd);
        save_fd = -1;
        save_tmp_path(tmp, (int)sizeof(tmp));
        if (rename(tmp, save_path) == 0)
            world_dirty = 0;    /* edits from here on mark it dirty again */
        return 0;
    }
    n = (left > SAVE_SLICE) ? SAVE_SLICE : (int)left;
    src = far_at(save_idx);
    for (i = 0; i < n; i++)
        buf[i] = src[i];

    if (write(save_fd, buf, n) != n) {
        close(save_fd);
        save_fd = -1;
        return 0;
    }
    save_idx += n;
    return 1;
}

int world_needs_save(void)
{
    return world_dirty && save_fd < 0 && save_path && wseg;
}

int world_saving(void)
{
    return save_fd >= 0;
}

/* ---------------------------------------------------------------- setup */

int world_init(unsigned int seed, const char *path)
{
    void __far *mem;
    long vol = world_volume();
    int want;

    wseed = seed;
    sea_level = world_h / 2;
    ndeltas = 0;
    save_path = path;

    /* the heightmap drives generation in both modes */
    hmap = (unsigned char *)malloc((size_t)world_w * world_l);

    mem = fmemalloc((unsigned long)vol);
    if (mem) {
        int x, z;

        wseg = FP_SEG(mem);
        if (hmap) {
            for (z = 0; z < world_l; z++)
                for (x = 0; x < world_w; x++)
                    hmap[z * world_w + x] = (unsigned char)height_at(x, z);
        }
        if (!path || !world_load(path))
            world_generate();
        /* the block array is the world now; the heightmap was scaffolding */
        free(hmap);
        hmap = (unsigned char *)0;
        return 1;
    }

    /*
     * No room for the block array.  Fall back to generating terrain on
     * demand, and spend whatever heap is left on the edit table.
     */
    if (!hmap)
        return 0;
    {
        int x, z;
        for (z = 0; z < world_l; z++)
            for (x = 0; x < world_w; x++)
                hmap[z * world_w + x] = (unsigned char)height_at(x, z);
    }
    for (want = 8192; want >= 256; want /= 2) {
        deltas = (struct delta *)malloc((size_t)want * sizeof(struct delta));
        if (deltas) {
            maxdeltas = want;
            break;
        }
    }
    return deltas ? 1 : 0;
}

int world_edit_capacity(void)
{
    return wseg ? -1 : maxdeltas;
}
