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
| `identity.h`       | Our group ID, from `tsam-register`                         |
| `log_event.h`      | Given log writer                                          |
| `server.log`       | Our complete log for the whole assignment                                   |

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
macOS), and built, ran on the TSAM server (Ubuntu 24.04).

## Client commands


Client commands remain unchanged from A6, only a single command has been added.

| Command                         | Reply                                                                                        |
|---------------------------------|----------------------------------------------------------------------------------------------|
| `LISTSERVERS`                   | Our own entry and the servers we dialled: the same list as our `SERVERS` reply. |

- Group IDs take the place of user names. Messages addressed to our group are read
  with `GETMSG,G_stammtischen`.

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

## Server behaviour

### Connections
- **One port for everything.** Our client and other servers connect to the same port.
  The first command on a connection decides what it is: a server always starts with
  `HELO`, our client never does.

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
  - An error on one connection (`recv`/`send` failure, connection reset) closes only
    that connection, and the reason is logged.
  - A peer that disappears never kills the server with SIGPIPE (`MSG_NOSIGNAL`).

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

We used:

- `log_note()`: (`<TIMESTAMP> note ...`) for what the server decided and why.
- Other logging functions : strictly followed what `log_event.h` wanted.


Notes record: peers we connect to and why, servers learned from `SERVERS`, rejected
entries, messages stored, sent and delivered with counts, discarded messages, slow
readers, the peer limit, and startup and shutdown.

About our log:
-  We do not suppress or summarise repeated lines
- Lines from 2026-10-25 and 2026-10-26 come from early versions: the first run rejected
  the instructor's `HELO` as `DROP` (2026-10-25T14:38:47Z), and `GETMSGS` was not
  implemented yet (2026-10-26T21:45:55Z).
- Two `SERVERS` lines contain garbage instead of our own entry
  (2026-10-29T21:59:26Z and 2026-10-29T23:14:19Z, `SERVERS,isten)0.0.0.0,...`). This was a
  bug in building the string.
- The server that produced most of the log was started at 2026-10-30T17:33:36Z from a
  build without the filter for `0.0.0.0` and `127.x` entries in `SERVERS` lists. After
  that start it still tried to connect to `0.0.0.0` 12 times (the last at
  2026-10-30T21:23:36Z). The submitted code has the filter.
- The reply to `GETMSGS` (our `SENDMSG` lines) and the `GETMSGS` we send after a
  `STATUSRESP` are logged just *before* the `RECV` line of the command that caused them,
  in the same second: those handlers send while they run, and `RECV` is written after.

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

### Bonus: 2(c) Chatty Bot (10 groups)
Messages to us from 10 student groups, each also fetched by our client:

| Group | `SENDMSG` arrives | Handed to our client (`GETMSG`) |
|---|---|---|
| `G_leo` | 2026-10-30T15:39:00Z | 2026-10-30T15:39:14Z |
| `G_bigbrother` | 2026-10-30T16:10:34Z | 2026-10-30T16:10:59Z |
| `G_nafn` | 2026-10-30T17:03:08Z | 2026-10-30T17:05:12Z |
| `G_wow` | 2026-10-30T17:13:08Z | 2026-10-30T17:15:27Z |
| `G_TangoLango` | 2026-10-30T21:22:01Z | 2026-10-31T19:08:34Z |
| `G_blueberries` | 2026-11-01T15:48:46Z | 2026-11-01T15:51:10Z |
| `G_ellur` | 2026-11-01T16:45:24Z | 2026-11-02T09:46:57Z |
| `G_G` | 2026-11-01T17:23:58Z | 2026-11-02T09:47:07Z |
| `G_G-Monster` | 2026-11-01T22:33:46Z | 2026-11-02T09:52:22Z |
| `G_order66` | 2026-11-01T22:56:30Z | 2026-11-02T09:52:52Z |

### Bonus: 2(d) Indirect Bot, relaying
Messages where we are neither sender nor recipient. Each came in as a `SENDMSG` and went
out in reply to a `GETMSGS` for its recipient, from the recipient itself or a server
relaying for it:

