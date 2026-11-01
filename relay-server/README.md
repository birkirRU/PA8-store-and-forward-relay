# Assignment 8: Store-and-forward relay (G_stammtischen)

Our Assignment 6 server and client, extended into a botnet relay: the server
exchanges messages with other groups' servers, holds messages for groups that are
not reachable, and passes on messages for groups further away.

| File               | Contents                                                                    |
|--------------------|-----------------------------------------------------------------------------|
| `server.cpp`       | Network layer: one epoll loop, accept/connect, buffered output, timer, peers |
| `server_cmds.cpp`  | Protocol layer: client commands (`c_*`) and server commands (`p_*`)          |
| `server.h`         | The `Server` class and its constants                                        |
| `client.cpp`       | The client: a poll loop over stdin and the server socket                    |
| `common.h/.cpp`    | Shared code: framing, command field splitting, name check, timestamps       |
| `identity.h`       | Our group ID, exactly as written by `tsam-register`                         |
| `log_event.h`      | The course's log writer, unchanged                                          |
| `server.log`       | Our complete log for the whole assignment                                   |
| `document/`        | The statement of authorship, and working notes from during the assignment (not kept up to date) |

## Compiling and running

```bash
make                         # builds tsamserver and tsamclient
./tsamserver 4106            # the listening port is the only argument
./tsamclient 127.0.0.1 4106  # the client talks to our own server only
```

There are no optional arguments or configuration files. The group ID comes only from
`identity.h`. The only hard-coded address is the first instructor server,
`130.208.246.98:5001`, which the server dials at startup to join the botnet.

The server appends to `server.log` in the directory it is started from, so the log
covers every run (each run starts with a `START` line). Stop the server with Ctrl-C (or
`kill`), which writes a final log line.

## Operating system

The server runs on **Linux only**, because it uses `epoll`, `timerfd`, `accept4`,
`SOCK_NONBLOCK` and `MSG_NOSIGNAL`. It was developed on Rocky Linux 9 (and edited on
macOS), and built, run and tested on the TSAM server (Ubuntu 24.04).

## Client commands

Commands and replies use the framing from the assignment. We read the 5000-byte limit as
applying to the command text between STX and ETX, so a frame can be up to 5005 bytes.

| Command                         | Reply                                                                                        |
|---------------------------------|----------------------------------------------------------------------------------------------|
| `SENDMSG,<from>,<to>,<message>` | `OK`. The server stamps the message with the current time and holds it (see Delivery)      |
| `GETMSG,<name>`                 | `<from>,<message>` (the oldest message for `<name>`, which is then deleted), or `NO_MESSAGE` |
| `STATUSREQ`                     | `<name1>:<count1>,<name2>:<count2>,...`, or `EMPTY`: everyone we hold messages for           |
| `LISTSERVERS`                   | Our own entry and the servers we dialled: the same list as our `SERVERS` reply (our addition) |
| anything else                   | `ERROR,command does not exist or is malformed`                                               |

- Group IDs take the place of user names. Messages addressed to our group are read
  with `GETMSG,G_stammtischen`.
- Names are 1-14 characters: ASCII letters, digits, `_` and `-`. This covers every
  `G_`/`I_` group ID and keeps `STATUSREQ` and `SERVERS` replies unambiguous, since a
  name cannot contain `,`, `:` or `;`.
- The message may contain commas and may be empty.
- Command names are case-sensitive, and commands take no extra fields: `STATUSREQ,x`
  is malformed.

## Server-to-server commands

| Command | We send it | When we receive it |
|---|---|---|
| `HELO,<id>` | First thing after our `connect()` | Record the ID, reply `SERVERS`, then send our own `HELO` if we accepted the connection |
| `SERVERS,<list>` | In reply to `HELO`: ourselves first, then the servers we dialled | Each entry is checked and added to our list of known servers. No reply |
| `KEEPALIVE,<n>` | Every 60 s to every peer, `n` = messages we hold for that peer | If `n > 0`, reply `GETMSGS,G_stammtischen` |
| `GETMSGS,<id>` | After a `KEEPALIVE,n>0`, or after a `STATUSRESP` (see Relaying) | One `SENDMSG` per message we hold for `<id>`, which are then deleted. Nothing if we hold none |
| `SENDMSG,<from>,<to>,<ts>,<text>` | In reply to `GETMSGS` | Stored under `<to>`, with the original timestamp. No reply |
| `STATUSREQ` | Every 90 s to every peer | Reply `STATUSRESP,<id>:<n>,...` or `STATUSRESP,EMPTY` |
| `STATUSRESP,<id>:<n>,...` | In reply to `STATUSREQ` | Fetch messages for us or for our connected peers (see Relaying). No reply |
| `ERROR,...` (not in the spec) | Never | Logged, never answered, so two servers cannot answer each other's errors forever |

