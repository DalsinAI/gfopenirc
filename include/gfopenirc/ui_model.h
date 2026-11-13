/* GFOpenIRC chat window models, kept apart from drawing so the host tests
 * reach them. Plain C99, no Amiga calls.
 *
 * - buffers (iu_*): one per server, channel and query. Each keeps its
 *   scrollback in a gfopennetkit text view (IRC colours and links parsed
 *   there), counts what came in while it wasn't shown, and notes a
 *   highlight (our nick said in it). The core's events (irc.h) are routed
 *   to the right buffer as readable lines;
 * - the nick list (iu_nicks_*): a channel's members sorted as IRC clients
 *   do, highest prefix first, then by nick;
 * - the input line (iu_hist_*, iu_complete): a history per buffer, walked
 *   with Up and Down, and nick completion with Tab.
 *
 * Passwords never pass through here: the program asks gfopenkeys.library
 * for them (service "irc-sasl" or "irc-server", the host, the nick) when it
 * connects.
 *
 * MIT licence, (c) 2026 AmiCradle Team. */
#ifndef GFOPENIRC_UI_MODEL_H
#define GFOPENIRC_UI_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "gfopennetkit/textview.h"
#include "gfopenirc/irc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { IU_SERVER = 0, IU_CHANNEL, IU_QUERY };

/* What a scrollback line is (NKTvLine.kind) */
enum {
    IU_L_MESSAGE = 1, IU_L_OWN, IU_L_ACTION, IU_L_NOTICE, IU_L_JOIN, IU_L_PART, IU_L_QUIT,
    IU_L_NICK, IU_L_KICK, IU_L_TOPIC, IU_L_MODE, IU_L_SERVER, IU_L_ERROR, IU_L_HIGHLIGHT
};

#define IU_LOG_LINES 2000        /* each buffer's scrollback */
#define IU_LOG_BYTES (256 * 1024)
#define IU_HISTORY   50          /* input lines kept per buffer */

typedef struct IuBuffer {
    char        name[200];       /* the server's name, a channel, a nick */
    int         kind;            /* IU_SERVER, IU_CHANNEL, IU_QUERY */
    NKTextView *log;
    uint32_t    unread;          /* lines that came while another buffer was shown */
    int         highlight;       /* our nick was said in one of them */
    int         joined;          /* a channel we are in (0 after PART or KICK) */
    char        topic[512];
} IuBuffer;

typedef struct IuState IuState;

/* A server's buffers; server is its name in the list ("" until the
 * server's NETWORK arrives: the host given). */
IuState *iu_new(const char *server);
void     iu_free(IuState *s);
size_t   iu_count(const IuState *s);
IuBuffer *iu_buffer(IuState *s, size_t i);
/* The buffer of name (any case, IRC's casemapping), or -1. */
long     iu_find(const IuState *s, const char *name);
/* The buffer of name, made when it isn't there (a channel when its name
 * starts with a channel type, else a query). -1 when out of memory. */
long     iu_open(IuState *s, const char *name);
/* Closes a channel's or query's buffer (not the server's). 1 when it went. */
int      iu_close(IuState *s, size_t i);
/* The one shown: its unread count and highlight are cleared. IU_NONE (or
 * any i past the last) when another server's buffer is shown, so every
 * buffer here counts what comes. */
#define IU_NONE ((size_t)-1)
void     iu_show(IuState *s, size_t i);
size_t   iu_shown(const IuState *s);
/* The list's label: "#amiga", "#amiga (3)", "#amiga (3!)" for a highlight;
 * channels and queries under the server are indented two spaces. */
size_t   iu_label(const IuState *s, size_t i, char *buf, size_t cap);

/* ---- what the core reports -------------------------------------------- */

/* Settings that change how events read. chantypes from the server's
 * ISUPPORT ("#&" until then); nick is ours. */
void     iu_set_nick(IuState *s, const char *nick);
void     iu_set_chantypes(IuState *s, const char *chantypes, int casemap);
void     iu_set_server(IuState *s, const char *name);

/* An event from ir_new's callback: a line in the right buffer (made when a
 * message or a join of ours needs one). now is "HH:MM" for lines without
 * server-time. Answers the buffer it went to, or -1 (out of memory). Sets
 * *members to 1 when the buffer's nick list changed. */
long     iu_event(IuState *s, const IrEvent *e, const char *now, int *members);
/* A line of our own (what we said, as the server won't echo it back without
 * echo-message). */
long     iu_own(IuState *s, const char *buffer, const char *nick, const char *text, int action, const char *now);
/* A line from the program itself in a buffer ("Connecting to ..."). */
long     iu_note(IuState *s, const char *buffer, const char *text, const char *now);
/* "HH:MM" of a server-time tag ("2026-11-13T20:30:00.000Z"), else now. */
const char *iu_time(const char *server_time, const char *now, char out[6]);
/* Our nick said as a word in text (any case, IRC's word edges). */
int      iu_mentions(const char *text, const char *nick, int casemap);

/* ---- the nick list ------------------------------------------------------- */

typedef struct IuNick {
    char nick[64];
    char prefix;                 /* the highest prefix ('@', '+', ...) or 0 */
    int  rank;                   /* its place in PREFIX (0 highest); the plain last */
    int  away;
} IuNick;

typedef struct IuNicks {
    IuNick *n;
    size_t  count, cap;
} IuNicks;

void   iu_nicks_init(IuNicks *l);
void   iu_nicks_free(IuNicks *l);
/* The members given (as ir_member hands them), sorted: by rank, then nick
 * (any case). prefix_chars is the server's PREFIX symbols, highest first. */
int    iu_nicks_set(IuNicks *l, const IrMember *const *m, size_t n, const char *prefix_chars, int casemap);
/* "@ann", "+bob", "carl"; away ones in brackets: "(dave)". */
size_t iu_nicks_label(const IuNicks *l, size_t i, char *buf, size_t cap);

/* ---- the input line ---------------------------------------------------- */

typedef struct IuHistory {
    char  *line[IU_HISTORY];
    size_t count;                /* held, newest last */
    long   at;                   /* where Up and Down are: count = past the newest */
} IuHistory;

void   iu_hist_init(IuHistory *h);
void   iu_hist_free(IuHistory *h);
/* A line sent: kept unless it is the same as the newest. Up and Down start again past it. */
void   iu_hist_add(IuHistory *h, const char *line);
/* Up: the line before (NULL at the oldest: stay). Down: the line after, or
 * "" past the newest. */
const char *iu_hist_up(IuHistory *h);
const char *iu_hist_down(IuHistory *h);

/* Tab: completes the word before the caret from the nick list (the first
 * nick, by the list's order, that starts with it, any case; again with the
 * same word, the next one after `after`). At the start of the line ": "
 * follows the nick, else " " (only ":" or nothing when a space
 * follows already, the caret then past it). Writes the new line into out and
 * answers the caret's place in it, or -1 when nothing matches. */
long   iu_complete(const IuNicks *l, const char *line, size_t caret, const char *after,
                   int casemap, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
