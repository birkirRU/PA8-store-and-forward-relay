# G_stammtischen: TODO

## Must have (assignment points 1a-1e)
1. SENDMSG from peers (p_sendmsg): validate fields and timestamp; if addressed
   to us, store for our client; otherwise forward if connected, else hold.
2. GETMSGS (p_getmsgs): reply with held messages for that group as SENDMSG.
3. Client SENDMSG: add timestamp, send to destination if connected, else hold.
   Client GETMSG must use our own GROUP_ID as the mailbox name.
4. On KEEPALIVE,n with n > 0: send GETMSGS to that peer.
5. Peer management: keep 3-8 connections, reconnect when lost, connect to
   servers in known_, prefer G_ over I_. Use on_timer for this.
6. Include ourselves in our SERVERS reply (needs our IP and port, e.g. from
   getsockname or an optional argument).
7. Message expiry, plus a log_note when we hold messages for an unreachable group.
8. Robustness: per-IP rate limit and temporary block, summarise repeated log
   lines per IP, tolerate over-long or garbage frames.
9. Test with other groups: being connected to (1b), receiving from 2 groups (1c),
   sending to 2 groups (1d). Send TEST_ME to an I_ server to check reachability.
10. Makefile builds both binaries with plain make; README.md and authorship
    statement (written by us); back up server.log regularly.

## Bonus (need evidence collected over time, so start early)
- Early Bot (week 1), Ready Bot (5 days x 2 h), Edge Bot (3 non-instructor peers, 5 times)
- Chatty Bot (messages from 10/20 groups), Indirect Bot (relay 10, hold and deliver 2)
- Cartographer (3 maps, 3 days: needs periodic HELO/SERVERS to neighbours and
  saving the answers)
- Courier Bot (dispatches from I_ servers, reply as told)
- External Bot (run on a non-RU machine), Identify the Enemy (keep notes: IP, lines, reasoning)