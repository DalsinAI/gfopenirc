/* GFOpenIRC client state: registration with CAP LS 302 and SASL PLAIN,
 * events to the app, channels and members, throttled sending.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfopenirc/irc.h"

static const char *const WANT_CAPS[] = {
    "sasl", "server-time", "message-tags", "multi-prefix", "away-notify",
    "account-notify", "extended-join", "echo-message", "batch", "chghost",
    "invite-notify", "cap-notify", 0
};

typedef struct Chan {
    char      name[200];
    char     *topic;
    IrMember *m;
    size_t    n, cap;
    int       in_names;        /* a 353 run is filling the list */
} Chan;

struct IrClient {
    NKConn   *conn;
    IrEventFn fn;
    void     *user;
    IrSupport sup;
    char      nick[64], user_name[64], realname[128];
    char      server_pass[256], sasl_user[128], sasl_pass[512];
    int       sasl_required;
    uint32_t  burst, interval, ping_ms;
    int       registered, quitting;
    int       cap_open;        /* CAP negotiation not ended yet */
    int       cap_pending;     /* REQs not answered */
    int       sasl_state;      /* 0 none, 1 asked PLAIN, 2 sent, 3 done */
    char      ls[4096];        /* capabilities offered (with values) */
    char      caps[512];       /* in use */
    Chan     *ch;
    size_t    nch;
    char    **q;               /* throttled lines */
    size_t    qh, qn, qcap;
    uint32_t  credit, last_credit;
    uint32_t  now, last_rx, ping_sent;
    int       ping_out;
    uint32_t  last_ctcp;
    int       ctcp_any;
    char      scratch[1024];
    char      jbuf[1024];      /* joined parameters handed to events */
};

/* ---- small helpers ---------------------------------------------------------- */
#define LAST(m) ((m)->nparams ? (m)->params[(m)->nparams - 1] : "")

static void scopy(char *d, size_t cap, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(d, s, n);
    d[n] = 0;
}

static int eqnick(const IrClient *c, const char *a, const char *b)
{
    return a && b && ir_casecmp(c->sup.casemap, a, b) == 0;
}

static void emit(IrClient *c, int type, const char *target, const char *nick,
                 const char *text, const char *extra, const IrMsg *m)
{
    IrEvent e;
    if (!c->fn) return;
    e.type = type;
    e.target = target;
    e.nick = nick;
    e.text = text;
    e.extra = extra;
    e.time = m ? ir_tag(m, "time") : 0;
    e.self = nick && eqnick(c, nick, c->nick);
    e.msg = m;
    c->fn(c->user, &e);
}

static int bad_text(const char *s)
{
    return !s || strpbrk(s, "\r\n") != 0;
}

/* straight to the connection: registration, PONG, keepalive, QUIT */
static int direct(IrClient *c, const char *line)
{
    return nk_conn_sendline(c->conn, line);
}

static int queue_line(IrClient *c, const char *line)
{
    char *s;
    if (bad_text(line) || !*line)
        return NK_EARG;
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16, i;
        char **nq = (char **)malloc(nc * sizeof(char *));
        if (!nq) return NK_ENOMEM;
        for (i = 0; i < c->qn; i++)
            nq[i] = c->q[(c->qh + i) % c->qcap];
        free(c->q);
        c->q = nq;
        c->qh = 0;
        c->qcap = nc;
    }
    s = (char *)malloc(strlen(line) + 1);
    if (!s) return NK_ENOMEM;
    strcpy(s, line);
    c->q[(c->qh + c->qn++) % c->qcap] = s;
    return NK_OK;
}

static void flush_queue(IrClient *c)
{
    uint32_t full = c->burst * c->interval;
    uint32_t el = c->now - c->last_credit;
    c->last_credit = c->now;
    c->credit = el >= full || c->credit + el >= full ? full : c->credit + el;
    while (c->qn && c->credit >= c->interval) {
        char *s = c->q[c->qh];
        if (nk_conn_sendline(c->conn, s) == NK_ENOMEM)
            break;                 /* output full: try on the next pump */
        free(s);
        c->qh = (c->qh + 1) % c->qcap;
        c->qn--;
        c->credit -= c->interval;
    }
}

static int sendf(IrClient *c, int throttled, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(c->scratch, sizeof(c->scratch), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(c->scratch))
        return NK_EARG;
    if (bad_text(c->scratch))
        return NK_EARG;
    if (throttled) {
        int r = queue_line(c, c->scratch);
        if (r == NK_OK) flush_queue(c);
        return r;
    }
    return direct(c, c->scratch);
}

/* ---- capabilities ------------------------------------------------------------- */
static const char *ls_value(const IrClient *c, const char *cap, int *found)
{
    const char *p = c->ls;
    size_t n = strlen(cap);
    *found = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!strncmp(p, cap, n) && (p[n] == ' ' || p[n] == '=' || !p[n])) {
            *found = 1;
            return p[n] == '=' ? p + n + 1 : "";
        }
        while (*p && *p != ' ') p++;
    }
    return 0;
}