Unknown or malformed commands from a server are logged as `DROP` and not answered.

## Server behaviour

### Connections
- **One port for everything.** Our client and other servers connect to the same port.
  The first command on a connection decides what it is: a server always starts with
  `HELO`, our client never does.
- **Many connections.** A single thread serves all connections with level-triggered
  `epoll`. The listening socket, every connection and a 5-second `timerfd` are all in
  one epoll set, so the server sleeps until something happens and uses no CPU while
  idle.
- **Outgoing connections** are non-blocking: `connect()` returns at once, and the
  connection counts as open when epoll reports it writable and `SO_ERROR` is 0.
- **Partial and joined frames.** TCP can split a frame across reads or join several
  frames together. Each connection has an input buffer, and a command is handled only
  once its whole frame has arrived. Several commands per connection are fine.
- **Slow readers.** Each connection has an output buffer. A reply that does not fit in
  the socket buffer is sent later, when the socket becomes writable. Other connections
  are not blocked.

### Staying connected
- Every 5 s the server counts its server connections (incoming, outgoing and still
  connecting). With fewer than **7**, it opens **one** new connection per tick to a server
  it knows from `SERVERS` replies, preferring student servers (`G_`) over instructors.
  The target was 4 at first and was raised to 7 on 2026-10-30: the log notes say
  `want 4` until 2026-10-30T16:04:06Z and `want 7` from 2026-10-30T16:07:41Z.
- It skips servers it is connected to, its own address, and any server it tried in the
  last 60 s. After an outgoing connection closes, failed or dropped, that server is not
  dialled again for 60 s, so a dead server cannot cause a reconnect loop.
- With no peers and nobody to dial, it connects to the first instructor again, at most
  once a minute.
- An incoming server whose `HELO` would make more than **8** peers is closed with a log note.
- `SERVERS` entries with an invalid ID, IP or port, and the addresses `0.0.0.0` and
  `127.x.x.x` (which some servers advertise, and which would reach our own machine),
  are ignored. At most 500 servers are remembered.

### Delivery (pull)
- A message from our client is stamped with the current time and stored under its
  recipient. It is not pushed: the recipient sees the count in our next `KEEPALIVE`
  and fetches it with `GETMSGS`.
- A `SENDMSG` from a server is accepted on any connection and stored under its
  recipient. Messages for us wait for our client's `GETMSG,G_stammtischen`.
- `GETMSGS` hands out every message held for that ID and deletes them. There is no
  acknowledgement in the protocol, so a message can be lost if a connection drops just
  after it was sent.

### Relaying
- Every 90 s we send `STATUSREQ` to each peer. If the `STATUSRESP` lists messages for us,
  or for a server we are connected to, we fetch them with `GETMSGS,<that id>`.
- Fetched messages for a connected peer are then held by us, announced in our next
  `KEEPALIVE` to that peer, and fetched by it. So a message can travel
  `A -> us -> B` even when A and B are not connected to each other.
- A peer that lists messages for itself is not asked for them: that is its own inbox.

### Malformed input and errors
- **Malformed commands.** Unknown commands, a wrong number of fields, invalid names or
  numbers and wrong case are rejected and logged as `DROP`. Our client gets
  `ERROR,command does not exist or is malformed`; other servers get no reply.
- **Numbers from other servers** (timestamps, counts, ports) are checked to be digits
  of a limited length before they are converted, so a huge number cannot crash the server.
- **Malformed frames.** Bytes outside a frame, a length below 5 or above 5005, and a
  missing STX or ETX are discarded without a reply. The server resynchronises on the
  next SOH and keeps serving the connection. Each discard is logged with its reason.
- **Storage limits.** At most 200 messages per recipient and 2000 in total. Messages
  beyond that are discarded with a log note, so a flooding server cannot exhaust memory.
- **Disconnects.** The connection's buffers are freed, and an incomplete frame left
  behind is logged. Its stored messages stay.
- **Errors.**
  - Startup failures make the server exit with status 1, with a message on stderr and
    in the log. Examples are a port already in use and socket or epoll errors.
  - An error on one connection (`recv`/`send` failure, connection reset) closes only
    that connection, and the reason is logged.
  - `EINTR` and `EAGAIN` are harmless and retried.
  - A peer that disappears never kills the server with SIGPIPE (`MSG_NOSIGNAL`).
  - When the server runs out of file descriptors, it uses a reserved descriptor to
    accept and immediately close the waiting connection. The listening socket therefore
    never makes the loop spin.
