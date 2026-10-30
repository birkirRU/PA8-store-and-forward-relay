# G_stammtischen: TODO

## Must have (assignment points 1a-1e)


2. GETMSGS abuse check. Answer only for the asker’s own HELO name (section 6 of the protocol notes).

3. Message expiry and dedupe. Delete messages older than 24 h, and drop repeated messages (item 3 on the to-do list).

4. Robustness:
* per-IP rate limit and temporary block
* summarising repeated log lines
* caps on stored messages and on output buffers
* handle_command still uses c after the connection may have closed * (server_cmds.cpp:74)

4. Robustness: per-IP rate limit and temporary block, summarise repeated log
   lines per IP, tolerate over-long or garbage frames.
5. Test with other groups: being connected to (1b), receiving from 2 groups (1c),
   sending to 2 groups (1d). Send TEST_ME to an I_ server to check reachability.
6. Makefile builds both binaries with plain make; README.md and authorship
    statement (written by us); back up server.log regularly.

## Bonus (need evidence collected over time, so start early)
- Early Bot (week 1), Ready Bot (5 days x 2 h), Edge Bot (3 non-instructor peers, 5 times)
- Chatty Bot (messages from 10/20 groups), Indirect Bot (relay 10, hold and deliver 2)
- Cartographer (3 maps, 3 days: needs periodic HELO/SERVERS to neighbours and
  saving the answers)
- Courier Bot (dispatches from I_ servers, reply as told)
- External Bot (run on a non-RU machine), Identify the Enemy (keep notes: IP, lines, reasoning)