| Message | In (from server) | Out (to server) |
|---|---|---|
| `G_nafn` -> `G_EastBot` | 2026-10-30T21:45:06Z (`G_kleina`) | 2026-10-30T21:48:11Z (`G_GOATS`) |
| `G_wow` -> `G_EastBot` | 2026-10-30T21:46:36Z (`G_kleina`) | 2026-10-30T21:48:11Z (`G_GOATS`) |
| `G_nafn` -> `G_EastBot` | 2026-10-30T21:46:36Z (`G_kleina`) | 2026-10-30T21:48:11Z (`G_GOATS`) |
| `G_blueberries` -> `G_NettNet` | 2026-11-01T15:49:01Z (`G_blueberries`) | 2026-11-01T15:49:16Z (`G_NettNet`) |
| `G_blueberries` -> `G_EastBot` | 2026-11-01T15:49:01Z (`G_blueberries`) | 2026-11-01T15:50:09Z (`G_NettNet`) |
| `G_G-Monster` -> `G_mjolk` | 2026-11-01T22:58:36Z (`G_G-Monster`) | 2026-11-01T22:58:46Z (`G_mjolk`) |
| `I_oracle` -> `G_bachata` | 2026-11-01T23:04:36Z (`G_G-Monster`) | 2026-11-01T23:04:46Z (`G_mjolk`) |
| `G_G-Monster` -> `G_mjolk` | 2026-11-01T23:48:31Z (`G_G-Monster`) | 2026-11-01T23:48:46Z (`G_mjolk`) |
| `G_G-Monster` -> `G_unit` | 2026-11-02T02:21:41Z (`G_G-Monster`) | 2026-11-02T02:22:01Z (`G_unit`) |
| `G_G-Monster` -> `G_unit` | 2026-11-02T04:28:26Z (`G_G-Monster`) | 2026-11-02T04:28:40Z (`G_G`) |
| `G_G-Monster` -> `G_unit` | 2026-11-02T07:23:56Z (`G_G-Monster`) | 2026-11-02T07:24:01Z (`G_unit`) |
| `G_G-Monster` -> `G_unit` | 2026-11-02T09:53:56Z (`G_G-Monster`) | 2026-11-02T09:54:06Z (`G_unit`) |

### Bonus: 2(d) Indirect Bot, patience
Messages our server accepted for a group it could not reach, held, and delivered later:

| Message | Stored | Recipient unreachable | Delivered |
|---|---|---|---|
| `G_stammtischen` -> `G_TangoLango` | 2026-10-31T19:11:17Z | last `CLOSE` 2026-10-31T05:19:25Z; `connect failed: Connection refused` 2026-11-02T11:42:36Z and 2026-11-02T16:03:11Z | 2026-11-02T16:27:41Z (`G_TangoLango` reconnected at 16:27:39Z, `sent 1 held message(s)`) |
| `G_stammtischen` -> `G_wow` | 2026-10-30T17:16:19Z | no connection to `G_wow`; still waiting at 2026-10-30T17:17:39Z and 2026-10-30T20:33:54Z | 2026-10-30T21:50:31Z (`GETMSGS,G_wow` from `G_kleina`) |
| `G_stammtischen` -> `G_leo` | 2026-10-30T15:39:51Z | no connection to `G_leo` until delivery | 2026-10-31T19:10:34Z (`GETMSGS,G_leo` from `G_blueberries`) |

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

### Birkir:
| Part | Files | Work |
|---|---|---|
| Server architecture and command handling | `server.h`, `server.cpp`, `server_cmds.cpp`, `Makefile` | Restructured the server, separated client and peer commands, and added automatic connection type detection based on the first command. |
| Outgoing peer connection | `server.cpp` | `connect_peer()` (non-blocking connect to the instructor server) and `finish_connect()` (sends `HELO`). |
| Server list | `server.cpp`, `server_cmds.cpp` | `servers_list()`, parsing `SERVERS,...` entries, storing them in `known_` (capped at 500). |
| Ticking function | `server.cpp` | `timerfd` created in the constructor, added to epoll, and handled by `on_timer()` (sends `KEEPALIVE,<n>` every 60 s per peer) and (`STATUSREQ` every 90s ). |
| Logging | `server.cpp` | Replaced `logmsg` with `log_event`, `log_event_bytes` and `log_note`. Logs now carry event type, IP, port and server ID. |
| Accept change | `server.cpp` | `accept_client()` now records the client's IP and port. |
| Name rules | `common.cpp`, `common.h` | Names can be 1-14 characters and may include `_` and `-`. `split_fields` has a default for `max_fields`. |
| Relay behaviour | `p_sendmsg()` and `p_statusresp()` | Modified, such that each message is stored upon recieved `SENDMSG`. On recieved `STATUSRESP` only fetch and store msgs that aren't you/any connected peer. |
| Assistance   | `server_cmds.cpp` | Helped and participated with protocol design and definition (Piazza threads and polls). |

