# Assignment 6: Client-Server Messaging

A mail server (`tsamserver`) and a client (`tsamclient`) that talk over TCP using
the framed protocol from the assignment.

| File            | Contents                                                              |
|-----------------|-----------------------------------------------------------------------|
| `server.cpp`    | The server: one epoll loop, the message store and the log             |
| `client.cpp`    | The client: a poll loop over stdin and the server socket              |
| `common.h/.cpp` | Shared code: framing, command field splitting, name check, timestamps |

## Compiling and running

```bash
make                    # builds tsamserver and tsamclient
./tsamserver 4044       # listen on port 4044 (any port 1-65535)
./tsamclient 127.0.0.1 4044
```

The server logs to `server.log` in the directory it is started from. The file is
emptied at every start, so one run produces the log of one session. Stop the server
with Ctrl-C (or `kill`), which writes a final log line.

## Operating system

The server runs on **Linux only**, because it uses `epoll`, `accept4`, `SOCK_NONBLOCK`
and `MSG_NOSIGNAL`. It is meant to be built and tested on Linux (the TSAM server, Ubuntu 24.04). The server and client applications where developed on Rocky Linux 9. 

## Commands

Commands and replies use the framing from the assignment. We read the 5000-byte limit as
applying to the command text between STX and ETX, so a frame can be up to 5005 bytes.

| Command                         | Reply                                                                                        |
|---------------------------------|----------------------------------------------------------------------------------------------|
| `SENDMSG,<from>,<to>,<message>` | `OK`                                                                                         |
| `GETMSG,<user>`                 | `<from>,<message>` (the oldest message for `<user>`, which is then deleted), or `NO_MESSAGE` |
| `STATUSREQ`                     | `<user1>:<count1>,<user2>:<count2>,...`, or `EMPTY` (format fixed by the assignment)         |
| anything else                   | `ERROR,command does not exist or is malformed`                                               |

- Usernames are 1-32 ASCII letters (`A-Z`, `a-z`). This keeps `STATUSREQ` replies
  unambiguous, since a name cannot contain `,` or `:`.
- The message may contain commas and may be empty.
- Command names are case-sensitive, and commands take no extra fields: `STATUSREQ,x`
  is malformed.
- Users are just names. Anyone can send a message to any name, and
  anyone can fetch the messages for any name. The server
  only knows connections, so that it can reply on the right socket.

## Server behaviour

- **Many clients.** A single thread serves all clients with level-triggered `epoll`.
  It sleeps until a socket is ready, so it uses no CPU while idle.
- **Partial and joined frames.** TCP can split a frame across reads or join several
  frames together. Each client has an input buffer, and a command is handled only once
  its whole frame has arrived. Several commands per connection are fine.
- **Slow clients.** Each client has an output buffer. A reply that does not fit in the
  socket buffer is sent later, when the socket becomes writable. Other clients are not
  blocked.
- **Messages** are stored per recipient, oldest first. The recipient does not have to
  be connected. `GETMSG` hands out one message and deletes it. `STATUSREQ` lists only
  users with at least one message, in alphabetical order.
- **Malformed commands.** Unknown names, a wrong number of fields, invalid usernames
  and wrong case are ignored, and the server replies with
  `ERROR,command does not exist or is malformed`.
- **Malformed frames.** Bytes outside a frame, a length below 5 or above 5005, and a
  missing STX or ETX are discarded without a reply. The server resynchronises on the
  next SOH and keeps serving the client. Each discard is logged with its reason.
- **Disconnects.** The client's buffers are freed, and an incomplete frame left behind
  is logged. Its stored messages stay.
- **Errors.**
  - Startup failures make the server exit with status 1, with a message on stderr and
    in the log. Examples are a port already in use, `server.log` that cannot be opened,
    and socket or epoll errors.
  - An error on one client's connection (`recv`/`send` failure, connection reset)
    closes only that client, and the reason is logged.
  - `EINTR` and `EAGAIN` are harmless and retried.
  - A client that disappears never kills the server with SIGPIPE (`MSG_NOSIGNAL`).
  - When the server runs out of file descriptors, it uses a reserved descriptor to
    accept and immediately close the waiting connection. The listening socket therefore
    never makes the loop spin.
- Messages are kept in memory only and are lost when the server stops.

### Server log

There is one line per event. Times are local, with the UTC offset (Common Log Format):

```
[12/Oct/2026:18:23:45 +0000] client 1 connected (1 connected)
[12/Oct/2026:18:23:45 +0000] client 1 RECV SENDMSG,alice,bob,hi
[12/Oct/2026:18:23:45 +0000] STATE stored message from alice for bob (1 waiting for them, 1 stored in total)
[12/Oct/2026:18:23:45 +0000] client 1 SEND OK
```

The log records:
- every command received (`RECV`) and every reply sent (`SEND`)
- connects and disconnects, with the reason
- rejected commands and discarded frames
- `STATE` lines with stored-message counts
- slow-client queueing
- startup and shutdown

Control characters sent by clients are written as `\xNN`.

## Client behaviour

- Prints every command sent and every reply received with a timestamp:
  ```
  2026-10-12 18:23:45 SENT: GETMSG,bob
  2026-10-12 18:23:45 RECV: alice,hello bob
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

## Known limitations

- A `STATUSREQ` reply longer than 5000 bytes cannot be framed. The server logs it and
  sends no reply.
- There is no limit on the number of stored messages.
