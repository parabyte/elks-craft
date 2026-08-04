/*
 * elks-craft - the ELKS Minecraft server
 *
 * Speaks protocol 7, which is what the ClassiCube client uses.  Packet
 * sizes follow the client's own table in src/Protocol.c rather than the
 * wiki, which has several of them wrong.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include "elkscraft.h"

struct player players[MAX_PLAYERS];
int world_w = DEF_WIDTH, world_h = DEF_HEIGHT, world_l = DEF_LENGTH;

static char srv_name[MC_STRLEN + 1] = "elks-craft";
static char srv_motd[MC_STRLEN + 1] = "The ELKS Minecraft Server";
static int opt_foreground;

/* ------------------------------------------------------------- utilities */

/*
 * Startup reporting.  Generating or loading a quarter of a million blocks
 * takes the better part of a minute on a 4.77MHz machine, and a boot script
 * that prints nothing for that long looks exactly like one that has hung.
 * These go to stderr, which is still the console until the daemon has
 * finished building the world.
 */
void errmsg(const char *str)
{
    write(STDERR_FILENO, str, strlen(str));
}

/* decimal; this program does not link printf */
void errnum(long v)
{
    char tmp[12];
    int i = 0;

    if (v < 0) {
        errmsg("-");
        v = -v;
    }
    if (v == 0)
        tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i > 0)
        write(STDERR_FILENO, &tmp[--i], 1);
}

/*
 * The player sockets are non-blocking so that reads never stall the server
 * (see the poll loop in main), which means a write can come back EAGAIN the
 * moment the peer's receive window fills - and with a 1800 byte window that
 * happens on the second chunk of every level stream.  EAGAIN is not an
 * error here, it is "ask again": retrying restores exactly the behaviour a
 * blocking write had, where the kernel did this waiting for us.
 */