### Bjorn:
| Part | Files | Work |
|---|---|---|
| Incoming SENDMSG | `server_cmds.cpp` | `p_sendmsg`: checks the 5 fields, both group IDs and the timestamp (digits only, at most 10), and stores messages addressed to us under our `GROUP_ID` so our client can fetch them with `GETMSG`. |
| GETMSGS | `server_cmds.cpp`, `server.h` | `p_getmsgs`: answers with one `SENDMSG` per held message, then deletes them. Passed `fd` through `p_handle` so one command can send several replies. |
| KEEPALIVE | `server_cmds.cpp` | `p_keepalive`: checks the count and answers `GETMSGS,<our id>` when it is above 0. |
| STATUSRESP | `server_cmds.cpp`, `server.h` | `p_statusresp`: reads the `<group>:<count>` entries and sends `GETMSGS` for messages held for us. |
| LISTSERVERS client command | `server_cmds.cpp`, `client.cpp` | Lists our own entry and the servers we dialled (the same list as our `SERVERS` reply). Added to the client's help line. |
| Peer management | `server.cpp`, `server.h`, `server_cmds.cpp` | A target number of peers (4 at first, raised to 7), one new connection per tick, `G_` servers first, a 60 s wait before redialling, the instructor as a fallback, and at most 8 peers.|

`identity.h` was generated by `tsam-register`, and `log_event.h` was given by the course. We have not changed either.


## Sources used (man pages)

| Used for | Where in code | Man page |
|---|---|---|
| Epoll instance | `epoll_create1` in constructor | `epoll_create1(2)`, `epoll(7)` |
| Add/remove fds from epoll | `epoll_ctl` | `epoll_ctl(2)` |
| Waiting for events in the main loop | `epoll_wait` in `run()` | `epoll_wait(2)`, `epoll(7)` (level-triggered behaviour) |
| 5-second timer as an fd | `timerfd_create`, `timerfd_settime` | `timerfd_create(2)`, `timerfd_settime(2)` |
| Reading the timer expiration count | `read(timer_fd_, ...)` in `on_timer()` | `timerfd_create(2)` (reading section), `read(2)` |
| Listening socket | `socket`, `bind`, `listen`, `setsockopt(SO_REUSEADDR)` | `socket(2)`, `bind(2)`, `listen(2)`, `setsockopt(2)`, `socket(7)` |
| Accepting clients, getting their address | `accept4` with `SOCK_NONBLOCK` | `accept4(2)` |
| Non-blocking outgoing connect | `connect` returning `EINPROGRESS` | `connect(2)` |
| Checking if the connect succeeded | `getsockopt(SO_ERROR)` in `finish_connect()` | `getsockopt(2)`, `socket(7)` |
| Waiting for connect completion | `EPOLLOUT` on a connecting socket | `connect(2)` (EINPROGRESS section), `epoll_ctl(2)` |
| Address conversion | `inet_pton`, `inet_ntoa`, `htons`, `ntohs` | `inet_pton(3)`, `inet_ntoa(3)`, `byteorder(3)` |
| Timestamps / keepalive timing | `time(nullptr)` | `time(2)` |
| Close and error text | `close`, `strerror` | `close(2)`, `strerror(3)` |

We also used the course project specification for the protocol (`HELO`, `SERVERS`, `KEEPALIVE`, `STATUSREQ`, `STATUSRESP`, `ERROR`).


## AI assistant use

We used an AI assistant (Claude) to understand concepts that are hard to write code around.

### Birkir
- **Level-triggered epoll loop:** how `epoll_wait` keeps reporting an fd as ready until I deal with it, and why that matters in my loop.
- **timerfd:** how a timer becomes a file descriptor that fits into the epoll loop the same way `listen_fd_` does, and why I must `read()` it or epoll fires again.
- **How `flush` works:** how output is buffered per client, and how `EPOLLOUT` is switched on/off when the socket cannot take all the data.
- **Brainstorm protocol definition**: discuss ideas/refine protocol definition to follow KISS principles. 

### Bjorn:
- **Step-by-step guidance:** for `p_sendmsg`, `p_getmsgs`, `p_keepalive`, `p_statusresp` and `LISTSERVERS`, Claude explained the existing code and then reviewed what I wrote.
- **Peer management:** I wrote this code with claude after we simplified the design together (`maintain_peers`, `count_peers`, `pick_candidate`, the retry rule in `close_client`, and the peer limit in `handle_command`).
- **Reviewing:** reading the assignment, finding why our Assignment 6 tests failed, and reviewing commits and merge conflicts.

