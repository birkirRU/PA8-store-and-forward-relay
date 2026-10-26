# G_stammtischen: status (what exists)

## Files
- identity.h, log_event.h: given by the course, unchanged
- common.cpp / common.h: framing (encode_frame, extract_frames), split_fields,
  is_valid_name, parse_port, send_all
- server.h / server.cpp: Server class, epoll loop, connect/accept, timer, logging
- server_cmds.cpp: handlers for client commands (c_*) and peer commands (p_*)
- client.cpp: command-line client (Assignment 6), talks only to our own server
- Makefile: builds tsamserver and tsamclient

## Server core
- One epoll loop, one listening port for both our client and other servers.
- Connection types: unknown / client / peer. The first command on a connection
  decides: HELO means peer, anything else means client.
- Non-blocking outgoing connect: EPOLLOUT means "connect finished",
  then SO_ERROR is checked, then CONN is logged and HELO sent.
- Output goes through queue_send(): bytes are queued in `out`, flush() sends,
  EPOLLOUT is only registered while `out` is non-empty.
- Group ID comes only from identity.h (GROUP_ID). The only hard-coded address
  is 130.208.246.98:5001 (the instructor).
- timerfd tick every 5 s inside the same epoll loop (no busy waiting).

## Peer protocol
| Command    | State |
|------------|-------|
| HELO       | Done. Name validated, we reply SERVERS and (for incoming) our HELO. |
| SERVERS    | Done. Parsed per entry, validated, stored in known_ (capped at 500). No reply. |
| KEEPALIVE  | Sent every >= 60 s per peer with the count of messages we hold for it. Received: accepted, nothing done with the number yet. |
| STATUSREQ  | Done. Replies STATUSRESP,<g>:<n>,... or STATUSRESP,EMPTY. |
| STATUSRESP | Accepted, ignored. |
| ERROR      | Logged, never answered (prevents ping-pong). |
| SENDMSG    | NOT DONE (stub, logged as DROP) |
| GETMSGS    | NOT DONE (stub, logged as DROP) |

Rejected peer commands get a DROP line and no reply. Clients get
ERROR,command does not exist or is malformed.

## Client protocol (from Assignment 6)
SENDMSG,<from>,<to>,<text> -> OK (stored in mailboxes_ with timestamp),
GETMSG,<user> -> from,text or NO_MESSAGE, STATUSREQ.
Client SENDMSG is only stored. It is not yet forwarded to peers.

## Logging
log_event / log_event_bytes for protocol events, log_note for internal state.
Log is appended across runs. RECV is only logged for well-formed commands,
DROP for the rest. A HELO is logged with "-" as ID, as required.

## Evidence in server.log
- 2026-10-25T14:38:47Z first run: the instructor answered our ERRORs with
  ERROR (bug, before HELO/SERVERS handling).
- 2026-10-25T22:49:34Z to 22:51:51Z: HELO/SERVERS exchange works, no ERROR loop.
- 2026-10-26T11:07:34Z run: HELO, SERVERS, KEEPALIVE sent (11:07:39, 11:08:39),
  STATUSREQ answered (11:08:17).
- Servers learned via SERVERS: I_instr2 (:5002), G_swaws (:4167), G_SHE (:4045).

## Known quirks
- The log tag in notes shows "Peer1" (the connection id), not the group ID.
- Our SERVERS reply lists only outgoing peers (incoming peers' listening port
  is unknown) and not ourselves.