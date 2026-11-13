#!/usr/bin/env python3
# gfopenirc-cli against gfopennetkit's fake IRC server: CAP LS 302, SASL
# PLAIN, nick in use, JOIN and NAMES, a channel message, /me, a query,
# CTCP VERSION, echo-message, server-time, QUIT; through the fake SOCKS5
# proxy; and a refused SASL login with --sasl-required.
# MIT licence, (c) 2026 AmiCradle Team.
import os
import subprocess
import sys

cli = sys.argv[1]
irc, socks = os.environ["NK_IRC_PORT"], os.environ["NK_SOCKS5_PORT"]
fails = 0


def check(cond, what):
    global fails
    if not cond:
        print("FAIL", what)
        fails += 1


def run(args, script, pw="s3cret"):
    env = dict(os.environ, IRC_PW=pw)
    return subprocess.run([cli] + args, input=script, capture_output=True, text=True, env=env, timeout=30)


base = ["--server", "127.0.0.1", "--port", irc, "--nick", "taken", "--sasl-user", "dale",
        "--pass-env", "IRC_PW", "--join", "#amiga", "--interval", "50"]
script = "hello amigans\n/me waves\n/msg friend psst\n/quit see you\n"
r = run(base, script)
ev = [l.split("\t") for l in r.stdout.splitlines()]


def has(*want):
    return any(e[:len(want)] == list(want) for e in ev)


check(r.returncode == 0, "exit 0 after /quit: %d %r" % (r.returncode, r.stderr))
check(has("*", "sasl", "", "SASL authentication successful [ok]"), "SASL ok: %r" % r.stdout)
check(has("*", "caps", "", "sasl server-time message-tags multi-prefix away-notify echo-message"),
      "caps: %r" % r.stdout)
check(has("*", "registered", "", "Welcome to the fake network"), "registered")
check(has("#amiga", "join", "taken_"), "joined as taken_ after 433: %r" % r.stdout)
check(has("#amiga", "topic", "", "Amiga talk, be nice"), "topic")
check(has("#amiga", "members", "", "@+op +voice taken_"), "members with multi-prefix: %r" % r.stdout)
check(has("#amiga", "mode", "fake.irc", "+o other"), "MODE")
check(has("#amiga", "message", "taken_", "hello amigans"), "own message echoed by the server")
check(has("#amiga", "message", "echo", "hello amigans"), "reply in the channel")
check(has("#amiga", "action", "taken_", "waves"), "/me")
check(has("friend", "message", "taken_", "psst"), "query")
check(has("echo", "message", "echo", "psst"), "a private reply opens a query with its sender")
check("\x01" not in r.stdout, "no raw CTCP bytes printed")

# the same through the SOCKS5 proxy
r = run(["--socks", "127.0.0.1:" + socks] + base, "through the proxy\n/quit\n")
check(r.returncode == 0 and "\techo\tthrough the proxy" in r.stdout,
      "through SOCKS5: %d %r %r" % (r.returncode, r.stdout, r.stderr))

# a wrong password with --sasl-required: no registration, exit 4
r = run(base + ["--sasl-required"], "", pw="nope")
check(r.returncode == 4, "refused SASL exits 4: %d %r %r" % (r.returncode, r.stdout, r.stderr))
check("registered" not in r.stdout, "never registered")

print("ok" if not fails else "%d failures" % fails)
sys.exit(1 if fails else 0)
