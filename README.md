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

## Building and testing

    cmake -S . -B build -DNETKIT_DIR=../gfopennetkit && cmake --build build && ctest --test-dir build

`cli_fake_server` runs the CLI against gfopennetkit's fake IRC server, directly and through its fake SOCKS5 proxy; no test needs the internet.
