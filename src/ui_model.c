/* GFOpenIRC chat window models: buffers, the nick list, the input line.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfopenirc/ui_model.h"

struct IuState {
    IuBuffer *b;
    size_t    n, cap;
    size_t    shown;
    char      nick[64];
    char      chantypes[16];
    int       casemap;
};

static void copy(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s ? s : "");
    if (n >= cap) n = cap - 1;
    memcpy(d, s ? s : "", n);
    d[n] = 0;
}

/* one character folded as IRC's casemapping does */
static int fold(int cm, int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (cm != IR_CM_ASCII) {
        if (c == '[') return '{';
        if (c == ']') return '}';
        if (c == '\\') return '|';
        if (c == '~' && cm == IR_CM_RFC1459) return '^';
    }
    return c;
}

static int starts_with(int cm, const char *s, const char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!s[i] || fold(cm, (unsigned char)s[i]) != fold(cm, (unsigned char)p[i])) return 0;
    return 1;
}

/* ---- buffers ---------------------------------------------------------------- */

static long add_buffer(IuState *s, const char *name, int kind)
{
    IuBuffer *b;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        IuBuffer *nb = realloc(s->b, cap * sizeof(*nb));
        if (!nb) return -1;
        s->b = nb; s->cap = cap;
    }
    b = &s->b[s->n];
    memset(b, 0, sizeof(*b));
    if (!(b->log = nk_tv_new(IU_LOG_LINES, IU_LOG_BYTES))) return -1;
    copy(b->name, sizeof(b->name), name);
    b->kind = kind;
    b->joined = kind == IU_CHANNEL;
    return (long)s->n++;
}

IuState *iu_new(const char *server)
{
    IuState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    copy(s->chantypes, sizeof(s->chantypes), "#&");
    s->casemap = IR_CM_RFC1459;
    if (add_buffer(s, server && *server ? server : "server", IU_SERVER) < 0) { free(s); return NULL; }
    return s;
}

void iu_free(IuState *s)
{
    size_t i;
    if (!s) return;
    for (i = 0; i < s->n; i++) nk_tv_free(s->b[i].log);
    free(s->b);
    free(s);
}

size_t iu_count(const IuState *s) { return s->n; }
IuBuffer *iu_buffer(IuState *s, size_t i) { return i < s->n ? &s->b[i] : NULL; }
size_t iu_shown(const IuState *s) { return s->shown; }

long iu_find(const IuState *s, const char *name)
{
    size_t i;
    if (!name) return 0;
    for (i = 1; i < s->n; i++)
        if (!ir_casecmp(s->casemap, s->b[i].name, name)) return (long)i;
    return -1;
}

static int is_channel(const IuState *s, const char *name) { return name && *name && strchr(s->chantypes, *name); }

long iu_open(IuState *s, const char *name)
{
    long i = iu_find(s, name);
    if (i >= 0) return i;
    return add_buffer(s, name, is_channel(s, name) ? IU_CHANNEL : IU_QUERY);
}

int iu_close(IuState *s, size_t i)
{
    if (!i || i >= s->n) return 0;
    nk_tv_free(s->b[i].log);
    memmove(&s->b[i], &s->b[i + 1], (s->n - i - 1) * sizeof(*s->b));
    s->n--;
    if (s->shown == IU_NONE) ;
    else if (s->shown == i) s->shown = i - 1;
    else if (s->shown > i) s->shown--;
    return 1;
}

void iu_show(IuState *s, size_t i)
{
    if (i >= s->n) { s->shown = IU_NONE; return; }   /* another server's buffer is shown */
    s->shown = i;
    s->b[i].unread = 0;
    s->b[i].highlight = 0;
}

size_t iu_label(const IuState *s, size_t i, char *buf, size_t cap)
{
    const IuBuffer *b;
    int n;
    if (i >= s->n) return (size_t)snprintf(buf, cap, "%s", "");
    b = &s->b[i];
    if (b->unread)
        n = snprintf(buf, cap, "%s%s%s (%lu%s)", b->kind == IU_SERVER ? "" : "  ", b->joined || b->kind != IU_CHANNEL ? "" : "-",
                     b->name, (unsigned long)b->unread, b->highlight ? "!" : "");
    else
        n = snprintf(buf, cap, "%s%s%s", b->kind == IU_SERVER ? "" : "  ", b->joined || b->kind != IU_CHANNEL ? "" : "-", b->name);
    return n < 0 ? 0 : (size_t)n;
}

