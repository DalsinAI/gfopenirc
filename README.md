# gfopenirc

GFOpenIRC: IRC client for GFOS (TLS, IRCv3 SASL, ARexx)

Part of GFOS (AmiCradle). Design: amicradle-teams `designs/` (GFOPENNEWS-20261113.md, GFOPENIRC-20261113.md, and the Team 2 answers of 13 Nov 2026).

Rules: commits are authored and committed by SacredTrees; draw through GFOpenLayout; third-party code keeps its own licence and goes in `third_party/` with its notice (BSD-3-Clause libtorrent is allowed; no GPL, LGPL or MPL code).

## What is here (step 2 of the design)

| Part | Header | Notes |
| --- | --- | --- |
| Messages | `irc.h` `ir_parse` | IRCv3 message tags (escapes undone), prefix split into nick, user and host, up to 15 parameters, trailing; over-long or broken lines refused |
| ISUPPORT, case mapping | `IrSupport`, `ir_casecmp` | CHANTYPES, PREFIX, CHANMODES, CASEMAPPING (ascii, rfc1459, strict-rfc1459), NICKLEN, NETWORK; which modes take a parameter |
| Registration | `ir_new` | CAP LS 302 (several LS lines), REQ only what we use, SASL PLAIN (password sent in 400-byte pieces and wiped), `--sasl-required` quits rather than going on unauthenticated, a server with no CAP works too, nick in use tries another |
| Client | `ir_pump`, `ir_command` | PING/PONG and a keepalive PING with timeout; channels, topics and members (multi-prefix, MODE prefix changes, NICK, PART, KICK, QUIT, away-notify, account-notify, extended-join); events per buffer (channel, query or server) with the server-time stamp; CTCP ACTION, VERSION, PING and CLIENTINFO with replies rate-limited; echo-message or local echo; STATUSMSG; a send throttle (burst, then one line per interval) so the server never kills us for flooding; long messages split at a space, never inside a UTF-8 character, sized for what other users receive; CR, LF and NUL refused everywhere |
| CLI | `src/cli.c` | `gfopenirc-cli --server H --port N --nick N [--sasl-user U --pass-env VAR] [--sasl-required] [--join #C] [--socks H:P]`, then commands on standard input; one line per event. Host build; the Amiga build (bsdsocket, gfopentls, password from gfopenkeys, ReadArgs) comes next |
| Chat models | `ui_model.h` `iu_*` | a buffer per server, channel and query, its scrollback in a gfopennetkit text view (IRC colours and links), unread counts and highlights (our nick said), each event made a readable line in the right buffer; the nick list sorted highest prefix first, then by nick; per-buffer input history; Tab completion (": " at the line's start) |
| Chat window | `ui_window.h` `iw_win_*` | on GFOpenLayout (olui), nothing drawn by the program: the buffers of every server in a list on the left (the tabs), the topic over the log, the nick list on the right (double-click: a query), the input line (Return sends, Up and Down for history, Tab for nicks, through `iw_win_key`), Close, and a Connect tab with no password field (the password comes from gfopenkeys at connect time) |
| IRCView | `amiga/IRCView.c` | the window on the Amiga on one server over plain TCP, no TLS and no password: the window's lab check until the chat program joins it to TLS and the key store |

## Building and testing

    cmake -S . -B build -DNETKIT_DIR=../gfopennetkit -DGFOPENLAYOUT_DIR=../gfopenlayout && cmake --build build && ctest --test-dir build

Without a gfopenlayout checkout the chat window and its test are left out. `test_ui_window` drives the window with olui's own pointer and key calls at 800 x 600.

IRCView for the Amiga: `./build-ircview.sh` (the os32 GCC 16 stove, gfopennetkit's and gfopenlayout's Amiga libraries built first).

`cli_fake_server` runs the CLI against gfopennetkit's fake IRC server, directly and through its fake SOCKS5 proxy; no test needs the internet.

## Lab steps (IRCView)

A unit test can't show the window drawn by the Amiga glue or the keys as Intuition gives them, so these run in a lab copy:

1. On the lab host: `python3 ../gfopennetkit/tests/fakes/fakeserv.py irc`. It prints `PORTS irc=N`.
2. Copy `build-ircview/IRCView` to the lab's `RAM:` and run `RAM:IRCView HOST=<the host as the lab's network sees it> PORT=N NICK=amiga CHANNELS=#amiga`.
3. The window opens with the host given at the top of the list. Its log shows "Connecting to ...", then the welcome. `#amiga` appears under it, indented.
4. Choose `#amiga`. The nick list shows `@op`, `+voice`, `amiga`. The topic sits over the log, and the input line has the focus.
5. Type `hello` and press Return. `<amiga> hello` goes in the log, and the fake server's `echo` answers.
6. Press Up: `hello` comes back. Press Down: the line is empty again.
7. Type `vo` and press Tab: the line reads `voice: `. Press Tab again; with one match it stays.
8. Double-click `op` in the nick list. An `op` query opens under the server and is shown.
9. Choose the server, then Close: QUIT is sent and the log says "Disconnected.".
10. Resize the window: the panes follow it, with nothing left over. Close the window.

Pass: every step reads as written, nothing is drawn outside the window, and no Enforcer or MuForce hits appear.