int ir_has_cap(const IrClient *c, const char *cap)
{
    const char *p = c->caps;
    size_t n = strlen(cap);
    while (*p) {
        while (*p == ' ') p++;
        if (!strncmp(p, cap, n) && (p[n] == ' ' || !p[n]))
            return 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

static void cap_set(IrClient *c, const char *cap, size_t n, int on)
{
    char name[64];
    if (!n || n >= sizeof(name)) return;
    memcpy(name, cap, n);
    name[n] = 0;
    if (on && !ir_has_cap(c, name)) {
        if (strlen(c->caps) + n + 2 < sizeof(c->caps)) {
            if (c->caps[0]) strcat(c->caps, " ");
            strcat(c->caps, name);
        }
    } else if (!on) {
        char *p = c->caps;
        while ((p = strstr(p, name)) != 0) {
            if ((p == c->caps || p[-1] == ' ') && (p[n] == ' ' || !p[n])) {
                char *e = p + n + (p[n] == ' ');
                memmove(p, e, strlen(e) + 1);
                n = strlen(c->caps);
                while (n && c->caps[n - 1] == ' ') c->caps[--n] = 0;
                return;
            }
            p++;
        }
    }
}

static int want_sasl(const IrClient *c)
{
    return c->sasl_user[0] && c->sasl_pass[0];
}

/* ask for what we want of what's offered; answers REQs sent */
static int request_caps(IrClient *c, int after_registration)
{
    char req[400];
    int i, sent = 0;
    req[0] = 0;
    for (i = 0; WANT_CAPS[i]; i++) {
        int found;
        const char *v = ls_value(c, WANT_CAPS[i], &found);
        if (!found || ir_has_cap(c, WANT_CAPS[i])) continue;
        if (!strcmp(WANT_CAPS[i], "sasl")) {
            if (after_registration || !want_sasl(c)) continue;
            if (v && *v && !strstr(v, "PLAIN")) continue;
        }
        if (strlen(req) + strlen(WANT_CAPS[i]) + 2 > sizeof(req) - 1) {
            sendf(c, 0, "CAP REQ :%s", req);
            sent++;
            req[0] = 0;
        }
        if (req[0]) strcat(req, " ");
        strcat(req, WANT_CAPS[i]);
    }
    if (req[0]) {
        sendf(c, 0, "CAP REQ :%s", req);
        sent++;
    }
    return sent;
}

static void cap_end(IrClient *c)
{
    if (!c->cap_open) return;
    c->cap_open = 0;
    direct(c, "CAP END");
    emit(c, IR_EV_CAPS, 0, 0, c->caps, 0, 0);
}

/* a store the compiler can't drop */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void wipe_pass(IrClient *c)
{
    wipe(c->sasl_pass, sizeof(c->sasl_pass));
}

static void caps_settled(IrClient *c)
{
    if (c->cap_pending > 0 || !c->cap_open) return;
    if (ir_has_cap(c, "sasl") && want_sasl(c) && !c->sasl_state) {
        c->sasl_state = 1;
        direct(c, "AUTHENTICATE PLAIN");
        return;
    }
    if (want_sasl(c) && !ir_has_cap(c, "sasl") && c->sasl_required) {
        emit(c, IR_EV_SASL, 0, 0, "the server offers no SASL PLAIN", "failed", 0);
        wipe_pass(c);
        ir_quit(c, 0);
        return;
    }
    cap_end(c);
}

static void on_cap(IrClient *c, const IrMsg *m)
{
    const char *sub = m->nparams > 1 ? m->params[1] : "";
    const char *list = m->nparams > 2 ? LAST(m) : "";
    if (!strcmp(sub, "LS")) {
        int more = m->nparams > 3 && !strcmp(m->params[2], "*");
        if (strlen(c->ls) + strlen(list) + 2 < sizeof(c->ls)) {
            if (c->ls[0]) strcat(c->ls, " ");
            strcat(c->ls, list);
        }
        if (more) return;
        c->cap_pending += request_caps(c, 0);
        caps_settled(c);
    } else if (!strcmp(sub, "ACK") || !strcmp(sub, "NAK")) {
        if (!strcmp(sub, "ACK")) {
            const char *p = list;
            while (*p) {
                const char *e;
                int on = 1;
                while (*p == ' ') p++;
                if (*p == '-') { on = 0; p++; }
                e = p;
                while (*e && *e != ' ') e++;
                cap_set(c, p, (size_t)(e - p), on);
                p = e;
            }
        }
        if (c->cap_pending > 0) c->cap_pending--;
        caps_settled(c);
    } else if (!strcmp(sub, "NEW")) {
        if (strlen(c->ls) + strlen(list) + 2 < sizeof(c->ls)) {
            if (c->ls[0]) strcat(c->ls, " ");
            strcat(c->ls, list);
        }
        c->cap_pending += request_caps(c, c->registered);
    } else if (!strcmp(sub, "DEL")) {
        const char *p = list;
        while (*p) {
            const char *e;
            while (*p == ' ') p++;
            e = p;
            while (*e && *e != ' ') e++;
            cap_set(c, p, (size_t)(e - p), 0);
            p = e;
        }
    }
}

static void on_authenticate(IrClient *c, const IrMsg *m)
{
    char raw[1024], b64[1400];
    size_t ul, pl, n, at;
    if (c->sasl_state != 1 || m->nparams < 1 || strcmp(m->params[0], "+"))
        return;
    ul = strlen(c->sasl_user);
    pl = strlen(c->sasl_pass);
    /* authzid \0 authcid \0 password, authzid = authcid */
    if (ul * 2 + pl + 2 > sizeof(raw)) {
        direct(c, "AUTHENTICATE *");
        wipe_pass(c);
        wipe(raw, sizeof(raw));
        return;
    }
    memcpy(raw, c->sasl_user, ul);
    raw[ul] = 0;
    memcpy(raw + ul + 1, c->sasl_user, ul);
    raw[2 * ul + 1] = 0;
    memcpy(raw + 2 * ul + 2, c->sasl_pass, pl);
    n = ir_b64(raw, 2 * ul + 2 + pl, b64, sizeof(b64));
    wipe(raw, sizeof(raw));
    wipe_pass(c);
    for (at = 0; at < n; at += 400) {
        char line[420];  /* 13 + 400 + NUL */
        size_t k = n - at > 400 ? 400 : n - at;
        memcpy(line, "AUTHENTICATE ", 13);
        memcpy(line + 13, b64 + at, k);
        line[13 + k] = 0;
        direct(c, line);
        wipe(line, sizeof(line));
    }
    if (n % 400 == 0)
        direct(c, "AUTHENTICATE +");
    wipe(b64, sizeof(b64));
    c->sasl_state = 2;
}

/* ---- channels ------------------------------------------------------------------ */
static Chan *find_chan(const IrClient *c, const char *name)
{
    size_t i;
    for (i = 0; name && i < c->nch; i++)
        if (!ir_casecmp(c->sup.casemap, c->ch[i].name, name))
            return &c->ch[i];
    return 0;
}

static Chan *add_chan(IrClient *c, const char *name)
{
    Chan *ch = find_chan(c, name), *n;
    if (ch) return ch;
    n = (Chan *)realloc(c->ch, (c->nch + 1) * sizeof(Chan));
    if (!n) return 0;
    c->ch = n;
    ch = &c->ch[c->nch++];
    memset(ch, 0, sizeof(*ch));
    scopy(ch->name, sizeof(ch->name), name);
    return ch;
}

static void drop_chan(IrClient *c, Chan *ch)
{
    size_t i = (size_t)(ch - c->ch);
    free(ch->topic);
    free(ch->m);
    memmove(&c->ch[i], &c->ch[i + 1], (c->nch - i - 1) * sizeof(Chan));
    c->nch--;
}

static IrMember *find_mem(const IrClient *c, Chan *ch, const char *nick)
{
    size_t i;
    for (i = 0; ch && i < ch->n; i++)
        if (eqnick(c, ch->m[i].nick, nick))
            return &ch->m[i];
    return 0;
}

static IrMember *add_mem(IrClient *c, Chan *ch, const char *nick)
{
    IrMember *m = find_mem(c, ch, nick);
    if (m) return m;
    if (ch->n == ch->cap) {
        size_t nc = ch->cap ? ch->cap * 2 : 8;
        IrMember *nm = (IrMember *)realloc(ch->m, nc * sizeof(IrMember));
        if (!nm) return 0;
        ch->m = nm;
        ch->cap = nc;
    }
    m = &ch->m[ch->n++];
    memset(m, 0, sizeof(*m));
    scopy(m->nick, sizeof(m->nick), nick);
    return m;
}

static void drop_mem(Chan *ch, IrMember *m)
{
    size_t i = (size_t)(m - ch->m);
    memmove(&ch->m[i], &ch->m[i + 1], (ch->n - i - 1) * sizeof(IrMember));
    ch->n--;
}

/* keep the prefixes in PREFIX order, highest first */
static void set_prefix(const IrClient *c, IrMember *m, char pc, int on)
{
    char out[16];
    size_t k = 0;
    const char *p;
    for (p = c->sup.prefix_chars; *p && k < sizeof(out) - 1; p++)
        if ((*p == pc) ? on : strchr(m->prefixes, *p) != 0)
            out[k++] = *p;
    out[k] = 0;
    strcpy(m->prefixes, out);
}

static void on_names(IrClient *c, const IrMsg *m)
{
    Chan *ch;
    const char *p;
    if (m->nparams < 4) return;
    ch = find_chan(c, m->params[2]);
    if (!ch) return;
    if (!ch->in_names) {
        ch->in_names = 1;
        ch->n = 0;
    }
    p = m->params[3];
    while (*p) {
        char pre[16], nick[64];
        size_t np = 0, nn = 0;
        IrMember *mem;
        while (*p == ' ') p++;
        if (!*p) break;
        while (*p && strchr(c->sup.prefix_chars, *p) && c->sup.prefix_chars[0]) {
            if (np < sizeof(pre) - 1) pre[np++] = *p;
            p++;
        }
        pre[np] = 0;
        while (*p && *p != ' ' && *p != '!') {
            if (nn < sizeof(nick) - 1) nick[nn++] = *p;
            p++;
        }
        nick[nn] = 0;
        while (*p && *p != ' ') p++;        /* userhost-in-names */
        if (!nn) continue;
        mem = add_mem(c, ch, nick);
        if (mem) {
            size_t i;
            mem->prefixes[0] = 0;
            for (i = 0; i < np; i++)
                set_prefix(c, mem, pre[i], 1);
        }
    }
}

static void on_mode(IrClient *c, const IrMsg *m)
{
    Chan *ch = find_chan(c, m->params[0]);
    const char *s;
    int set = 1, arg = 2;
    char text[512];
    size_t tl = 0;
    int i;
    for (i = 1; i < m->nparams; i++) {
        size_t n = strlen(m->params[i]);
        if (tl + n + 2 >= sizeof(text)) break;
        if (tl) text[tl++] = ' ';
        memcpy(text + tl, m->params[i], n);
        tl += n;
    }
    text[tl] = 0;
    if (!ir_is_channel(&c->sup, m->params[0])) {
        emit(c, IR_EV_MODE, 0, m->nick, text, m->params[0], m);
        return;
    }
    for (s = m->nparams > 1 ? m->params[1] : ""; *s; s++) {
        const char *pm;
        if (*s == '+' || *s == '-') { set = *s == '+'; continue; }
        if (!ir_mode_has_arg(&c->sup, *s, set)) continue;
        if (arg >= m->nparams) break;
        pm = strchr(c->sup.prefix_modes, *s);
        if (pm && ch) {
            IrMember *mem = find_mem(c, ch, m->params[arg]);
            if (mem) set_prefix(c, mem, c->sup.prefix_chars[pm - c->sup.prefix_modes], set);
        }
        arg++;
    }
    emit(c, IR_EV_MODE, m->params[0], m->nick, text, 0, m);
}

/* ---- messages ----------------------------------------------------------------- */
static const char *join_params(IrClient *c, const IrMsg *m, int from)
{
    size_t tl = 0;
    int i;
    for (i = from; i < m->nparams; i++) {
        size_t n = strlen(m->params[i]);
        if (tl + n + 2 >= sizeof(c->jbuf)) break;
        if (tl) c->jbuf[tl++] = ' ';
        memcpy(c->jbuf + tl, m->params[i], n);
        tl += n;
    }
    c->jbuf[tl] = 0;
    return c->jbuf;
}

static void ctcp_reply(IrClient *c, const char *to, const char *what, const char *arg)
{
    if (c->ctcp_any && c->now - c->last_ctcp < 2000)
        return;                       /* at most one reply in 2 s: no flood */
    c->ctcp_any = 1;
    c->last_ctcp = c->now;
    if (!strcmp(what, "VERSION"))
        sendf(c, 1, "NOTICE %s :\001VERSION " IR_VERSION " (AmigaOS)\001", to);
    else if (!strcmp(what, "PING") && arg && strlen(arg) < 64 && !strchr(arg, '\001'))
        sendf(c, 1, "NOTICE %s :\001PING %s\001", to, arg);
    else if (!strcmp(what, "CLIENTINFO"))
        sendf(c, 1, "NOTICE %s :\001CLIENTINFO ACTION CLIENTINFO PING VERSION\001", to);
}

static void on_msg(IrClient *c, const IrMsg *m, int notice)
{
    const char *target, *buf, *from = m->nick;
    char text[IR_LINE_MAX + 1];
    int self, server_src;
    if (m->nparams < 2) return;
    target = m->params[0];
    scopy(text, sizeof(text), m->params[1]);
    self = eqnick(c, from, c->nick);
    server_src = !from || (!m->user && !m->host && strchr(from, '.'));
    /* STATUSMSG: "@#chan" goes to #chan */
    if (strchr(c->sup.prefix_chars, target[0]) && target[0] && ir_is_channel(&c->sup, target + 1))
        target++;
    if (ir_is_channel(&c->sup, target))
        buf = target;
    else if (self)
        buf = target;
    else if (server_src || !c->registered || !strcmp(target, "*"))
        buf = 0;
    else
        buf = from;
    if (text[0] == '\001') {
        char *cmd = text + 1, *arg, *end = strchr(cmd, '\001');
        if (end) *end = 0;
        arg = strchr(cmd, ' ');
        if (arg) *arg++ = 0;
        if (!notice && !strcmp(cmd, "ACTION")) {
            emit(c, IR_EV_ACTION, buf, from, arg ? arg : "", 0, m);
            return;
        }
        if (!notice) {
            emit(c, IR_EV_CTCP, buf, from, cmd, arg, m);
            if (!self && from && !server_src)
                ctcp_reply(c, from, cmd, arg);
            return;
        }
        /* a CTCP reply shows as a notice with its words */
        if (arg) arg[-1] = ' ';
        emit(c, IR_EV_NOTICE, buf, from, cmd, "ctcp", m);
        return;
    }
    emit(c, notice ? IR_EV_NOTICE : IR_EV_MESSAGE, buf, from, text, 0, m);
}

static void alt_nick(IrClient *c, const char *tried)
{
    char n[64];
    size_t l;
    scopy(n, sizeof(n), tried && *tried && strcmp(tried, "*") ? tried : c->nick);
    l = strlen(n);
    if ((int)l < c->sup.nicklen && l < sizeof(n) - 1) {
        n[l] = '_';
        n[l + 1] = 0;
    } else if (l) {
        n[l - 1] = (char)(n[l - 1] >= '0' && n[l - 1] < '9' ? n[l - 1] + 1 : '1');
    }
    scopy(c->nick, sizeof(c->nick), n);
    sendf(c, 0, "NICK %s", c->nick);
}

static void handle(IrClient *c, const IrMsg *m)
{
    const char *cmd = m->cmd, *p0 = m->nparams > 0 ? m->params[0] : "";
    int num = (cmd[0] >= '0' && cmd[0] <= '9' && strlen(cmd) == 3) ? atoi(cmd) : -1;
    size_t i;

    if (!strcmp(cmd, "PING")) {
        sendf(c, 0, "PONG :%s", p0);
        return;
    }
    if (!strcmp(cmd, "PONG"))
        return;
    if (!strcmp(cmd, "CAP")) {
        on_cap(c, m);
        return;
    }
    if (!strcmp(cmd, "AUTHENTICATE")) {
        on_authenticate(c, m);
        return;
    }
    switch (num) {
    case 1:
        c->registered = 1;
        if (*p0) scopy(c->nick, sizeof(c->nick), p0);
        emit(c, IR_EV_REGISTERED, 0, 0, LAST(m), 0, m);
        return;
    case 5:
        ir_isupport_apply(&c->sup, m);
        emit(c, IR_EV_SERVER, 0, m->nick, join_params(c, m, 1), cmd, m);
        return;
    case 900:
        emit(c, IR_EV_SERVER, 0, m->nick, LAST(m), cmd, m);
        return;
    case 903: case 907:
        c->sasl_state = 3;
        emit(c, IR_EV_SASL, 0, 0, LAST(m), "ok", m);
        cap_end(c);
        return;
    case 902: case 904: case 905: case 906:
        c->sasl_state = 3;
        wipe_pass(c);
        emit(c, IR_EV_SASL, 0, 0, LAST(m), "failed", m);
        if (c->sasl_required)
            ir_quit(c, 0);
        else
            cap_end(c);
        return;
    case 908:
        return;
    case 432: case 433: case 436: case 437:
        if (!c->registered) {
            alt_nick(c, m->nparams > 1 ? m->params[1] : 0);
            return;
        }
        break;
    case 332:
        if (m->nparams >= 3) {
            Chan *ch = find_chan(c, m->params[1]);
            if (ch) {
                free(ch->topic);
                ch->topic = (char *)malloc(strlen(m->params[2]) + 1);
                if (ch->topic) strcpy(ch->topic, m->params[2]);
            }
            emit(c, IR_EV_TOPIC, m->params[1], 0, m->params[2], 0, m);
        }
        return;
    case 353:
        on_names(c, m);
        return;
    case 366:
        if (m->nparams >= 2) {
            Chan *ch = find_chan(c, m->params[1]);
            if (ch) ch->in_names = 0;
            emit(c, IR_EV_NAMES, m->params[1], 0, 0, 0, m);
        }
        return;
    default:
        break;
    }
    if (num >= 400 && num < 600) {
        const char *t = m->nparams > 2 && ir_is_channel(&c->sup, m->params[1]) ? m->params[1] : 0;
        emit(c, IR_EV_ERROR, t, m->nick, LAST(m),
             m->nparams > 2 ? m->params[1] : 0, m);
        return;
    }
    if (num >= 0) {
        emit(c, IR_EV_SERVER, 0, m->nick, join_params(c, m, 1), cmd, m);
        return;
    }
    if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
        on_msg(c, m, cmd[0] == 'N');
    } else if (!strcmp(cmd, "JOIN") && m->nparams >= 1 && m->nick) {
        Chan *ch = eqnick(c, m->nick, c->nick) ? add_chan(c, p0) : find_chan(c, p0);
        const char *acct = ir_has_cap(c, "extended-join") && m->nparams >= 3 ? m->params[1] : 0;
        if (ch) {
            IrMember *mem = add_mem(c, ch, m->nick);
            if (mem && acct) scopy(mem->account, sizeof(mem->account), acct);
        }
        emit(c, IR_EV_JOIN, p0, m->nick, 0, acct, m);
    } else if (!strcmp(cmd, "PART") && m->nparams >= 1 && m->nick) {
        Chan *ch = find_chan(c, p0);
        emit(c, IR_EV_PART, p0, m->nick, m->nparams > 1 ? m->params[1] : 0, 0, m);
        if (ch) {
            if (eqnick(c, m->nick, c->nick))
                drop_chan(c, ch);
            else {
                IrMember *mem = find_mem(c, ch, m->nick);
                if (mem) drop_mem(ch, mem);
            }
        }
    } else if (!strcmp(cmd, "KICK") && m->nparams >= 2) {
        Chan *ch = find_chan(c, p0);
        emit(c, IR_EV_KICK, p0, m->nick, m->nparams > 2 ? m->params[2] : 0, m->params[1], m);
        if (ch) {
            if (eqnick(c, m->params[1], c->nick))
                drop_chan(c, ch);
            else {
                IrMember *mem = find_mem(c, ch, m->params[1]);
                if (mem) drop_mem(ch, mem);
            }
        }
    } else if (!strcmp(cmd, "QUIT") && m->nick) {
        for (i = 0; i < c->nch; i++) {
            IrMember *mem = find_mem(c, &c->ch[i], m->nick);
            if (!mem) continue;
            emit(c, IR_EV_QUIT, c->ch[i].name, m->nick, m->nparams ? m->params[0] : 0, 0, m);
            drop_mem(&c->ch[i], mem);
        }
    } else if (!strcmp(cmd, "NICK") && m->nparams >= 1 && m->nick) {
        char old[64];
        int any = 0, self = eqnick(c, m->nick, c->nick);
        scopy(old, sizeof(old), m->nick);
        if (self) scopy(c->nick, sizeof(c->nick), p0);
        for (i = 0; i < c->nch; i++) {
            IrMember *mem = find_mem(c, &c->ch[i], old);
            if (!mem) continue;
            scopy(mem->nick, sizeof(mem->nick), p0);
            emit(c, IR_EV_NICK, c->ch[i].name, old, 0, p0, m);
            any = 1;
        }
        if (!any)
            emit(c, IR_EV_NICK, 0, old, 0, p0, m);
    } else if (!strcmp(cmd, "TOPIC") && m->nparams >= 2) {
        Chan *ch = find_chan(c, p0);
        if (ch) {
            free(ch->topic);
            ch->topic = (char *)malloc(strlen(m->params[1]) + 1);
            if (ch->topic) strcpy(ch->topic, m->params[1]);
        }
        emit(c, IR_EV_TOPIC, p0, m->nick, m->params[1], 0, m);
    } else if (!strcmp(cmd, "MODE") && m->nparams >= 2) {
        on_mode(c, m);
    } else if (!strcmp(cmd, "AWAY") && m->nick) {
        for (i = 0; i < c->nch; i++) {
            IrMember *mem = find_mem(c, &c->ch[i], m->nick);
            if (mem) mem->away = m->nparams > 0;
        }
        emit(c, IR_EV_AWAY, 0, m->nick, m->nparams ? p0 : 0, 0, m);
    } else if (!strcmp(cmd, "ACCOUNT") && m->nick && m->nparams >= 1) {
        for (i = 0; i < c->nch; i++) {
            IrMember *mem = find_mem(c, &c->ch[i], m->nick);
            if (mem) scopy(mem->account, sizeof(mem->account), p0);
        }
        emit(c, IR_EV_ACCOUNT, 0, m->nick, 0, p0, m);
    } else if (!strcmp(cmd, "INVITE") && m->nparams >= 2) {
        emit(c, IR_EV_INVITE, 0, m->nick, m->params[1], p0, m);
    } else if (!strcmp(cmd, "ERROR")) {
        emit(c, IR_EV_ERROR, 0, 0, p0, "ERROR", m);
    } else if (!strcmp(cmd, "CHGHOST") || !strcmp(cmd, "BATCH")) {
        /* nothing kept: hosts aren't shown, batches are read line by line */
    } else {
        emit(c, IR_EV_SERVER, 0, m->nick, join_params(c, m, 0), cmd, m);
    }
}

/* ---- public ------------------------------------------------------------------ */
int ir_config_check(const IrConfig *cfg)
{
    const char *u, *r;
    if (!cfg || !cfg->nick || !*cfg->nick || bad_text(cfg->nick) || strchr(cfg->nick, ' '))
        return NK_EARG;
    u = cfg->user && *cfg->user ? cfg->user : cfg->nick;
    r = cfg->realname && *cfg->realname ? cfg->realname : cfg->nick;
    if (bad_text(u) || strchr(u, ' ') || bad_text(r) ||
        (cfg->server_pass && bad_text(cfg->server_pass)) ||
        (cfg->sasl_user && bad_text(cfg->sasl_user)))
        return NK_EARG;
    if (!cfg->tls && !cfg->allow_plain_auth &&
        ((cfg->server_pass && *cfg->server_pass) ||
         (cfg->sasl_user && *cfg->sasl_user && cfg->sasl_pass && *cfg->sasl_pass)))
        return NK_EREFUSED;
    return NK_OK;
}

IrClient *ir_new(NKConn *conn, const IrConfig *cfg, IrEventFn fn, void *user, uint32_t now_ms)
{
    IrClient *c;
    if (!conn || ir_config_check(cfg) != NK_OK)
        return 0;
    c = (IrClient *)calloc(1, sizeof(IrClient));
    if (!c) return 0;
    c->conn = conn;
    c->fn = fn;
    c->user = user;
    ir_isupport_init(&c->sup);
    scopy(c->nick, sizeof(c->nick), cfg->nick);
    scopy(c->user_name, sizeof(c->user_name), cfg->user && *cfg->user ? cfg->user : cfg->nick);
    scopy(c->realname, sizeof(c->realname), cfg->realname && *cfg->realname ? cfg->realname : cfg->nick);
    scopy(c->server_pass, sizeof(c->server_pass), cfg->server_pass);
    scopy(c->sasl_user, sizeof(c->sasl_user), cfg->sasl_user);
    scopy(c->sasl_pass, sizeof(c->sasl_pass), cfg->sasl_pass);
    c->sasl_required = cfg->sasl_required;
    c->burst = cfg->burst ? cfg->burst : 5;
    c->interval = cfg->interval_ms ? cfg->interval_ms : 2000;
    c->ping_ms = cfg->ping_ms ? cfg->ping_ms : 90000;
    c->credit = c->burst * c->interval;
    c->now = c->last_credit = c->last_rx = now_ms;
    if (bad_text(c->user_name) || bad_text(c->realname) || strchr(c->user_name, ' ') ||
        strpbrk(c->server_pass, "\r\n")) {
        wipe(c, sizeof(*c));
        free(c);
        return 0;
    }
    c->cap_open = 1;
    direct(c, "CAP LS 302");
    if (c->server_pass[0]) {
        sendf(c, 0, "PASS %s", c->server_pass);
        wipe(c->scratch, sizeof(c->scratch));
    }
    wipe(c->server_pass, sizeof(c->server_pass));
    sendf(c, 0, "NICK %s", c->nick);
    sendf(c, 0, "USER %s 0 * :%s", c->user_name, c->realname);
    return c;
}

void ir_free(IrClient *c)
{
    size_t i;
    if (!c) return;
    while (c->nch)
        drop_chan(c, &c->ch[c->nch - 1]);
    free(c->ch);
    for (i = 0; i < c->qn; i++)
        free(c->q[(c->qh + i) % c->qcap]);
    free(c->q);
    nk_conn_free(c->conn);
    wipe(c, sizeof(*c));
    free(c);
}

int ir_pump(IrClient *c, uint32_t now_ms)
{
    int r;
    const char *l;
    size_t n;
    IrMsg *m = 0;
    c->now = now_ms;
    flush_queue(c);
    r = nk_conn_pump(c->conn, now_ms);
    for (;;) {
        int k = nk_conn_line(c->conn, &l, &n);
        if (k == 0) break;
        if (k != 1) continue;            /* an over-long line was dropped */
        c->last_rx = now_ms;
        c->ping_out = 0;
        if (!m && !(m = (IrMsg *)malloc(sizeof(IrMsg)))) { r = NK_ENOMEM; break; }
        if (ir_parse(m, l, n) == NK_OK)
            handle(c, m);
    }
    free(m);
    if (r != NK_OK)
        return r;
    flush_queue(c);
    if (c->ping_out && now_ms - c->ping_sent >= c->ping_ms)
        return NK_ETIMEDOUT;
    if (!c->ping_out && now_ms - c->last_rx >= c->ping_ms) {
        sendf(c, 0, "PING :gfopenirc");
        c->ping_out = 1;
        c->ping_sent = now_ms;
    }
    /* write what was queued while handling lines */
    return nk_conn_pump(c->conn, now_ms);
}

NKConn *ir_conn(IrClient *c) { return c->conn; }
int ir_registered(const IrClient *c) { return c->registered; }
const char *ir_nick(const IrClient *c) { return c->nick; }
const IrSupport *ir_support(const IrClient *c) { return &c->sup; }
const char *ir_caps(const IrClient *c) { return c->caps; }
int ir_quitting(const IrClient *c) { return c->quitting; }
size_t ir_queued(const IrClient *c) { return c->qn; }

int ir_raw(IrClient *c, const char *line)
{
    return queue_line(c, line) == NK_OK ? (flush_queue(c), NK_OK) : NK_EARG;
}

static int say_kind(IrClient *c, const char *cmd, const char *target, const char *text, int action)
{
    size_t len, room, over;
    int type = action ? IR_EV_ACTION : cmd[0] == 'N' ? IR_EV_NOTICE : IR_EV_MESSAGE;
    if (!target || !*target || strpbrk(target, " ,\r\n") || bad_text(text))
        return NK_EARG;
    /* what others see: ":nick!user@host CMD target :" plus CR LF */
    over = 1 + strlen(c->nick) + 1 + strlen(c->user_name) + 1 + 63 + 1 +
           strlen(cmd) + 1 + strlen(target) + 2 + 2 + (action ? 9 : 0);
    if (over >= 400)
        return NK_EARG;
    room = 512 - over;
    len = strlen(text);
    do {
        char piece[512];
        size_t k = ir_split_at(text, len, room);
        int r;
        memcpy(piece, text, k);
        piece[k] = 0;
        r = action ? sendf(c, 1, "%s %s :\001ACTION %s\001", cmd, target, piece)
                   : sendf(c, 1, "%s %s :%s", cmd, target, k ? piece : " ");
        if (r != NK_OK) return r;
        if (!ir_has_cap(c, "echo-message"))
            emit(c, type, target, c->nick, piece, 0, 0);
        text += k;
        len -= k;
        while (len && *text == ' ' && k < len) { text++; len--; }
    } while (len);
    return NK_OK;
}

int ir_say(IrClient *c, const char *target, const char *text) { return say_kind(c, "PRIVMSG", target, text, 0); }
int ir_notice(IrClient *c, const char *target, const char *text) { return say_kind(c, "NOTICE", target, text, 0); }
int ir_action(IrClient *c, const char *target, const char *text) { return say_kind(c, "PRIVMSG", target, text, 1); }

int ir_join(IrClient *c, const char *channel, const char *key)
{
    if (!channel || !*channel || strpbrk(channel, " \r\n") || (key && strpbrk(key, " \r\n")))
        return NK_EARG;
    if (!ir_is_channel(&c->sup, channel))
        return sendf(c, 1, key && *key ? "JOIN #%s %s" : "JOIN #%s", channel, key);
    return sendf(c, 1, key && *key ? "JOIN %s %s" : "JOIN %s", channel, key);
}

int ir_part(IrClient *c, const char *channel, const char *reason)
{
    if (!channel || !*channel || strpbrk(channel, " \r\n") || (reason && bad_text(reason)))
        return NK_EARG;
    return reason && *reason ? sendf(c, 1, "PART %s :%s", channel, reason) : sendf(c, 1, "PART %s", channel);
}

int ir_set_nick(IrClient *c, const char *nick)
{
    if (!nick || !*nick || strpbrk(nick, " \r\n,!@:"))
        return NK_EARG;
    if (!c->registered)
        scopy(c->nick, sizeof(c->nick), nick);
    return sendf(c, 1, "NICK %s", nick);
}

int ir_quit(IrClient *c, const char *reason)
{
    if (reason && bad_text(reason))
        return NK_EARG;
    c->quitting = 1;
    /* QUIT goes at once, after whatever is queued (the server may drop the
     * rest anyway) */
    while (c->qn) {
        nk_conn_sendline(c->conn, c->q[c->qh]);
        free(c->q[c->qh]);
        c->qh = (c->qh + 1) % c->qcap;
        c->qn--;
    }
    return sendf(c, 0, "QUIT :%s", reason && *reason ? reason : IR_VERSION);
}

int ir_command(IrClient *c, const char *buffer, const char *input)
{
    char cmd[32], *arg;
    const char *p;
    size_t n = 0;
    if (!input || bad_text(input))
        return NK_EARG;
    if (input[0] != '/' || input[1] == '/') {
        if (!buffer) return NK_EARG;
        return ir_say(c, buffer, input[0] == '/' ? input + 1 : input);
    }
    for (p = input + 1; *p && *p != ' ' && n < sizeof(cmd) - 1; p++)
        cmd[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    cmd[n] = 0;
    while (*p == ' ') p++;
    arg = (char *)p;
    if (!strcmp(cmd, "join") || !strcmp(cmd, "j")) {
        char ch[200], *sp;
        scopy(ch, sizeof(ch), arg);
        sp = strchr(ch, ' ');
        if (sp) *sp++ = 0;
        return ir_join(c, ch, sp);
    }
    if (!strcmp(cmd, "part") || !strcmp(cmd, "leave")) {
        if (ir_is_channel(&c->sup, arg)) {
            char ch[200], *sp;
            scopy(ch, sizeof(ch), arg);
            sp = strchr(ch, ' ');
            if (sp) *sp++ = 0;
            return ir_part(c, ch, sp);
        }
        return buffer && ir_is_channel(&c->sup, buffer) ? ir_part(c, buffer, arg) : NK_EARG;
    }
    if (!strcmp(cmd, "msg") || !strcmp(cmd, "notice") || !strcmp(cmd, "query")) {
        char to[200], *sp;
        scopy(to, sizeof(to), arg);
        sp = strchr(to, ' ');
        if (sp) *sp++ = 0;
        if (cmd[0] == 'q')
            return to[0] ? (sp && *sp ? ir_say(c, to, sp) : NK_OK) : NK_EARG;
        if (!sp || !*sp) return NK_EARG;
        return cmd[0] == 'm' ? ir_say(c, to, sp) : ir_notice(c, to, sp);
    }
    if (!strcmp(cmd, "me"))
        return buffer ? ir_action(c, buffer, arg) : NK_EARG;
    if (!strcmp(cmd, "nick"))
        return ir_set_nick(c, arg);
    if (!strcmp(cmd, "quit"))
        return ir_quit(c, arg);
    if (!strcmp(cmd, "topic")) {
        if (!buffer || !ir_is_channel(&c->sup, buffer)) return NK_EARG;
        return *arg ? sendf(c, 1, "TOPIC %s :%s", buffer, arg) : sendf(c, 1, "TOPIC %s", buffer);
    }
    if (!strcmp(cmd, "raw") || !strcmp(cmd, "quote"))
        return ir_raw(c, arg);
    return NK_EARG;
}

/* ---- channel state access ----------------------------------------------------- */
size_t ir_channel_count(const IrClient *c) { return c->nch; }
const char *ir_channel_name(const IrClient *c, size_t i) { return i < c->nch ? c->ch[i].name : 0; }

const char *ir_topic(const IrClient *c, const char *channel)
{
    Chan *ch = find_chan(c, channel);
    return ch ? ch->topic : 0;
}

size_t ir_member_count(const IrClient *c, const char *channel)
{
    Chan *ch = find_chan(c, channel);
    return ch ? ch->n : 0;
}

const IrMember *ir_member(const IrClient *c, const char *channel, size_t i)
{
    Chan *ch = find_chan(c, channel);
    return ch && i < ch->n ? &ch->m[i] : 0;
}

const IrMember *ir_find_member(const IrClient *c, const char *channel, const char *nick)
{
    return find_mem(c, find_chan(c, channel), nick);
}