void iu_set_nick(IuState *s, const char *nick) { copy(s->nick, sizeof(s->nick), nick); }
void iu_set_server(IuState *s, const char *name) { if (name && *name) copy(s->b[0].name, sizeof(s->b[0].name), name); }

void iu_set_chantypes(IuState *s, const char *chantypes, int casemap)
{
    if (chantypes && *chantypes) copy(s->chantypes, sizeof(s->chantypes), chantypes);
    s->casemap = casemap;
}

/* ---- lines ---------------------------------------------------------------------- */

const char *iu_time(const char *st, const char *now, char out[6])
{
    /* "YYYY-MM-DDTHH:MM..." */
    if (st && strlen(st) >= 16 && st[10] == 'T' && st[13] == ':') {
        memcpy(out, st + 11, 5);
        out[5] = 0;
        return out;
    }
    copy(out, 6, now && *now ? now : "--:--");
    return out;
}

static int nickchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("[]\\`_^{|}-", c));
}

int iu_mentions(const char *text, const char *nick, int cm)
{
    size_t n = strlen(nick ? nick : ""), i;
    if (!n || !text) return 0;
    for (i = 0; text[i]; i++) {
        if ((i == 0 || !nickchar((unsigned char)text[i - 1])) && starts_with(cm, text + i, nick, n) &&
            !nickchar((unsigned char)text[i + n]))
            return 1;
    }
    return 0;
}

/* Adds "[HH:MM] line" to buffer i. */
static long put(IuState *s, long i, int kind, const char *when, const char *fmt_a, const char *a,
                const char *fmt_b, const char *b, const char *tail)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "[%s] %s%s%s%s%s", when, fmt_a ? fmt_a : "", a ? a : "",
                     fmt_b ? fmt_b : "", b ? b : "", tail ? tail : "");
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    if (!nk_tv_add(s->b[i].log, line, (size_t)n, NK_TV_MIRC, (uint8_t)kind)) return -1;
    return i;
}

/* A message-like line counts as unread when its buffer isn't shown. */
static void count(IuState *s, long i, int highlight)
{
    if (i < 0 || (size_t)i == s->shown) return;
    s->b[i].unread++;
    if (highlight) s->b[i].highlight = 1;
}

