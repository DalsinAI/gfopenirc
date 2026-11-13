/* GFOpenIRC protocol core: message parsing, IRCv3 CAP and SASL PLAIN,
 * ISUPPORT, case mapping, channel and member state, send throttling.
 * Our own code from RFC 1459/2812 and the IRCv3 specifications; no GPL
 * client was used (irssi, HexChat, WeeChat: ideas only).
 * Runs over a gfopennetkit connection (TCP, TLS or a SOCKS5 tunnel), never
 * blocks, and reads no clock: the caller passes the time.
 * MIT licence, (c) 2026 AmiCradle Team. */
#ifndef GFOPENIRC_IRC_H
#define GFOPENIRC_IRC_H

#include <stddef.h>
#include <stdint.h>
#include "gfopennetkit/netkit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_VERSION "GFOpenIRC 0.1"

/* ---- one message ------------------------------------------------------------
 * "@tags :nick!user@host COMMAND p1 p2 :trailing". The line is copied and
 * split in place; tag values are unescaped. Up to 15 parameters (RFC 2812),
 * the rest folded into the last. */
#define IR_MAX_PARAMS 15
#define IR_MAX_TAGS   32
#define IR_LINE_MAX   8704      /* 8191 tag bytes + 512 */

typedef struct IrMsg {
    const char *nick, *user, *host;      /* prefix parts; NULL when absent */
    const char *cmd;                     /* upper case */
    int         nparams;
    const char *params[IR_MAX_PARAMS];
    int         ntags;
    const char *tag_key[IR_MAX_TAGS], *tag_val[IR_MAX_TAGS];
    char        buf[IR_LINE_MAX + 1];
} IrMsg;

/* NK_OK, NK_EPROTO (no command, or over IR_LINE_MAX). */
int ir_parse(IrMsg *m, const char *line, size_t len);
/* A tag's value ("" for a tag with no value), or NULL when not there. */
const char *ir_tag(const IrMsg *m, const char *key);

/* ---- case mapping (ISUPPORT CASEMAPPING) ---------------------------------- */
enum { IR_CM_ASCII = 0, IR_CM_RFC1459, IR_CM_STRICT_RFC1459 };
int  ir_casecmp(int cm, const char *a, const char *b);
void ir_casefold(int cm, char *s);

/* ---- ISUPPORT (005) ---------------------------------------------------------- */
typedef struct IrSupport {
    char chantypes[16];
    char prefix_modes[16], prefix_chars[16];   /* "ov", "@+": highest first */
    char chanmodes[4][64];                     /* CHANMODES A,B,C,D */
    int  casemap;
    int  nicklen;
    char network[64];
} IrSupport;

void ir_isupport_init(IrSupport *s);
void ir_isupport_apply(IrSupport *s, const IrMsg *m);
int  ir_is_channel(const IrSupport *s, const char *name);
/* Does mode letter `m` take a parameter when set (+) or unset (-)? */
int  ir_mode_has_arg(const IrSupport *s, char m, int set);

/* ---- client ---------------------------------------------------------------- */
typedef struct IrConfig {
    const char *nick;
    const char *user;          /* default: the nick */
    const char *realname;      /* default: the nick */
    const char *server_pass;   /* PASS, or NULL */
    const char *sasl_user;     /* SASL PLAIN when set (needs sasl_pass) */
    const char *sasl_pass;     /* copied and wiped after use */
    int         sasl_required; /* quit rather than go on unauthenticated */
    int         tls;           /* the connection is TLS with the server's
                                  certificate checked (set by the caller;
                                  TLS without the check counts as plain) */
    int         allow_plain_auth; /* send PASS/SASL over plain TCP anyway
                                     (off by default: the password would
                                     cross the network in clear) */
    uint32_t    burst;         /* lines sent at once before throttling (5) */
    uint32_t    interval_ms;   /* then one line every this many ms (2000) */
    uint32_t    ping_ms;       /* PING the server after this quiet time (90 s) */
} IrConfig;

enum {
    IR_EV_REGISTERED = 1, /* 001 arrived; text = welcome */
    IR_EV_MESSAGE,        /* PRIVMSG; target = channel or query nick */
    IR_EV_NOTICE,
    IR_EV_ACTION,         /* CTCP ACTION (/me) */
    IR_EV_CTCP,           /* other CTCP request; text = "VERSION" etc. */
    IR_EV_JOIN,           /* extra = account (extended-join) */
    IR_EV_PART,
    IR_EV_QUIT,           /* once per channel the nick was in */
    IR_EV_NICK,           /* once per shared channel; extra = new nick */
    IR_EV_KICK,           /* extra = the nick kicked */
    IR_EV_TOPIC,
    IR_EV_NAMES,          /* the member list of target is complete */
    IR_EV_MODE,           /* text = "+o nick" etc. */
    IR_EV_AWAY,           /* away-notify; text = reason or NULL (back) */
    IR_EV_ACCOUNT,        /* account-notify; extra = account or "*" */
    IR_EV_INVITE,
    IR_EV_SERVER,         /* numerics and other server text */
    IR_EV_ERROR,          /* an error numeric or ERROR; text says what */
    IR_EV_SASL,           /* extra = "ok" or "failed" */
    IR_EV_CAPS            /* capabilities settled; text = what was enabled */
};

