/*
 * elks-craft - the ELKS Minecraft server
 *
 * Derived in spirit from cnlohr's dumbcraft (avrcraft), which serves the
 * modern Minecraft 1.11 protocol to tiny machines.  The wire protocol here
 * is instead Minecraft Classic 0.30 (protocol 7), which is what the
 * ClassiCube client speaks, and which suits a 16-bit box far better:
 * fixed-size packets, no NBT, no VarInts.
 *
 * The design ideas that carried over from it: a fixed array of players,
 * no malloc anywhere, a procedurally generated world that is never held in
 * RAM, and a small bounded table of player edits layered on top of it.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef ELKSCRAFT_H
#define ELKSCRAFT_H

/*
 * Protocol constants.  Packet sizes below are taken from the ClassiCube
 * client itself (src/Protocol.c, Classic_Reset()), not from the wiki, whose
 * size column disagrees with the client for several packets.
 */
#define MC_PORT             25565
#define MC_PROTOCOL_VER     0x07    /* Classic 0.30 */
#define MC_STRLEN           64      /* fixed string field, space padded */
#define MC_SELF_ID          0xff    /* entity id meaning "you" */

/* server -> client opcodes */
#define S_IDENT             0x00    /* 131 bytes */
#define S_PING              0x01    /* 1 */
#define S_LEVEL_INIT        0x02    /* 1 */
#define S_LEVEL_DATA        0x03    /* 1028 */
#define S_LEVEL_FINAL       0x04    /* 7 */
#define S_SET_BLOCK         0x06    /* 8 */
#define S_SPAWN             0x07    /* 74 */
#define S_TELEPORT          0x08    /* 10 */
#define S_DESPAWN           0x0c    /* 2 */
#define S_MESSAGE           0x0d    /* 66 */
#define S_KICK              0x0e    /* 65 */
#define S_USERTYPE          0x0f    /* 2 */

/* client -> server opcodes */
#define C_IDENT             0x00    /* 131 */
#define C_PING              0x01    /* 1, only sent by old protocol versions */
#define C_SET_BLOCK         0x05    /* 9 */
#define C_POSITION          0x08    /* 10 */
#define C_MESSAGE           0x0d    /* 66 */

#define C_MAXPKT            131     /* largest client packet (identification) */

/* Classic block ids we generate */
#define BLK_AIR             0
#define BLK_STONE           1
#define BLK_GRASS           2
#define BLK_DIRT            3
#define BLK_BEDROCK         7
#define BLK_WATER_STILL     9
#define BLK_SAND            12
#define BLK_LOG             17
#define BLK_LEAVES          18
#define BLK_MAX             49      /* highest id a vanilla 0.30 client knows */

/*
 * Server limits.  An XT is not an ATmega: where the AVR original had to cap
 * itself at 3 players and a toy world in 2K of SRAM, here the only real
 * ceilings are NR_OPEN (20 file descriptors per process) and how much main
 * memory fmemalloc() can hand us outside our 64K data segment.
 *
 * Descriptors are not the binding constraint, memory in ktcp is, and it is a
 * much tighter one.  ktcp is linked with a 33792 byte heap and allocates
 * sizeof(struct tcpcb_list_s) + CB_NORMAL_BUFSIZ = 4470 bytes for every
 * connection, so the whole machine has room for about seven at once - shared
 * with telnetd, ftpd and anything else listening.  Ask for more than that and
 * ktcp's malloc fails, which does not merely refuse the player: it takes down
 * every TCP service on the box until it is restarted.
 *
 *      33792 / 4470 = 7.5 connections, machine wide
 *
 * So this is deliberately small, with headroom left for the other daemons.
 * Setting it to the descriptor limit (20 - stderr - listener - world file =
 * 17) looked reasonable and killed the machine the first time it met the
 * open internet, where port 25565 is scanned continuously.
 *
 * Note ktcp allocates that buffer when the SYN arrives, not when the server
 * accepts, and ignores SO_RCVBUF for accepted sockets, so a server cannot
 * avoid the cost by refusing connections - it can only hold fewer of them,
 * and let go of the ones that are not players quickly.
 *
 * Running in the foreground (-f) keeps stdin and stdout, so two descriptor
 * slots cannot be filled; accept() failing is handled rather than fatal.
 */
#define MAX_PLAYERS         4

/* world size limits.  Dimensions ride in single bytes in the fallback edit
 * table, and the whole level is streamed to each joining client. */