long iu_event(IuState *s, const IrEvent *e, const char *now, int *members)
{
    char t[6], tail[600];
    const char *when = iu_time(e->time, now, t);
    const char *nick = e->nick ? e->nick : "";
    const char *text = e->text ? e->text : "";
    long i = e->target ? iu_find(s, e->target) : 0;
    int m = 0, hl;
    long r = -1;
    if (members) *members = 0;
    switch (e->type) {
    case IR_EV_MESSAGE: case IR_EV_ACTION: case IR_EV_NOTICE:
        if (i < 0 && (i = iu_open(s, e->target)) < 0) return -1;
        hl = !e->self && (s->b[i].kind == IU_QUERY || iu_mentions(text, s->nick, s->casemap));
        if (e->type == IR_EV_MESSAGE)
            r = put(s, i, e->self ? IU_L_OWN : hl ? IU_L_HIGHLIGHT : IU_L_MESSAGE, when, "<", nick, "> ", text, NULL);
        else if (e->type == IR_EV_ACTION)
            r = put(s, i, e->self ? IU_L_OWN : hl ? IU_L_HIGHLIGHT : IU_L_ACTION, when, "* ", nick, " ", text, NULL);
        else
            r = put(s, i, IU_L_NOTICE, when, "-", nick, "- ", text, NULL);
        if (!e->self) count(s, i, hl);
        break;
    case IR_EV_CTCP:
        r = put(s, 0, IU_L_SERVER, when, NULL, nick, " asked for CTCP ", text, NULL);
        break;
    case IR_EV_JOIN:
        if (i < 0) {
            if (!e->self) return -1;          /* a join we don't show */
            if ((i = iu_open(s, e->target)) < 0) return -1;
        }
        if (e->self) {
            s->b[i].joined = 1;
            r = put(s, i, IU_L_JOIN, when, "--> You joined ", s->b[i].name, NULL, NULL, NULL);
        } else r = put(s, i, IU_L_JOIN, when, "--> ", nick, " joined ", e->target, NULL);
        m = 1;
        break;
    case IR_EV_PART:
        if (i < 0) return -1;
        snprintf(tail, sizeof(tail), "%s%s%s", *text ? " (" : "", text, *text ? ")" : "");
        if (e->self) { s->b[i].joined = 0; r = put(s, i, IU_L_PART, when, "<-- You left ", s->b[i].name, NULL, NULL, tail); }
        else r = put(s, i, IU_L_PART, when, "<-- ", nick, " left ", e->target, tail);
        m = 1;
        break;
    case IR_EV_QUIT:
        if (i < 0) return -1;
        snprintf(tail, sizeof(tail), "%s%s%s", *text ? " (" : "", text, *text ? ")" : "");
        r = put(s, i, IU_L_QUIT, when, "<-- ", nick, " quit", NULL, tail);
        m = 1;
        break;
    case IR_EV_NICK:
        if (e->self && e->extra) iu_set_nick(s, e->extra);
        if (i < 0) return e->target ? -1 : 0;
        r = put(s, i, IU_L_NICK, when, NULL, nick, " is now known as ", e->extra, NULL);
        m = 1;
        break;
    case IR_EV_KICK:
        if (i < 0) return -1;
        snprintf(tail, sizeof(tail), "%s%s%s", *text ? " (" : "", text, *text ? ")" : "");
        if (e->extra && !ir_casecmp(s->casemap, e->extra, s->nick)) {
            s->b[i].joined = 0;
            r = put(s, i, IU_L_KICK, when, "You were kicked by ", nick, NULL, NULL, tail);
            count(s, i, 1);
        } else r = put(s, i, IU_L_KICK, when, NULL, nick, " kicked ", e->extra, tail);
        m = 1;
        break;
    case IR_EV_TOPIC:
        if (i < 0) return -1;
        copy(s->b[i].topic, sizeof(s->b[i].topic), text);
        if (*nick) r = put(s, i, IU_L_TOPIC, when, NULL, nick, " changed the topic to: ", text, NULL);
        else r = put(s, i, IU_L_TOPIC, when, "Topic: ", text, NULL, NULL, NULL);
        break;
    case IR_EV_MODE:
        if (i < 0) i = 0;
        r = put(s, i, IU_L_MODE, when, NULL, *nick ? nick : "The server", " sets mode ", text, NULL);
        m = s->b[i].kind == IU_CHANNEL;
        break;
    case IR_EV_NAMES: case IR_EV_AWAY: case IR_EV_ACCOUNT:
        if (members) *members = i > 0;
        return i < 0 ? 0 : i;
    case IR_EV_INVITE:
        r = put(s, 0, IU_L_SERVER, when, NULL, nick, " invites you to ", e->extra && *e->extra ? e->extra : text, NULL);
        count(s, 0, 1);
        break;
    case IR_EV_ERROR:
        if (i < 0) i = 0;
        r = put(s, i, IU_L_ERROR, when, "! ", text, NULL, NULL, NULL);
        break;
    case IR_EV_SASL:
        r = put(s, 0, IU_L_SERVER, when, "SASL login: ", e->extra && !strcmp(e->extra, "ok") ? "done" : "failed",
                NULL, NULL, NULL);
        break;
    case IR_EV_CAPS:
        r = put(s, 0, IU_L_SERVER, when, "Capabilities: ", *text ? text : "none", NULL, NULL, NULL);
        break;
    default:          /* REGISTERED, SERVER */
        if (i < 0) i = 0;
        r = put(s, i, IU_L_SERVER, when, NULL, text, NULL, NULL, NULL);
        break;
    }
    if (members) *members = m;
    return r;
}

long iu_own(IuState *s, const char *buffer, const char *nick, const char *text, int action, const char *now)
{
    char t[6];
    long i = buffer ? iu_open(s, buffer) : 0;
    if (i < 0) return -1;
    return action ? put(s, i, IU_L_OWN, iu_time(NULL, now, t), "* ", nick, " ", text, NULL)
                  : put(s, i, IU_L_OWN, iu_time(NULL, now, t), "<", nick, "> ", text, NULL);
}

long iu_note(IuState *s, const char *buffer, const char *text, const char *now)
{
    char t[6];
    long i = buffer ? iu_find(s, buffer) : 0;
    if (i < 0) i = 0;
    return put(s, i, IU_L_SERVER, iu_time(NULL, now, t), "-- ", text, NULL, NULL, NULL);
}

/* ---- the nick list ---------------------------------------------------------------- */

void iu_nicks_init(IuNicks *l) { memset(l, 0, sizeof(*l)); }
void iu_nicks_free(IuNicks *l) { free(l->n); memset(l, 0, sizeof(*l)); }