typedef struct IrEvent {
    int         type;
    const char *target;  /* buffer: channel, query nick, or NULL = server */
    const char *nick;    /* who did it */
    const char *text;
    const char *extra;
    const char *time;    /* server-time tag, or NULL */
    int         self;    /* we did it */
    const IrMsg *msg;    /* the message, or NULL for local events */
} IrEvent;

typedef void (*IrEventFn)(void *user, const IrEvent *ev);

typedef struct IrClient IrClient;

/* NK_OK; NK_EARG for a bad nick, user, real name or password; NK_EREFUSED
 * when a password (PASS, or SASL: a sasl_user is enough, so a caller can
 * check before it looks the password up) would go over plain TCP and
 * allow_plain_auth isn't set. */
int ir_config_check(const IrConfig *cfg);
/* Takes the connection (freed by ir_free) and queues the registration.
 * NULL when ir_config_check fails (the connection is then the caller's). */
IrClient *ir_new(NKConn *conn, const IrConfig *cfg, IrEventFn fn, void *user, uint32_t now_ms);
void      ir_free(IrClient *c);
/* Moves bytes, handles every line, answers PING, keeps the link alive and
 * sends throttled lines. NK_OK, or NK_EOF / NK_EIO / NK_ETIMEDOUT /
 * NK_ECANCELLED from the connection. */
int       ir_pump(IrClient *c, uint32_t now_ms);
NKConn   *ir_conn(IrClient *c);

int  ir_registered(const IrClient *c);
const char *ir_nick(const IrClient *c);
const IrSupport *ir_support(const IrClient *c);
/* Space-separated capabilities in use. */
const char *ir_caps(const IrClient *c);
int  ir_has_cap(const IrClient *c, const char *cap);
/* 1 once the user asked to QUIT (so an EOF is the expected end). */
int  ir_quitting(const IrClient *c);
/* Lines waiting for the throttle. */
size_t ir_queued(const IrClient *c);

/* Sending. Lines go through the throttle; CR, LF and NUL are refused. A
 * message too long for one line is split at a space or a UTF-8 boundary. */
int ir_raw(IrClient *c, const char *line);
int ir_say(IrClient *c, const char *target, const char *text);
int ir_notice(IrClient *c, const char *target, const char *text);
int ir_action(IrClient *c, const char *target, const char *text);
int ir_join(IrClient *c, const char *channel, const char *key);
int ir_part(IrClient *c, const char *channel, const char *reason);
int ir_set_nick(IrClient *c, const char *nick);
int ir_quit(IrClient *c, const char *reason);
/* Input from the user's line in `buffer` (a channel, a query or NULL for
 * the server tab): "/join #x", "/msg nick text", "/me ...", "/nick",
 * "/part [reason]", "/quit [reason]", "/topic [text]", "/notice", "/query",
 * "/raw LINE", "//text" sends "/text"; plain text is said to the buffer.
 * NK_EARG for an unknown command or plain text in the server tab. */
int ir_command(IrClient *c, const char *buffer, const char *input);
/* The buffer a front end writes to after the user's line: "/msg nick ..."
 * and "/query nick" (any case) make `nick` current. 1 when `current` was set,
 * 0 when the line names no target or it does not fit in `cap` bytes. */
int ir_input_target(const char *input, char *current, size_t cap);

/* ---- channel state ---------------------------------------------------------- */
typedef struct IrMember {
    char nick[64];
    char prefixes[16];         /* "@+", highest first */
    char account[64];          /* "" unknown, "*" logged out */
    int  away;
} IrMember;

size_t      ir_channel_count(const IrClient *c);
const char *ir_channel_name(const IrClient *c, size_t i);
const char *ir_topic(const IrClient *c, const char *channel);
size_t      ir_member_count(const IrClient *c, const char *channel);
const IrMember *ir_member(const IrClient *c, const char *channel, size_t i);
const IrMember *ir_find_member(const IrClient *c, const char *channel, const char *nick);

/* ---- helpers ----------------------------------------------------------------- */
/* Base64 (RFC 4648) for SASL; answers the length written (no NUL) or 0
 * when out is too small. */
size_t ir_b64(const void *in, size_t n, char *out, size_t cap);
/* How many bytes of text fit in `room` without cutting a UTF-8 sequence,
 * preferring the last space. */
size_t ir_split_at(const char *text, size_t len, size_t room);

#ifdef __cplusplus
}
#endif
#endif
