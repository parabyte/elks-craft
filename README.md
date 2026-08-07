# elks-craft

**The ELKS Minecraft server.**

Map designed by Void of irc

A Minecraft Classic 0.30 server that runs on [ELKS](https://github.com/ghaerr/elks),
the 16-bit Unix for 8086-class machines. It serves protocol 7 — what the
[ClassiCube](https://www.classicube.net/) client speaks — so you can point a
modern client at a 1986 PC and walk around a world it is generating and
streaming to you out of a 640K address space.

Developed against and running on an **Amstrad PC1640**: 8086, 640K RAM, a
WD1002A-WX1 MFM hard disk, and an SMC Ultra ethernet card.

```
elkscraft: listening on port 25565, up to 4 players
elkscraft: world 48x24x48 (55296 blocks)
elkscraft: generating ........................ trees
elkscraft: far memory, saving to /root/world.dat
elkscraft: ready
```

## Why Classic

Classic 0.30 suits a 16-bit machine far better than the modern protocol:
fixed-size packets, no NBT, no VarInts, no compression negotiation. The whole
wire format is a handful of structs and one gzip stream.

Packet sizes here follow ClassiCube's own table in `src/Protocol.c` rather than
the wiki, which has several of them wrong.

## Design

The machine sets the constraints, and they are the interesting part.

- **No malloc in the serving path.** A fixed array of player slots, a fixed
  packet buffer per player, one level streamer.
- **The world lives outside the 64K data segment.** `fmemalloc()` takes the
  block array from main memory and it is addressed through far pointers, with
  the index folded into the segment so the offset stays small. The default
  48x24x48 world is 54K — it could not otherwise exist at all.
- **Everything expensive is sliced.** The level is generated, compressed and
  pushed a bounded amount per turn round the select loop, so a joining client
  cannot freeze everyone already in the world. Disk writes go out in 1K slices
  from the idle path. No packet handler ever waits on the drive.
- **The level is compressed without a compressor.** `ecgzip.c` emits a single
  fixed-Huffman deflate block and finds matches by run length only: a run of N
  identical bytes becomes one literal plus length/distance-1 matches. Terrain is
  almost entirely long runs of air and stone, so it compresses hundreds to one
  for a couple hundred lines of code and no tables to speak of. The default
  55296-block world leaves as about 2.6K.
- **Connection slots are the scarcest resource, and the limit is not
  descriptors.** ELKS gives a process 20 descriptors, but the real ceiling is
  memory inside ktcp: it is linked with a 33792 byte heap and allocates 4470
  bytes per connection, so the whole machine has room for about seven at once,
  shared with telnetd and ftpd. The server caps itself well below that.
- **The default world size leaves the machine administrable.** It is chosen so
  there is still room to fork, not to be as large as will fit. `fork` copies a
  process's whole data segment, so logging in over telnet and running one
  command costs about 66K on top of the server's own 33K. Measured on a 640K
  PC1640 with 194K free: the 54K default leaves 107K, and `ps`, `kill` and
  `shutdown` all still work while the server runs. Ask for a 96K world with
  `-w`/`-l` and they stop working — the box then cannot even be upgraded
  without a power cycle.

## Building

You need an ELKS source tree — for its headers and libc — and the `ia16-elf`
cross toolchain that tree builds.

```sh
git clone https://github.com/ghaerr/elks
cd elks && ./build.sh          # builds the toolchain, then the system
cd ..
git clone https://github.com/parabyte/elks-craft
cd elks-craft
make ELKS_TOP=../elks
```

That produces `elkscraft`, an ELKS a.out binary. Nothing else needs to be on
`PATH`; the Makefile finds `cross/bin` and `elks/tools/bin` itself.

To build it as part of ELKS instead, drop the sources into `elkscmd/inet/`, add
an `elkscraft` rule to that Makefile and a `inet/elkscraft :net` line to
`elkscmd/Applications`.

## Installing and running

Copy `elkscraft` to `/bin` on the target, `chmod 755` it, and run it. It
daemonises immediately after it starts listening, then reports progress while
it builds the world — generation takes about twenty seconds on a real PC1640, and a
boot script that printed nothing for that long would look like it had hung.

```sh
elkscraft                      # 48x24x48 default, nothing persisted
elkscraft -d /root/world.dat   # persist to disk
elkscraft -w 96 -h 32 -l 96 -s 7 -n "my server"
```

| option | meaning | default |
| --- | --- | --- |
| `-f` | stay in the foreground | daemonise |
| `-p <port>` | listening port | 25565 |
| `-w -h -l` | world width, height, length (16..255) | 48, 24, 48 |
| `-s <seed>` | terrain seed | 1 |
| `-n <name>` | server name shown to clients | `elks-craft` |
| `-m <motd>` | message of the day | `The ELKS Minecraft Server` |
| `-d <file>` | world file to load at start and save to | none |

### Commands

| command | what it does |
| --- | --- |
| `/setspawn` | move the spawn point to where you are standing, facing the way you are looking |

Without one set, players arrive on top of the highest block in the middle of
the map. That is right for open terrain and wrong the moment anybody builds
there, since the roof is not where a visitor should turn up. `/setspawn` is
stored in the world file header and survives a restart, so it needs `-d`;
without it the server says so and the spawn reverts on the next start.

There is no operator model on a machine this size, so anyone in the world can
move the spawn, and everyone is told who did.

### Persistence

**Builds are only saved if you pass `-d`.** Without it the world lives in far
memory and is gone when the server stops.

With `-d`, edits mark the world dirty and it is written back in 1K slices from
the idle path, at most once a minute, plus a full write on shutdown. The save
goes to `<file>.new` and is renamed over the real file only once complete, so an
interrupted save cannot destroy the previous world.

The file starts with a short header — magic, dimensions, and the spawn point —
followed by the raw block array. Worlds written before `/setspawn` existed carry
the older `DCW1` magic and no spawn field; they still load, and simply have no
spawn set until one is placed.

Persistence needs the far-memory block array. If `fmemalloc()` cannot get it,
the server falls back to generating terrain on demand with a bounded table of
player edits — playable, but nothing is saved.

## Connecting

Use [**ClassiCube**](https://www.classicube.net/). It is the recommended
client, it is free, it runs on Windows, macOS, Linux, Android and more, and
protocol 7 is exactly what it speaks — no plugins or compatibility layers are
needed.

Point it at the server's address on port 25565, either through *Direct connect*
in the launcher or from the command line:

```sh
ClassiCube <username> <mppass> <server-address> 25565
```

`mppass` is unused here, so anything will do.

Two things worth knowing on a first run:

- ClassiCube needs its own base resources (`default.zip` and `classicube.zip`)
  before it will render properly. The launcher offers to download them the
  first time you start it; do that once before connecting. It will still run
  without them, just untextured.
- Joining takes a little while and that is expected. The world is generated,
  compressed and streamed by an 8086, so the default level takes around six
  seconds to arrive over ethernet. The server keeps serving everyone else while
  it happens.

Any other Minecraft Classic 0.30 client should work too, since nothing here is
ClassiCube-specific, but ClassiCube is what this is developed and tested
against.

## Notes on the platform

Things ELKS does that a server written the obvious way gets wrong. All of these
were found the hard way and are worked around in the code.

- **Never put a listening socket in a `select()` set.** `inet_select()` reports
  any socket that is not `SS_CONNECTED` as readable, and a listening socket
  never is — so `select()` returns immediately every pass and a following
  blocking `accept()` parks the whole process until the *next* connection. The
  listening socket here is non-blocking and polled.
- **`select()` cannot be trusted to report a socket readable at all.** It
  answers from a per-socket counter that does not count anything which arrived
  before `accept()` finished — so a client sending its login in the same breath
  as the connection is invisible for ever — and in practice it never flagged a
  player already in the world either. Waiting on it meant chat, block edits and
  movement were read from nobody once a player had joined, while the join
  itself worked perfectly, which is a nasty way for it to fail: everything
  server-to-client looks fine. Players are polled every turn instead.
- **`O_NONBLOCK` breaks large writes.** A multi-KB write on a non-blocking
  socket never completes — the process sleeps inside `write()` and the transfer
  simply stops. Reads therefore flip the flag on and straight back off around
  each `read()`, so writes always happen on a blocking socket. Sockets being
  streamed to are left out of the poll entirely: toggling the flag thousands of
  times a second on the connection the level streamer is writing to stalled the
  transfer outright.
- **Writes can block indefinitely and cannot be interrupted.** `inet_write()`
  retries forever with no signal check, so a peer that vanishes mid-write can
  wedge the process. There is no userland fix; all this code can do is bound how
  much it tries to push per turn (one 1028-byte chunk).
- **Too many connections kill the whole TCP stack, not just the server.** ktcp
  allocates 4470 bytes per connection from a 33792 byte heap, and it allocates
  on the arriving SYN, not on `accept()`, ignoring `SO_RCVBUF` for accepted
  sockets. Exhaust it and its `malloc` fails with
  `SBRK 4470 FAIL, OUT OF HEAP SPACE`, after which every TCP service on the
  machine is dead until ktcp is restarted — telnet and FTP included. A server
  cannot prevent the allocation, only hold few connections and drop non-players
  quickly, which is why `MAX_PLAYERS` is small and `JOIN_TIMEOUT` is short.

  **Do not expose this straight to the internet.** Port 25565 is scanned
  continuously, and a handful of simultaneous scanners is enough to take the
  machine off the network. If you want it publicly reachable, put something in
  front that caps concurrent connections to a couple and drops idle ones, so
  the 8086 only ever sees a trickle.
- **`rename()` cannot replace an existing file, and failing to allow for that
  cost every edit anyone ever made.** ELKS implements `rename()` as `link()`
  followed by `unlink()` (`fs/namei.c`), and `minix_link()` refuses a name that
  already exists with `EEXIST` — where POSIX would replace it. So the finished
  save could be renamed over the world file exactly once, when there was no
  world file yet, and never again. Because the dirty flag is deliberately left
  set when a save does not complete, the result was not one lost save but a
  server that rewrote the entire world to an MFM drive every sixty seconds for
  ever, persisted nothing, and eventually ground the machine to a halt. The
  destination is now unlinked first, which is what `mv`, `decomp` and every
  other program in the ELKS tree that renames over a file already does.

  The save is no longer atomic as a result, so the loader falls back to the
  `.new` file: at every instant at least one of the two names holds a complete
  world. And since what matters is that the world reached the disk rather than
  which name it landed under, the save counts as done even if the rename itself
  fails — otherwise a filesystem that cannot link at all, like FAT, would bring
  the sixty-second retry loop straight back.
- **`ia16-elf-gcc` 6.3 at `-Os` miscompiles some 32-bit values.** In
  `level_finish()` a `unsigned long` local was allocated a stack slot that was
  never written, and the gzip trailer went out as uninitialised stack — every
  level shipped with a different garbage checksum. Passing the value as an
  argument corrupted it differently, and reading it straight from the struct
  still got the `>> 16` byte wrong. The trailer is now emitted from a file-scope
  static, shifted 8 bits at a time. If a 32-bit value comes out wrong on ELKS
  but right on a host build, suspect codegen before logic.

Terrain differs slightly between a host build and ELKS, because the noise
function's arithmetic is 16-bit there. That is expected, not a bug.

## Files

| file | what it does |
| --- | --- |
| `src/elkscraft.c` | protocol, player state machine, select loop |
| `src/ecworld.c` | terrain generation, far-memory block array, save and load |
| `src/ecgzip.c` | level streaming as a fixed-Huffman deflate stream |
| `src/elkscraft.h` | protocol constants, server limits, shared declarations |

## Credit and licence

Derived in spirit from [cnlohr's dumbcraft](https://github.com/cnlohr/avrcraft),
which serves the modern protocol to tiny machines. The design ideas that carried
over: a fixed array of players, no malloc anywhere, a procedurally generated
world that is never held in RAM, and a small bounded table of edits layered on
top of it. The wire protocol and all of the code here are different.

GPL-2.0-or-later. See [LICENSE](LICENSE).