- Messages are kept in memory only and are lost when the server stops. The shutdown
  line in the log says how many.

## Protocol choices and assumptions

Where the protocol was unclear we followed what was agreed with the other groups on
Piazza:

- A server lists **itself first** in its `SERVERS` reply. Our own address is the local
  address the kernel uses to reach the first instructor; the listening socket itself is
  bound to `0.0.0.0`, which nobody can dial.
- Our `SERVERS` reply lists only servers we dialled. For incoming connections we only
  know the source port, not the port the server listens on.
- Delivery is **pull**: `KEEPALIVE,n>0` -> `GETMSGS` -> `SENDMSG`. Push is allowed by the
  class agreement, but we do not push.
- An empty answer to `GETMSGS` is **no frames at all**, not an `EMPTY` marker.
- `GETMSGS` with another group's ID is allowed (needed for relaying).
- A message's timestamp is set once, by the server that creates it, and kept unchanged
  by relays.
- Errors are never answered with `ERROR`.

## Server log

The log is written with the course's `log_event.h`, unchanged, in the required format:
`<TIMESTAMP> <EVENT> <IP:PORT> <GROUP_ID> <detail>`, with `log_note` lines
(`<TIMESTAMP> note ...`) for what the server decided and why. For example:

```
2026-10-30T15:17:55Z note 1 peer(s), want 4: connecting to G_christnik
2026-10-30T15:21:07Z ACCEPT 130.208.246.98:45976 - Unknown6 connected (6 connected)
2026-10-30T15:21:07Z RECV 130.208.246.98:45976 - HELO,G_minions
2026-10-30T16:10:34Z note stored message from G_bigbrother for G_stammtischen (1 waiting for them, 2 stored in total)
```

Notes record: peers we connect to and why, servers learned from `SERVERS`, rejected
entries, messages stored, sent and delivered with counts, discarded messages, slow
readers, the peer limit, and startup and shutdown.

About our log:
- It is the complete log of every run, from 2026-10-25 to the time of submission. We
  have not removed, reordered or edited any lines, and we do not suppress or summarise
  repeated events.
- Lines from 2026-10-25 and 2026-10-26 come from early versions: the first run rejected
  the instructor's `HELO` as `DROP` (2026-10-25T14:38:47Z), and `GETMSGS` was not
  implemented yet (2026-10-26T21:45:55Z).
- Two `SERVERS` lines contain garbage instead of our own entry
  (2026-10-29T21:59:26Z and 2026-10-29T23:14:19Z, `SERVERS,isten)0.0.0.0,...`). This was a
  bug in building the string, fixed the same night.
- The server that produced most of the log was started at 2026-10-30T17:33:36Z from a
  build without the filter for `0.0.0.0` and `127.x` entries in `SERVERS` lists. After
  that start it still tried to connect to `0.0.0.0` 12 times (the last at
  2026-10-30T21:23:36Z). The submitted code has the filter.
- The connection tag in notes and details (`Unknown6`, `Peer1`, `Client2`) is the
  connection type and our internal connection number, not a group ID. A connection is
  `Unknown` until its first command.
- The reply to `GETMSGS` (our `SENDMSG` lines) and the `GETMSGS` we send after a
  `STATUSRESP` are logged just *before* the `RECV` line of the command that caused them,
  in the same second: those handlers send while they run, and `RECV` is written after.

## Known limitations

- Messages do not expire and are not deduplicated.
- `GETMSGS` is answered for any group ID, so any server can collect the messages we
  hold for another group.
- A connection attempt to a server that never answers counts as a peer until the
  kernel gives up (about 2 minutes); there is no handshake timeout of our own.
- No per-IP rate limiting or temporary blocking.
- A reply longer than 5000 bytes cannot be framed. The server logs it and sends nothing.

## Client behaviour

- Prints every command sent and every reply received with a timestamp:
  ```
  2026-10-12 18:23:45 SENT: GETMSG,G_stammtischen
  2026-10-12 18:23:45 RECV: G_leo,Hello from G_leo
  ```
- Uses `poll` to wait on stdin and the server at the same time, so it never
  busy-waits. Incoming bytes are reassembled into complete frames.
- Skips empty lines and strips a trailing `\r`. A command longer than 5000 bytes is not
  sent, and an error is printed instead.
