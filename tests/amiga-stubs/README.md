Stand-ins for the few Amiga headers IRCView includes, so the host CI can
compile `amiga/IRCView.c` with GCC's warnings (format truncation among
them) at -O2 -Werror. They declare only what IRCView uses and are never
linked: the real build is `build-ircview.sh` on the stove.