static int sort_cm;
static int by_rank(const void *a, const void *b)
{
    const IuNick *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return ir_casecmp(sort_cm, x->nick, y->nick);
}

int iu_nicks_set(IuNicks *l, const IrMember *const *m, size_t n, const char *prefix_chars, int casemap)
{
    size_t i, plain = strlen(prefix_chars ? prefix_chars : "");
    if (n > l->cap) {
        IuNick *nn = realloc(l->n, n * sizeof(*nn));
        if (!nn) return NK_ENOMEM;
        l->n = nn; l->cap = n;
    }
    for (i = 0; i < n; i++) {
        IuNick *k = &l->n[i];
        const char *p;
        copy(k->nick, sizeof(k->nick), m[i]->nick);
        k->prefix = m[i]->prefixes[0];
        p = k->prefix && prefix_chars ? strchr(prefix_chars, k->prefix) : NULL;
        k->rank = p ? (int)(p - prefix_chars) : (int)plain;
        if (!p) k->prefix = 0;
        k->away = m[i]->away;
    }
    l->count = n;
    sort_cm = casemap;
    qsort(l->n, n, sizeof(*l->n), by_rank);
    return NK_OK;
}

size_t iu_nicks_label(const IuNicks *l, size_t i, char *buf, size_t cap)
{
    char p[2] = { 0, 0 };
    int n;
    if (i >= l->count) return (size_t)snprintf(buf, cap, "%s", "");
    p[0] = l->n[i].prefix;
    n = l->n[i].away ? snprintf(buf, cap, "(%s%s)", p, l->n[i].nick) : snprintf(buf, cap, "%s%s", p, l->n[i].nick);
    return n < 0 ? 0 : (size_t)n;
}

/* ---- the input line ------------------------------------------------------------------ */

void iu_hist_init(IuHistory *h) { memset(h, 0, sizeof(*h)); }

void iu_hist_free(IuHistory *h)
{
    size_t i;
    for (i = 0; i < h->count; i++) free(h->line[i]);
    memset(h, 0, sizeof(*h));
}

void iu_hist_add(IuHistory *h, const char *line)
{
    size_t n;
    char *c;
    if (!line || !*line) { h->at = (long)h->count; return; }
    if (h->count && !strcmp(h->line[h->count - 1], line)) { h->at = (long)h->count; return; }
    n = strlen(line);
    if (!(c = malloc(n + 1))) return;
    memcpy(c, line, n + 1);
    if (h->count == IU_HISTORY) {
        free(h->line[0]);
        memmove(h->line, h->line + 1, (IU_HISTORY - 1) * sizeof(*h->line));
        h->count--;
    }
    h->line[h->count++] = c;
    h->at = (long)h->count;
}

const char *iu_hist_up(IuHistory *h)
{
    if (h->at <= 0) return NULL;
    return h->line[--h->at];
}

const char *iu_hist_down(IuHistory *h)
{
    if (h->at >= (long)h->count) return NULL;
    h->at++;
    return h->at == (long)h->count ? "" : h->line[h->at];
}

long iu_complete(const IuNicks *l, const char *line, size_t caret, const char *after, int cm, char *out, size_t cap)
{
    size_t ws = caret, len = strlen(line), i, start = 0, k;
    const char *nick = NULL, *suffix;
    int n;
    if (caret > len) caret = ws = len;
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    if (ws == caret || !l->count) return -1;
    if (after && *after) {                    /* again: from the one after `after` */
        for (i = 0; i < l->count; i++)
            if (!ir_casecmp(cm, l->n[i].nick, after)) { start = i + 1; break; }
    }
    for (k = 0; k < l->count; k++) {
        i = (start + k) % l->count;
        if (starts_with(cm, l->n[i].nick, line + ws, caret - ws)) { nick = l->n[i].nick; break; }
    }
    if (!nick) return -1;
    if (line[caret] == ' ') {                 /* a space follows already: the caret goes past it */
        suffix = ws == 0 ? ":" : "";
        n = snprintf(out, cap, "%.*s%s%s%s", (int)ws, line, nick, suffix, line + caret);
        if (n < 0) return -1;
        return (long)(ws + strlen(nick) + strlen(suffix) + 1);
    }
    suffix = ws == 0 ? ": " : " ";
    n = snprintf(out, cap, "%.*s%s%s%s", (int)ws, line, nick, suffix, line + caret);
    if (n < 0) return -1;
    return (long)(ws + strlen(nick) + strlen(suffix));
}