- When stdin ends (Ctrl-D, or the end of piped input), the client waits up to 5 seconds
  for the replies it is still owed, then exits. It also exits when the server closes the
  connection.
- Exit status is 0 on a normal end and 1 on errors: cannot connect, send or recv
  failure, or a missing reply.
- Stdout is line-buffered, so output redirected to a file appears immediately.

## Points claimed

All timestamps are in `server.log`.

### 1(a) Server-to-server protocol
Every command is both sent and handled. Examples:

| What | Timestamps |
|---|---|
| `HELO`/`SERVERS` on a connection we opened | 2026-10-30T15:17:50Z (`I_instr1`) |
| `KEEPALIVE,1` received, `GETMSGS,G_stammtischen` sent | 2026-10-30T15:27:00Z (`G_ASA`) |
| `GETMSGS` received, our `SENDMSG` sent | 2026-10-30T15:30:00Z (`G_ASA`, `GETMSGS,I_dispatch`) |
| `STATUSREQ` answered | 2026-10-30T15:19:00Z (`G_ASA`, `STATUSRESP,EMPTY`) |
| Peers kept and re-established | notes `connecting to ...` from 2026-10-30T15:17:55Z on |
| Peer limit enforced | 2026-10-31T20:40:59Z (`peer limit reached`) |

### 1(b) Connected to by another server
`G_minions` connected to us, all on 2026-10-30T15:21:07Z:
- `ACCEPT 130.208.246.98:45976`
- `RECV ... HELO,G_minions` (theirs)
- `SEND ... SERVERS,G_stammtischen,...` (ours)
- `SEND ... HELO,G_stammtischen` (ours)
- `RECV ... SERVERS,G_minions,...` (theirs)

Also `I_instr1` after a `TEST_ME`, all on 2026-10-30T16:55:52Z (same five lines,
`ACCEPT 130.208.246.98:60208`).

### 1(c) Messages received from 2 groups
| Group | `SENDMSG` arrives | Handed to our client (`GETMSG`) |
|---|---|---|
| `G_leo` | 2026-10-30T15:39:00Z | 2026-10-30T15:39:14Z |
| `G_bigbrother` | 2026-10-30T16:10:34Z | 2026-10-30T16:10:59Z |
| `G_nafn` | 2026-10-30T17:03:08Z | 2026-10-30T17:05:12Z |
| `G_wow` | 2026-10-30T17:13:08Z | 2026-10-30T17:15:27Z |
| `G_blueberries` | 2026-11-01T15:48:46Z | 2026-11-01T15:51:10Z |

### 1(d) Messages sent to 2 groups
| Group | Our client hands it to our server | Our server sends it |
|---|---|---|
| `G_bachata` | 2026-10-30T16:19:36Z | 2026-10-30T16:19:41Z |
| `G_ASA` | 2026-10-30T15:42:08Z | 2026-10-30T15:43:05Z |
| `G_G-Monster` | 2026-10-30T15:50:26Z | 2026-10-30T15:50:35Z |
| `G_hearts` | 2026-10-30T17:03:15Z | 2026-10-30T17:03:24Z |

### 1(e), 1(f)
Submission and log format as described above; code structure and log notes as
described under Server behaviour and Server log.

### Bonus: 2(e) Edge Bot
At least 3 student peers at once (`KEEPALIVE` traffic with each within the same minute):

| Time | Peers |
|---|---|
| 2026-10-30T15:18:05Z | `G_ASA`, `G_EastBot`, `G_christnik` |
| 2026-10-30T18:18:06Z | `G_EastBot`, `G_G`, `G_G-Monster` (7 student peers) |
| 2026-10-30T21:18:06Z | `G_EastBot`, `G_G`, `G_G-Monster` (7 student peers) |
| 2026-10-31T00:18:06Z | `G_EastBot`, `G_G`, `G_G-Monster` (8 student peers) |
| 2026-10-31T03:18:06Z | `G_EastBot`, `G_G`, `G_G-Monster` (7 student peers) |

### Bonus: 2(g) Courier Bot, dispatch 0 (Roll call)
| What | Timestamp |
|---|---|
| Dispatch arrives (relayed by `G_ASA`) | 2026-10-30T15:25:35Z |
| Our client hands our reply to our server | 2026-10-30T15:29:05Z |
| Our reply goes out (`G_ASA` fetches it for `I_dispatch`) | 2026-10-30T15:30:00Z |
| Credited by `I_dispatch` | 2026-10-30T15:31:35Z |

## Statement of authorship

Who wrote which part, and the tools and sources we used, are in
[`document/authorship.md`](document/authorship.md).