/*
 * Default world size.  The block array comes out of main memory, and the
 * default is chosen so that the machine is still usable while the server runs
 * rather than to be as large as will fit.
 *
 * Measured on a 640K PC1640 with ktcp, telnetd and ftpd up: 194K free before
 * the server starts.  The daemon's own segments take about 33K, and forking
 * anything from a telnet session costs another 66K, because fork copies the
 * shell's whole data segment.  So
 *
 *      194K free - 33K daemon - 66K to fork a shell = 95K for the world,
 *
 * and that is the ceiling at which the machine can still be administered, not
 * a comfortable figure.  The old 64x24x64 default was 96K, right on it, and
 * the result was that ps, kill, chmod and shutdown all failed with "Cannot
 * fork" for as long as the server was running - which meant it could not even
 * be stopped or upgraded without a power cycle.
 *
 * 48x24x48 is 54K, leaving about 107K.  A telnet login and a command run from
 * it both fit, with room to spare.  Bigger worlds are still available with
 * -w/-h/-l on machines that have the memory; if the allocation fails the
 * server falls back to generating terrain on demand rather than refusing to
 * start.
 */
#define WORLD_MIN           16
#define WORLD_MAX           255
#define DEF_WIDTH           48
#define DEF_HEIGHT          24
#define DEF_LENGTH          48

/* fmemalloc tops out just under 1M, and the index arithmetic below assumes
 * a block index fits the segment maths */
#define WORLD_MAXVOL        1000000L

struct player {
    int             fd;             /* -1 when the slot is free */
    unsigned char   state;
#define PS_FREE     0
#define PS_HELLO    1               /* connected, awaiting identification */
#define PS_PLAY     2               /* level sent, entity spawned */
#define PS_QUEUE    3               /* identified, waiting for the streamer */
#define PS_LOAD     4               /* being sent the level right now */
    unsigned char   proto;          /* version the client identified with */
    /*
     * Set when a write to this player fails, which is how a client that has
     * gone away is usually noticed first: the peer's FIN puts the socket in
     * CLOSE_WAIT and ktcp refuses further writes with EPIPE.  A player cannot
     * be dropped from inside a broadcast without disturbing the walk over the
     * array, so it is flagged here and reaped at the top of the next turn.
     * Reaping matters more than tidiness - every connection still open pins
     * 4470 bytes of ktcp's 33792 byte heap.
     */
    unsigned char   dead;
    char            name[MC_STRLEN + 1];
    int             x, y, z;        /* fixed point, 1/32 block */
    unsigned char   yaw, pitch;
    unsigned char   in[C_MAXPKT];   /* partial packet reassembly */
    int             inlen;
    unsigned long   since;          /* when this slot was taken, for timeouts */
};

/*
 * A connection that has not finished joining must not hold a slot for ever.
 * Kept short because the slot is not the expensive part: every connection
 * pins 4470 bytes of ktcp's heap until it goes away, and a port scanner that
 * connects and says nothing is the common case on a public address.  A real
 * client identifies itself immediately, so this only ever evicts something
 * that was not going to play.
 */
#define JOIN_TIMEOUT        15

/* elkscraft.c */
extern struct player players[MAX_PLAYERS];
extern int world_w, world_h, world_l;
void send_message(int fd, const char *msg);
int  write_all(int fd, const unsigned char *buf, int len);
void errmsg(const char *str);       /* startup reporting, on stderr */
void errnum(long v);

/* ecworld.c */
int  world_init(unsigned int seed, const char *path);
const char *world_mode(void);
unsigned char world_block(int x, int y, int z);
int  world_setblock(int x, int y, int z, unsigned char blk);
void world_spawn(int *x, int *y, int *z, unsigned char *yaw, unsigned char *pitch);
void world_setspawn(int x, int y, int z, unsigned char yaw, unsigned char pitch);
int  world_spawn_isset(void);
int  world_persists(void);          /* does anything written here survive? */
void world_row(int y, int z, unsigned char *row);
long world_volume(void);
int  world_edit_capacity(void);     /* -1 when edits are unbounded */

/* background save, driven from the idle path only */
int  world_needs_save(void);
int  world_saving(void);
void world_save_begin(void);
int  world_save_pump(void);

/*
 * ecgzip.c - level streaming.
 *
 * The level is pushed out incrementally: level_start() sets it up and
 * level_pump() emits a bounded slice per call, so a joining client cannot
 * freeze everyone else while a quarter megabyte is compressed and sent.
 * Only one client streams at a time, which keeps this to one buffer.
 */
int  level_start(int fd);
int  level_pump(void);      /* 1 = more to send, 0 = done, -1 = failed */
int  level_active(void);
void level_abort(void);

#endif /* ELKSCRAFT_H */
