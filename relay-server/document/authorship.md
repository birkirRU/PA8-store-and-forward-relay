# Statement of Authorship

## Who wrote what

### Birkir:
| Part | Files | What I did |
|---|---|---|
| Split server into header and source | `server.h`, `server.cpp`, `Makefile` | Moved the `Server` class into `server.h`. Added `server_cmds.cpp` to the build. |
| Client commands | `server_cmds.cpp` | Renamed `sendmsg` / `getmsg` / `statusreq` to `c_sendmsg` / `c_getmsg` / `c_statusreq`. Added `c_handle`. |
| Peer commands | `server_cmds.cpp` | Added `p_handle`, `p_helo`, `p_servers`, `p_keepalive`, `p_statusreq`, `p_statusresp`. `p_getmsgs` and `p_sendmsg` are stubs for now. |
| Connection type detection | `server_cmds.cpp`, `server.h` | `ConnType` (client / peer / unknown). The first command decides: `HELO` means peer, anything else means client. |
| Outgoing peer connection | `server.cpp` | `connect_peer()` (non-blocking connect to the instructor server) and `finish_connect()` (sends `HELO`). |
| Server list | `server.cpp`, `server_cmds.cpp` | `servers_list()`, parsing `SERVERS,...` entries, storing them in `known_` (capped at 500). |
| KEEPALIVE timer | `server.cpp` | `timerfd` created in the constructor, added to epoll, and handled by `on_timer()` (sends `KEEPALIVE,<n>` every 60 s per peer). |
| Logging | `server.cpp` | Replaced `logmsg` with `log_event`, `log_event_bytes` and `log_note`. Logs now carry event type, IP, port and server ID. |
| Accept change | `server.cpp` | `accept_client()` now records the client's IP and port. |
| Name rules | `common.cpp`, `common.h` | Names can be 1-14 characters and may include `_` and `-`. `split_fields` has a default for `max_fields`. |
| Relay behaviour | `p_sendmsg()` and `p_statusresp()` | Modified, such that each message is stored upon recieved `SENDMSG`. On recieved `STATUSRESP` only fetch and store msgs that aren't you/any connected peer. |
### Bjorn:
| Part | Files | What I did |
|---|---|---|
| Incoming SENDMSG | `server_cmds.cpp` | `p_sendmsg`: checks the 5 fields, both group IDs and the timestamp (digits only, at most 10), and stores messages addressed to us under our `GROUP_ID` so our client can fetch them with `GETMSG`. |
| GETMSGS | `server_cmds.cpp`, `server.h` | `p_getmsgs`: answers with one `SENDMSG` per held message, then deletes them. Passed `fd` through `p_handle` so one command can send several replies. |
| KEEPALIVE | `server_cmds.cpp` | `p_keepalive`: checks the count and answers `GETMSGS,<our id>` when it is above 0. |
| STATUSRESP | `server_cmds.cpp`, `server.h` | `p_statusresp`: reads the `<group>:<count>` entries and sends `GETMSGS` for messages held for us. |
| LISTSERVERS client command | `server_cmds.cpp`, `client.cpp` | Lists the servers we are connected to. Added to the client's help line. |
| Peer management | `server.cpp`, `server.h`, `server_cmds.cpp` | Written by Claude and me at my request (see AI assistant use), from a design we worked out together: aim for 4 peers, one new connection per tick, `G_` servers first, a 60 s wait before redialling, the instructor as a fallback, and at most 8 peers. |

Files used by this diff but **not shown in it**: `identity.h` (`GROUP_ID`) and `log_event.h`. (Add a line here about who wrote those if they are yours.)

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

We used an AI assistant (Claude) to **understand concepts** that are hard to write code around.

### Birkir
- **Level-triggered epoll loop:** how `epoll_wait` keeps reporting an fd as ready until I deal with it, and why that matters in my loop.
- **timerfd:** how a timer becomes a file descriptor that fits into the epoll loop the same way `listen_fd_` does, and why I must `read()` it or epoll fires again.
- **How `flush` works:** how output is buffered per client, and how `EPOLLOUT` is switched on/off when the socket cannot take all the data.

### Bjorn:
- **Step-by-step guidance:** for `p_sendmsg`, `p_getmsgs`, `p_keepalive`, `p_statusresp` and `LISTSERVERS`, Claude explained the existing code and then reviewed what I wrote.
- **Peer management:** Claude wrote this code with me after we simplified the design together (`maintain_peers`, `count_peers`, `pick_candidate`, the retry rule in `close_client`, and the peer limit in `handle_command`).
- **Reviewing:** reading the assignment, finding why our Assignment 6 tests failed, and reviewing commits and merge conflicts.