int write_all(int fd, const unsigned char *buf, int len)
{
    int n;

    while (len > 0) {
        n = write(fd, buf, len);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

/* fixed 64 byte field, padded with spaces */
static void mcstr(unsigned char *dst, const char *src)
{
    int i = 0;

    while (i < MC_STRLEN && src[i]) {
        dst[i] = (unsigned char)src[i];
        i++;
    }
    while (i < MC_STRLEN)
        dst[i++] = ' ';
}

static void mcstr_get(char *dst, const unsigned char *src)
{
    int i;

    memcpy(dst, src, MC_STRLEN);
    dst[MC_STRLEN] = '\0';
    for (i = MC_STRLEN - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = '\0';
}

/* append to a fixed size buffer, never overrunning it */
static void catn(char *dst, const char *src, int size)
{
    int i = (int)strlen(dst);

    while (i < size - 1 && *src)
        dst[i++] = *src++;
    dst[i] = '\0';
}

static int rd16(const unsigned char *p)
{
    return (int)(short)(((unsigned int)p[0] << 8) | p[1]);
}

static void wr16(unsigned char *p, int v)
{
    p[0] = (unsigned char)(((unsigned int)v) >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static int pid_of(struct player *p)
{
    return (int)(p - players);
}

/*
 * A socket is left non-blocking only for as long as it takes the client to
 * identify itself, and is put back to blocking the moment it has.
 *
 * Both halves of that matter.  Non-blocking is needed at the start because
 * ELKS counts readable data per socket and does not count anything that
 * arrived before accept() finished, so a client that sends its login in the
 * same breath as the connection is never reported readable and would sit
 * there until the join timeout: the handshake has to be polled, not waited
 * for.  Blocking has to come back before the level stream, because a
 * non-blocking socket never completes a large write here at all - the
 * kernel's write path retries internally and simply never succeeds.
 */
static int set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
}

/* ---------------------------------------------------------- packet sends */

void send_message(int fd, const char *msg)
{
    unsigned char pkt[66];

    pkt[0] = S_MESSAGE;
    pkt[1] = MC_SELF_ID;
    mcstr(pkt + 2, msg);
    write_all(fd, pkt, sizeof(pkt));
}

static void send_kick(int fd, const char *why)
{
    unsigned char pkt[65];

    pkt[0] = S_KICK;
    mcstr(pkt + 1, why);
    write_all(fd, pkt, sizeof(pkt));
}

static void send_spawn(int fd, int id, struct player *who)
{
    unsigned char pkt[74];

    pkt[0] = S_SPAWN;
    pkt[1] = (unsigned char)id;
    mcstr(pkt + 2, who->name);
    wr16(pkt + 66, who->x);
    wr16(pkt + 68, who->y);
    wr16(pkt + 70, who->z);
    pkt[72] = who->yaw;
    pkt[73] = who->pitch;
    write_all(fd, pkt, sizeof(pkt));
}

/* returns <0 if the recipient has gone away, so the caller can flag it */
static int write_all_teleport(struct player *to, int id, struct player *who)
{
    unsigned char pkt[10];

    pkt[0] = S_TELEPORT;
    pkt[1] = (unsigned char)id;
    wr16(pkt + 2, who->x);
    wr16(pkt + 4, who->y);
    wr16(pkt + 6, who->z);
    pkt[8] = who->yaw;
    pkt[9] = who->pitch;
    return write_all(to->fd, pkt, sizeof(pkt));
}

/* send to every player in the game, optionally skipping one */
static void broadcast(const unsigned char *pkt, int len, struct player *skip)
{
    struct player *p;

    for (p = players; p < players + MAX_PLAYERS; p++) {
        if (p->state != PS_PLAY || p == skip)
            continue;
        if (write_all(p->fd, pkt, len) < 0)
            p->dead = 1;        /* gone; reaped at the top of the next turn */
    }
}

static void broadcast_message(const char *msg, struct player *skip)
{
    unsigned char pkt[66];

    pkt[0] = S_MESSAGE;
    pkt[1] = MC_SELF_ID;
    mcstr(pkt + 2, msg);
    broadcast(pkt, sizeof(pkt), skip);
}

/* ------------------------------------------------------- join-time edits */

/*
 * The level a joining client receives is a snapshot taken row by row over
 * many seconds, so anything edited behind the streamer's back never reaches
 * them - their world stays wrong until they reconnect.  Edits made while a
 * stream is running are logged here and replayed as SetBlock packets the
 * moment the joiner is in the world.
 *
 * The log is keyed by position, so a block worked on repeatedly costs one
 * slot rather than one per change, and 128 slots is far more than a join
 * window realistically sees.  Overflow is reported to the joiner rather than
 * passed off silently.
 */
#define EDITLOG_MAX     128

static struct editrec {
    unsigned char x, y, z, b;
} editlog[EDITLOG_MAX];
static int editlog_n;
static int editlog_lost;

static void editlog_reset(void)
{
    editlog_n = 0;
    editlog_lost = 0;
}

static void editlog_add(int x, int y, int z, unsigned char blk)
{
    struct editrec *e;

    for (e = editlog; e < editlog + editlog_n; e++) {
        if (e->x == (unsigned char)x && e->y == (unsigned char)y &&
            e->z == (unsigned char)z) {
            e->b = blk;
            return;
        }
    }
    if (editlog_n >= EDITLOG_MAX) {
        editlog_lost = 1;
        return;
    }
    e = &editlog[editlog_n++];
    e->x = (unsigned char)x;
    e->y = (unsigned char)y;
    e->z = (unsigned char)z;
    e->b = blk;
}

static void editlog_replay(struct player *p)
{
    unsigned char out[8];
    struct editrec *e;

    for (e = editlog; e < editlog + editlog_n; e++) {
        out[0] = S_SET_BLOCK;
        wr16(out + 1, e->x);
        wr16(out + 3, e->y);
        wr16(out + 5, e->z);
        out[7] = e->b;
        if (write_all(p->fd, out, sizeof(out)) < 0)
            return;
    }
    if (editlog_lost)
        send_message(p->fd, "&eWorld changed while loading; reconnect to resync");
}

/* ------------------------------------------------------------ join/leave */

static void player_drop(struct player *p, const char *why)
{
    unsigned char pkt[2];
    char line[MC_STRLEN + 1];

    if (p->state == PS_FREE)
        return;
    if (why)
        send_kick(p->fd, why);

    if (p->state == PS_PLAY) {
        pkt[0] = S_DESPAWN;
        pkt[1] = (unsigned char)pid_of(p);
        broadcast(pkt, sizeof(pkt), p);

        line[0] = '\0';
        catn(line, "- ", sizeof(line));
        catn(line, p->name, sizeof(line));
        catn(line, " left", sizeof(line));
        broadcast_message(line, p);
    }
    close(p->fd);
    p->fd = -1;
    p->state = PS_FREE;
    p->inlen = 0;
    p->dead = 0;
}

/*
 * Identification accepted.  The level itself is not sent here: the player
 * is parked in the queue and the streamer feeds it out a slice at a time
 * from the main loop, so joining does not stall anyone already in game.
 */
static void player_identify(struct player *p, const unsigned char *pkt)
{
    unsigned char out[131];
    int len = 131;

    /* handshake done: back to blocking before anything large is written */
    set_nonblock(p->fd, 0);

    p->proto = pkt[1];
    mcstr_get(p->name, pkt + 2);
    if (p->name[0] == '\0')
        strcpy(p->name, "player");
    p->name[24] = '\0';         /* keep chat lines within a 64 byte field */

    out[0] = S_IDENT;
    out[1] = MC_PROTOCOL_VER;
    mcstr(out + 2, srv_name);
    mcstr(out + 66, srv_motd);
    out[130] = 0x64;            /* op, so the client lets you place any block */
    /*
     * Clients older than 0.0.20a read a 130 byte handshake with no user type
     * on the end.  Send them 131 and they take our 0x64 for an opcode, fail
     * to find a handler for it and hang up before the level is ever asked
     * for; the login they sent us is 131 bytes either way, so the version
     * byte is the only thing that tells them apart.
     */
    if (p->proto <= 5)
        len = 130;
    if (write_all(p->fd, out, len) < 0) {
        player_drop(p, (char *)0);
        return;
    }
    p->state = PS_QUEUE;
}

/* the level has finished streaming: put the player into the world */
static void player_spawn_in(struct player *p)
{
    unsigned char out[7];
    struct player *q;
    char line[MC_STRLEN + 1];
    int i;

    out[0] = S_LEVEL_FINAL;
    wr16(out + 1, world_w);
    wr16(out + 3, world_h);
    wr16(out + 5, world_l);
    if (write_all(p->fd, out, 7) < 0) {
        player_drop(p, (char *)0);
        return;
    }

    /* anything that changed behind the streamer's back, before they look */
    editlog_replay(p);

    world_spawn(&p->x, &p->y, &p->z);
    p->yaw = 0;
    p->pitch = 0;
    p->state = PS_PLAY;

    /* our own entity, then everyone else's, then us to everyone else */
    send_spawn(p->fd, MC_SELF_ID, p);
    for (i = 0; i < MAX_PLAYERS; i++) {
        q = &players[i];
        if (q == p || q->state != PS_PLAY)
            continue;
        send_spawn(p->fd, i, q);
        send_spawn(q->fd, pid_of(p), p);
    }

    line[0] = '\0';
    catn(line, "+ ", sizeof(line));
    catn(line, p->name, sizeof(line));
    catn(line, " joined", sizeof(line));
    broadcast_message(line, (struct player *)0);
}

/* --------------------------------------------------------- packet inputs */

static void do_setblock(struct player *p, const unsigned char *pkt)
{
    unsigned char out[8];
    int x = rd16(pkt + 1), y = rd16(pkt + 3), z = rd16(pkt + 5);
    int mode = pkt[7];
    unsigned char blk = pkt[8];

    if (mode == 0)
        blk = BLK_AIR;
    else if (blk > BLK_MAX)
        blk = BLK_STONE;

    /* outside the world is not an error worth telling anyone about */
    if (x < 0 || y < 0 || z < 0 || x >= world_w || y >= world_h || z >= world_l)
        return;

    if (!world_setblock(x, y, z, blk)) {
        /* out of edit slots: tell the client what is really there so its
         * local prediction gets corrected */
        send_message(p->fd, "&cEdit table full, block reverted");
        blk = world_block(x, y, z);
    }

    /* someone is mid-join and will not see this in their snapshot */
    if (level_active())
        editlog_add(x, y, z, blk);

    out[0] = S_SET_BLOCK;
    wr16(out + 1, x);
    wr16(out + 3, y);
    wr16(out + 5, z);
    out[7] = blk;
    /* everyone, including the sender, whose prediction may have been wrong */
    broadcast(out, sizeof(out), (struct player *)0);
}

static void do_position(struct player *p, const unsigned char *pkt)
{
    struct player *q;
    int x = rd16(pkt + 2), y = rd16(pkt + 4), z = rd16(pkt + 6);

    /* pkt[1] is the held block under CPE, the self id otherwise; unused */

    /*
     * ClassiCube sends position every tick whether or not anything moved.
     * Fanning those out unchanged costs (players-1) blocking writes several
     * times a second for a room full of people standing still, which on a
     * 4.77MHz machine is most of the CPU budget.
     */
    if (x == p->x && y == p->y && z == p->z &&
        pkt[8] == p->yaw && pkt[9] == p->pitch)
        return;

    p->x = x;
    p->y = y;
    p->z = z;
    p->yaw = pkt[8];
    p->pitch = pkt[9];

    /* absolute teleports rather than relative deltas: a few more bytes on
     * the wire, but no accumulated drift to worry about */
    for (q = players; q < players + MAX_PLAYERS; q++) {
        if (q->state != PS_PLAY || q == p)
            continue;
        if (write_all_teleport(q, pid_of(p), p) < 0)
            q->dead = 1;
    }
}

static void do_message(struct player *p, const unsigned char *pkt)
{
    unsigned char out[66];
    char text[MC_STRLEN + 1];
    char line[MC_STRLEN + 1];

    mcstr_get(text, pkt + 2);

    line[0] = '\0';
    catn(line, p->name, sizeof(line));
    catn(line, ": ", sizeof(line));
    catn(line, text, sizeof(line));

    out[0] = S_MESSAGE;
    out[1] = (unsigned char)pid_of(p);
    mcstr(out + 2, line);
    broadcast(out, sizeof(out), (struct player *)0);
}

static int packet_size(int op)
{
    switch (op) {
    case C_IDENT:       return 131;
    case C_PING:        return 1;
    case C_SET_BLOCK:   return 9;
    case C_POSITION:    return 10;
    case C_MESSAGE:     return 66;
    }
    return -1;
}

static void player_read(struct player *p)
{
    int n, need;

    n = read(p->fd, p->in + p->inlen, (int)sizeof(p->in) - p->inlen);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        player_drop(p, (char *)0);
        return;
    }
    p->inlen += n;

    /* a packet may arrive split, or several may arrive together */
    while (p->inlen > 0) {
        need = packet_size(p->in[0]);
        if (need < 0) {
            player_drop(p, "Unknown packet");
            return;
        }
        if (p->inlen < need)
            return;

        switch (p->in[0]) {
        case C_IDENT:
            if (p->state == PS_HELLO)
                player_identify(p, p->in);
            break;
        case C_PING:
            break;
        case C_SET_BLOCK:
            if (p->state == PS_PLAY)
                do_setblock(p, p->in);
            break;
        case C_POSITION:
            if (p->state == PS_PLAY)
                do_position(p, p->in);
            break;
        case C_MESSAGE:
            if (p->state == PS_PLAY)
                do_message(p, p->in);
            break;
        }
        if (p->state == PS_FREE)        /* dropped while handling */
            return;

        p->inlen -= need;
        if (p->inlen > 0)
            memmove(p->in, p->in + need, p->inlen);
    }
}

/* -------------------------------------------------------------- main */

static void usage(void)
{
    errmsg("usage: elkscraft [-f] [-p port] [-w width] [-h height]"
           " [-l length] [-s seed] [-n name] [-m motd] [-d worldfile]\n");
    exit(1);
}

static int stopping;

/* ELKS resets a handler once it fires, so re-arm or a second TERM kills us
 * in the middle of writing the world back out */
static void on_term(int sig)
{
    signal(sig, on_term);
    stopping = 1;
}

/* which player the streamer is currently feeding, if any */
static struct player *loader;

static void loader_check(void)
{
    struct player *p;
    int r;

    /* the client being streamed to went away: throw the stream out */
    if (loader && loader->state != PS_LOAD) {
        level_abort();
        loader = (struct player *)0;
    }

    if (level_active()) {
        r = level_pump();
        if (r < 0) {
            if (loader)
                player_drop(loader, (char *)0);
            loader = (struct player *)0;
        } else if (r == 0) {
            p = loader;
            loader = (struct player *)0;
            if (p && p->state == PS_LOAD)
                player_spawn_in(p);
        }
        return;
    }

    /* streamer is free: start the next player waiting for a level */
    for (p = players; p < players + MAX_PLAYERS; p++) {
        unsigned char op = S_LEVEL_INIT;

        if (p->state != PS_QUEUE)
            continue;
        editlog_reset();
        if (write_all(p->fd, &op, 1) < 0 || level_start(p->fd) < 0) {
            player_drop(p, (char *)0);
            return;
        }
        p->state = PS_LOAD;
        loader = p;
        return;
    }
}

int main(int argc, char **argv)
{
    int listen_sock, conn, ret, i, maxfd, busy, slots;
    unsigned int seed = 1;
    int port = MC_PORT;
    const char *worldfile = (const char *)0;
    struct sockaddr_in addr, peer;
    socklen_t addrlen;
    struct player *p;
    fd_set rfds;
    struct timeval tv;
    unsigned char ping = S_PING;
    unsigned long now = 0;
    time_t last_ping = 0, last_save = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][2] != '\0')
            usage();
        if (argv[i][1] == 'f') {
            opt_foreground = 1;
            continue;
        }
        if (i + 1 >= argc)
            usage();
        switch (argv[i][1]) {
        case 'p': port = atoi(argv[++i]); break;
        case 'w': world_w = atoi(argv[++i]); break;
        case 'h': world_h = atoi(argv[++i]); break;
        case 'l': world_l = atoi(argv[++i]); break;
        case 's': seed = (unsigned int)atoi(argv[++i]); break;
        case 'n': strncpy(srv_name, argv[++i], MC_STRLEN); break;
        case 'm': strncpy(srv_motd, argv[++i], MC_STRLEN); break;
        case 'd': worldfile = argv[++i]; break;
        default: usage();
        }
    }

    if (world_w < WORLD_MIN || world_w > WORLD_MAX ||
        world_h < WORLD_MIN || world_h > WORLD_MAX ||
        world_l < WORLD_MIN || world_l > WORLD_MAX) {
        errmsg("elkscraft: world dimensions must be 16..255\n");
        return 1;
    }
    if (world_volume() > WORLD_MAXVOL) {
        errmsg("elkscraft: world too large\n");
        return 1;
    }

    for (i = 0; i < MAX_PLAYERS; i++) {
        players[i].fd = -1;
        players[i].state = PS_FREE;
    }

    /* a client that vanishes mid-write must not take the server with it */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        errmsg("elkscraft: network is down\n");
        return 1;
    }
    ret = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &ret, sizeof(int)) < 0)
        errmsg("elkscraft: SO_REUSEADDR\n");
    ret = SO_LISTEN_BUFSIZ;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_RCVBUF, &ret, sizeof(int)) < 0)
        errmsg("elkscraft: SO_RCVBUF\n");

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        errmsg("elkscraft: bind failed (already running?)\n");
        return 1;
    }
    if (listen(listen_sock, MAX_PLAYERS) < 0) {
        errmsg("elkscraft: listen failed\n");
        return 1;
    }
    /*
     * The listening socket is polled, never selected on.  ELKS answers
     * select-for-read on any socket that is not SS_CONNECTED, and a
     * listening socket never is, so it reports ready on every call whether
     * or not a connection is waiting.  Putting it in the read set therefore
     * turns the loop into "select returns instantly, then block forever in
     * accept()", which stops every other player being served.  Non-blocking
     * accept() once per turn is correct and costs nothing.
     */
    if (fcntl(listen_sock, F_SETFL, O_NONBLOCK) < 0) {
        errmsg("elkscraft: cannot set the listening socket non-blocking\n");
        return 1;
    }

    errmsg("elkscraft: listening on port ");
    errnum(port);
    errmsg(", up to ");
    errnum(MAX_PLAYERS);
    errmsg(" players\n");

    /*
     * Daemonise before building the world, not after.  Generating or
     * loading a quarter million blocks takes real time on a 4.77MHz XT,
     * and doing it in the foreground would stall the boot script that
     * started us.  The listening socket is already open at this point, so
     * clients that connect during generation simply wait in the backlog.
     */
    if (!opt_foreground) {
        if ((ret = fork()) == -1) {
            errmsg("elkscraft: no more processes\n");
            return 1;
        }
        if (ret)
            exit(0);
        setsid();
    }

    /* stderr is still the console here, so progress and failures are visible */
    errmsg("elkscraft: world ");
    errnum(world_w);
    errmsg("x");
    errnum(world_h);
    errmsg("x");
    errnum(world_l);
    errmsg(" (");
    errnum(world_volume());
    errmsg(" blocks)\n");

    if (!world_init(seed, worldfile)) {
        errmsg("elkscraft: not enough memory for the world\n");
        return 1;
    }

    errmsg("elkscraft: ");
    errmsg(world_mode());
    if (world_edit_capacity() >= 0) {
        errmsg(", ");
        errnum(world_edit_capacity());
        errmsg(" edit slots");
    }
    if (worldfile) {
        errmsg(", saving to ");
        errmsg(worldfile);
    } else
        errmsg(", not saved (no -d)");
    errmsg("\nelkscraft: ready\n");

    /*
     * Point stderr at /dev/null but close stdin and stdout outright: a
     * server has no use for them, and on ELKS every descriptor freed is
     * one more player who can be in the world (NR_OPEN is 20).
     */
    if (!opt_foreground) {
        ret = open("/dev/null", O_RDWR);
        dup2(ret, 2);
        if (ret > 2)
            close(ret);
        close(0);
        close(1);
    }

    for (;;) {
        FD_ZERO(&rfds);
        maxfd = 0;
        slots = 0;
        for (i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].state == PS_FREE) {
                slots++;
                continue;
            }
            FD_SET(players[i].fd, &rfds);
            if (players[i].fd > maxfd)
                maxfd = players[i].fd;
        }

        /*
         * When there is background work pending we poll rather than sleep,
         * so packets and level slices interleave.  Otherwise sleep for a
         * second and let the machine idle.
         */
        busy = level_active() || world_saving();
        if (maxfd == 0) {
            /*
             * Nobody is connected, so there is nothing to wait on: the
             * listening socket is polled below rather than selected on.
             * Sleeping here rather than calling select() with an empty set
             * keeps the idle machine idle and avoids depending on what
             * select() does with no descriptors at all.
             */
            if (!busy)
                sleep(1);
            ret = 0;
        } else {
            tv.tv_sec = busy ? 0 : 1;
            tv.tv_usec = 0;
            ret = select(maxfd + 1, &rfds, (fd_set *)0, (fd_set *)0, &tv);
            if (ret < 0 && errno != EINTR) {
                errmsg("elkscraft: select failed\n");
                return 1;
            }
        }

        /*
         * Shutting down is the one place a long disk write is acceptable:
         * there is no gameplay left to protect.
         */
        if (stopping) {
            if (world_needs_save())
                world_save_begin();
            while (world_save_pump())
                ;
            return 0;
        }
        if (ret < 0)
            continue;

        now = (unsigned long)time((time_t *)0);

        /*
         * Players still shaking hands are polled rather than waited on: what
         * they sent before accept() completed is not counted as readable by
         * ELKS and select() would never flag it (see set_nonblock).  Their
         * sockets are non-blocking, so this costs one EAGAIN per idle
         * connection.  Everyone else is read only when select() says so, on
         * a blocking socket, which is what the level streamer needs.
         */
        for (i = 0; i < MAX_PLAYERS; i++) {
            p = &players[i];
            if (p->state == PS_FREE)
                continue;
            if (p->state == PS_HELLO || (ret > 0 && FD_ISSET(p->fd, &rfds)))
                player_read(p);
        }

        /*
         * Players a write has already failed on are gone: the peer's FIN put
         * the socket in CLOSE_WAIT and ktcp answered EPIPE.  Let go of them
         * now rather than waiting for a read to return end of file, so the
         * connection stops being written to on every ping and its ktcp buffer
         * comes back.  No kick packet - there is nobody left to read it.
         */
        for (i = 0; i < MAX_PLAYERS; i++) {
            p = &players[i];
            if (p->state != PS_FREE && p->dead)
                player_drop(p, (char *)0);
        }

        /*
         * A connection that stalls part way through joining would otherwise
         * hold its slot for ever, and slots are the scarcest thing here.
         */
        for (i = 0; i < MAX_PLAYERS; i++) {
            p = &players[i];
            if (p->state != PS_FREE && p->state != PS_PLAY &&
                now - p->since >= JOIN_TIMEOUT)
                player_drop(p, "Join timed out");
        }

        /* keepalive, which is also how dead clients get noticed */
        if (now - last_ping >= 5) {
            last_ping = now;
            broadcast(&ping, 1, (struct player *)0);
        }

        /* one slice of level streaming per turn round the loop */
        loader_check();

        /*
         * Disk work happens only when nobody is mid-join, and only a few KB
         * at a time.  On an MFM drive a full save takes a while; spreading
         * it out means players never feel it.
         */
        if (!level_active()) {
            if (world_saving())
                world_save_pump();
            else if (world_needs_save() && now - last_save >= 60) {
                last_save = now;
                world_save_begin();
            }
        }

        /*
         * Take at most one new connection per turn, and only while there is
         * a slot for it.  When full, pending clients stay in the backlog
         * until a slot frees, which beats burning a descriptor on kicking
         * them.
         */
        if (slots) {
            addrlen = sizeof(peer);
            conn = accept(listen_sock, (struct sockaddr *)&peer, &addrlen);
            if (conn < 0) {
                if (errno == ENOTSOCK)
                    return 1;
                continue;       /* nothing waiting, or out of descriptors */
            }
            for (i = 0; i < MAX_PLAYERS; i++)
                if (players[i].state == PS_FREE)
                    break;
            if (i == MAX_PLAYERS) {
                send_kick(conn, "Server is full");
                close(conn);
                continue;
            }
            /* polled until identified, then blocking again */
            if (set_nonblock(conn, 1) < 0) {
                close(conn);
                continue;
            }
            p = &players[i];
            p->fd = conn;
            p->state = PS_HELLO;
            p->inlen = 0;
            p->dead = 0;
            p->proto = MC_PROTOCOL_VER;
            p->since = now;
            p->name[0] = '\0';
        }
    }
}
