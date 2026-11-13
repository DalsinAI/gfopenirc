/* GFOpenIRC chat window on GFOpenLayout. MIT licence, (c) 2026 AmiCradle Team. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfopenirc/ui_window.h"

static const char markup[] =
    "<column id=main>"
    " <tabs id=tabs>"
    "  <tab title=Chat id=tchat>"
    "   <row class=panes>"
    "    <column class=left>"
    "     <vlist id=buffers columns='170'></vlist>"
    "     <button id=close>Close</button>"
    "    </column>"
    "    <column class=middle>"
    "     <label id=topic></label>"
    "     <textarea id=log readonly></textarea>"
    "     <row class=say><input id=input placeholder='Say something, or /join #channel'></row>"
    "    </column>"
    "    <column class=right>"
    "     <vlist id=nicks columns='140'></vlist>"
    "    </column>"
    "   </row>"
    "  </tab>"
    "  <tab title=Connect id=tconnect>"
    "   <grid columns=2>"
    "    <label for=chost>Server</label><input id=chost placeholder='irc.libera.chat'>"
    "    <label for=cport>Port</label><input id=cport type=number value=6697 min=1 max=65535>"
    "    <label for=cnick>Nick</label><input id=cnick>"
    "    <label for=caccount>Account</label><input id=caccount placeholder='for SASL; empty for none'>"
    "    <label for=cchannels>Channels</label><input id=cchannels placeholder='#amiga,#amigachrome'>"
    "   </grid>"
    "   <checkbox id=ctls checked>Use TLS</checkbox>"
    "   <label id=cnote>The password is kept in the key store, not here.</label>"
    "   <label id=cwhy></label>"
    "   <row class=send>"
    "    <button id=connect>Connect</button>"
    "   </row>"
    "  </tab>"
    " </tabs>"
    " <label id=status></label>"
    "</column>";

static const char css[] =
    "#main{flex:1}"
    "#tabs{flex:1}"
    "tab{flex:1}"
    ".panes{flex:1;gap:6px}"
    ".left{width:170px;gap:4px}"
    ".middle{flex:1;gap:4px}"
    ".right{width:140px}"
    "#buffers{flex:1}"
    "#nicks{flex:1}"
    "#topic{height:18px}"
    "#input{flex:1}"
    "#log{flex:1;font-family:monospace}"
    "#chost,#cnick,#caccount,#cchannels{flex:1}"
    ".send{justify-content:end;gap:4px}"
    "#status{height:18px}";

typedef struct Hist {
    IuState  *s;
    char      name[200];
    IuHistory h;
} Hist;

struct IwWin {
    olui        *ui;
    IwWinOps     ops;
    void        *user;
    olui_el     *tabs, *buffers, *close, *topic, *log, *input, *nicks, *status;
    olui_el     *chost, *cport, *cnick, *caccount, *cchannels, *ctls, *cwhy;
    IuState     *srv[IW_MAX_SERVERS];
    size_t       nsrv;
    IuState     *shown_s;          /* the buffer shown: its server ... */
    char         shown_name[200];  /* ... and name, "" for the server's (indexes move when buffers close) */
    const IuNicks *nl;
    int          casemap;
    /* what the log shows, to build it again only when it changed */
    const NKTextView *log_tv;
    uint32_t     log_count, log_seq;
    int32_t      log_end;          /* the scroll at the log's end when last shown */
    Hist        *hist;
    size_t       nhist, chist;
    /* Tab again on the line Tab made: the next nick */
    int          comp;
    char         comp_line[512], comp_out[512], comp_nick[64];
    size_t       comp_caret, comp_at;
    char         cell[256];
};

static void set_text(IwWin *w, olui_el *el, const char *s) { olui_set_text(w->ui, el, s ? s : ""); }

/* ---- the buffers, all servers in one list ---- */

static size_t rows(const IwWin *w)
{
    size_t k, n = 0;
    for (k = 0; k < w->nsrv; k++) n += iu_count(w->srv[k]);
    return n;
}

static int at_row(const IwWin *w, long row, IuState **s, size_t *i)
{
    size_t k;
    if (row < 0) return 0;
    for (k = 0; k < w->nsrv; k++) {
        size_t n = iu_count(w->srv[k]);
        if ((size_t)row < n) { *s = w->srv[k]; *i = (size_t)row; return 1; }
        row -= (long)n;
    }
    return 0;
}

static long row_of(const IwWin *w, const IuState *s, size_t i)
{
    size_t k;
    long base = 0;
    for (k = 0; k < w->nsrv; k++) {
        if (w->srv[k] == s) return i < iu_count(s) ? base + (long)i : -1;
        base += (long)iu_count(w->srv[k]);
    }
    return -1;
}

IuState *iw_win_shown(const IwWin *w, size_t *buffer)
{
    long i;
    if (!w->shown_s) return NULL;
    i = w->shown_name[0] ? iu_find(w->shown_s, w->shown_name) : 0;   /* "": the server's own */
    if (i < 0) return NULL;
    if (buffer) *buffer = (size_t)i;
    return w->shown_s;
}

static const char *buffer_cell(void *userdata, int row, int column, int *image)
{
    IwWin *w = (IwWin *)userdata;
    IuState *s;
    size_t i;
    (void)column; (void)image;
    if (!at_row(w, row, &s, &i)) return NULL;
    iu_label(s, i, w->cell, sizeof(w->cell));
    return w->cell;
}

static const char *nick_cell(void *userdata, int row, int column, int *image)
{
    IwWin *w = (IwWin *)userdata;
    (void)column; (void)image;
    if (!w->nl || row < 0 || (size_t)row >= w->nl->count) return NULL;
    iu_nicks_label(w->nl, (size_t)row, w->cell, sizeof(w->cell));
    return w->cell;
}

/* olui keeps a list's selection by row; only row (-1: none) stays selected */
static void select_only(IwWin *w, olui_el *list, long row)
{
    int i;
    for (i = 0; i < olui_vlist_rows(list); i++)
        if (i != row && olui_vlist_selected(list, i)) olui_vlist_select(w->ui, list, i, 0);
    if (row >= 0) {
        olui_vlist_select(w->ui, list, (int)row, 1);
        olui_vlist_set_current(w->ui, list, (int)row);
    }
}

/* ---- the log ---- */

static void show_log(IwWin *w, int force)
{
    size_t i = 0, n, bytes = 0, k;
    IuState *s = iw_win_shown(w, &i);
    const IuBuffer *b = s ? iu_buffer(s, i) : NULL;
    const NKTextView *tv = b ? b->log : NULL;
    uint32_t count = tv ? nk_tv_count(tv) : 0, seq = count ? nk_tv_line(tv, count - 1)->seq : 0;
    int at_end;
    char *text, *p;
    if (!force && tv == w->log_tv && count == w->log_count && seq == w->log_seq) return;
    at_end = force || tv != w->log_tv || olui_get_scroll(w->log) >= w->log_end;
    n = count;
    for (k = 0; k < n; k++) bytes += nk_tv_line(tv, (uint32_t)k)->len + 1;
    if (!(text = malloc(bytes + 1))) return;
    for (k = 0, p = text; k < n; k++) {
        const NKTvLine *l = nk_tv_line(tv, (uint32_t)k);
        memcpy(p, l->text, l->len);
        p += l->len;
        if (k + 1 < n) *p++ = '\n';
    }
    *p = 0;
    set_text(w, w->log, text);
    free(text);
    w->log_tv = tv; w->log_count = count; w->log_seq = seq;
    if (at_end) {
        olui_set_scroll(w->ui, w->log, INT32_MAX);
        w->log_end = olui_get_scroll(w->log);
    }
}

static void show_topic(IwWin *w)
{
    size_t i = 0;
    IuState *s = iw_win_shown(w, &i);
    const IuBuffer *b = s ? iu_buffer(s, i) : NULL;
    if (!b) { set_text(w, w->topic, ""); return; }
    set_text(w, w->topic, b->kind == IU_CHANNEL ? b->topic : b->name);
}

void iw_win_refresh(IwWin *w)
{
    size_t i = 0;
    IuState *s = iw_win_shown(w, &i);
    if (w->shown_s && !s) {              /* the shown buffer closed: its server's own */
        s = w->shown_s;
        iw_win_show(w, s, 0);
        return;
    }
    olui_vlist_set(w->ui, w->buffers, (int)rows(w), buffer_cell, w);
    select_only(w, w->buffers, s ? row_of(w, s, i) : -1);
    show_topic(w);
    show_log(w, 0);
}

void iw_win_show(IwWin *w, IuState *s, size_t buffer)
{
    const IuBuffer *b = s ? iu_buffer(s, buffer) : NULL;
    size_t k;
    if (!b) return;
    for (k = 0; k < w->nsrv; k++)
        if (w->srv[k] != s) iu_show(w->srv[k], IU_NONE);
    iu_show(s, buffer);
    w->shown_s = s;
    snprintf(w->shown_name, sizeof(w->shown_name), "%s", buffer ? b->name : "");
    w->nl = NULL;
    olui_vlist_set(w->ui, w->nicks, 0, nick_cell, w);
    w->comp = 0;
    olui_vlist_set(w->ui, w->buffers, (int)rows(w), buffer_cell, w);
    select_only(w, w->buffers, row_of(w, s, buffer));
    show_topic(w);
    show_log(w, 1);
    if (w->ops.shown) w->ops.shown(w->user, s, buffer);
}

int iw_win_add(IwWin *w, IuState *s)
{
    if (!s || w->nsrv >= IW_MAX_SERVERS) return 0;
    w->srv[w->nsrv++] = s;
    if (!w->shown_s) iw_win_show(w, s, 0);
    else iw_win_refresh(w);
    return 1;
}

void iw_win_remove(IwWin *w, IuState *s)
{
    size_t k, j;
    for (k = 0; k < w->nsrv && w->srv[k] != s; k++) ;
    if (k == w->nsrv) return;
    memmove(&w->srv[k], &w->srv[k + 1], (w->nsrv - k - 1) * sizeof(w->srv[0]));
    w->nsrv--;
    for (j = 0; j < w->nhist; ) {         /* its histories go too */
        if (w->hist[j].s == s) {
            iu_hist_free(&w->hist[j].h);
            w->hist[j] = w->hist[--w->nhist];
        } else j++;
    }
    if (w->shown_s == s) {
        w->shown_s = NULL; w->shown_name[0] = 0; w->log_tv = NULL;
        w->nl = NULL;
        olui_vlist_set(w->ui, w->nicks, 0, nick_cell, w);
        if (w->nsrv) { iw_win_show(w, w->srv[0], 0); return; }
        set_text(w, w->log, ""); set_text(w, w->topic, "");
    }
    iw_win_refresh(w);
}

void iw_win_nicks(IwWin *w, const IuNicks *nicks, int casemap)
{
    w->nl = nicks;
    w->casemap = casemap;
    w->comp = 0;
    olui_vlist_set(w->ui, w->nicks, nicks ? (int)nicks->count : 0, nick_cell, w);
    select_only(w, w->nicks, -1);
}

void iw_win_status(IwWin *w, const char *text) { set_text(w, w->status, text); }

void iw_win_set_connect(IwWin *w, const IwConnect *c)
{
    char port[16];
    set_text(w, w->chost, c->host);
    snprintf(port, sizeof(port), "%d", c->port > 0 ? c->port : c->tls ? 6697 : 6667);
    set_text(w, w->cport, port);
    olui_set_number(w->ui, w->cport, c->port > 0 ? c->port : c->tls ? 6697 : 6667);
    set_text(w, w->cnick, c->nick);
    set_text(w, w->caccount, c->account);
    set_text(w, w->cchannels, c->channels);
    olui_set_checked(w->ui, w->ctls, c->tls);
}

/* ---- the input line: history and Tab ---- */

static IuHistory *history(IwWin *w)
{
    size_t i;
    IuState *s = iw_win_shown(w, NULL);
    if (!s) return NULL;
    for (i = 0; i < w->nhist; i++)
        if (w->hist[i].s == s && !strcmp(w->hist[i].name, w->shown_name)) return &w->hist[i].h;
    if (w->nhist == w->chist) {
        size_t c = w->chist ? w->chist * 2 : 8;
        Hist *h = realloc(w->hist, c * sizeof(*h));
        if (!h) return NULL;
        w->hist = h; w->chist = c;
    }
    w->hist[w->nhist].s = s;
    snprintf(w->hist[w->nhist].name, sizeof(w->hist[0].name), "%s", w->shown_name);
    iu_hist_init(&w->hist[w->nhist].h);
    return &w->hist[w->nhist++].h;
}

static void complete(IwWin *w)
{
    const char *cur = olui_get_text(w->input);
    size_t from = 0, to = 0, ws, nl;
    const char *line, *after = NULL;
    size_t caret;
    char out[512];
    long at;
    olui_get_selection(w->input, &from, &to);
    if (w->comp && !strcmp(cur, w->comp_out) && to == w->comp_at) {
        line = w->comp_line; caret = w->comp_caret; after = w->comp_nick;
    } else {
        snprintf(w->comp_line, sizeof(w->comp_line), "%s", cur);
        line = w->comp_line; caret = to < strlen(line) ? to : strlen(line);
        w->comp_caret = caret;
    }
    if (!w->nl || (at = iu_complete(w->nl, line, caret, after, w->casemap, out, sizeof(out))) < 0) return;
    for (ws = caret; ws > 0 && line[ws - 1] != ' '; ws--) ;
    nl = (size_t)at - ws - (ws == 0 ? 2 : 1);
    if (nl >= sizeof(w->comp_nick)) nl = sizeof(w->comp_nick) - 1;
    memcpy(w->comp_nick, out + ws, nl);
    w->comp_nick[nl] = 0;
    snprintf(w->comp_out, sizeof(w->comp_out), "%s", out);
    w->comp_at = (size_t)at;
    w->comp = 1;
    set_text(w, w->input, out);
    olui_set_selection(w->ui, w->input, (size_t)at, (size_t)at);
}

int iw_win_key(IwWin *w, unsigned code, unsigned qualifiers)
{
    if (olui_focused(w->ui) == w->input && olui_tabs_current(w->tabs) == IW_TAB_CHAT
        && !(qualifiers & OLUI_QUAL_COMMAND)) {
        if (code == OLUI_KEY_TAB && !(qualifiers & OLUI_QUAL_SHIFT)) { complete(w); return 1; }
        if (code == OLUI_KEY_UP || code == OLUI_KEY_DOWN) {
            IuHistory *h = history(w);
            const char *l = h ? (code == OLUI_KEY_UP ? iu_hist_up(h) : iu_hist_down(h)) : NULL;
            w->comp = 0;
            if (l) set_text(w, w->input, l);
            return 1;
        }
    }
    w->comp = 0;
    return olui_key(w->ui, code, qualifiers);
}

/* ---- events ---- */

static void submit(IwWin *w)
{
    size_t i = 0;
    IuState *s = iw_win_shown(w, &i);
    char *text;
    const char *t = olui_get_text(w->input);
    size_t n = strlen(t);
    IuHistory *h;
    if (!s || !n) return;
    if (!(text = malloc(n + 1))) return;
    memcpy(text, t, n + 1);
    if ((h = history(w)) != NULL) iu_hist_add(h, text);
    set_text(w, w->input, "");
    w->comp = 0;
    if (w->ops.command) w->ops.command(w->user, s, i, text);
    free(text);
}

static void connect_pressed(IwWin *w)
{
    IwConnect c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "%s", olui_get_text(w->chost));
    snprintf(c.nick, sizeof(c.nick), "%s", olui_get_text(w->cnick));
    snprintf(c.account, sizeof(c.account), "%s", olui_get_text(w->caccount));
    snprintf(c.channels, sizeof(c.channels), "%s", olui_get_text(w->cchannels));
    c.tls = olui_get_checked(w->ctls);
    c.port = atoi(olui_get_text(w->cport));
    if (c.port <= 0 || c.port > 65535) c.port = c.tls ? 6697 : 6667;
    if (!c.host[0] || strchr(c.host, ' ')) { set_text(w, w->cwhy, "Give the server's name."); return; }
    if (!c.nick[0] || strchr(c.nick, ' ')) { set_text(w, w->cwhy, "Give a nick (no spaces)."); return; }
    set_text(w, w->cwhy, "");
    if (w->ops.connect) w->ops.connect(w->user, &c);
    olui_tabs_select(w->ui, w->tabs, IW_TAB_CHAT);
    olui_focus(w->ui, w->input);
}

static void on_event(olui *ui, const olui_event *ev, void *userdata)
{
    IwWin *w = (IwWin *)userdata;
    const char *id = ev->id ? ev->id : "";
    (void)ui;
    if (ev->type == OLUI_SELECT && !strcmp(id, "buffers")) {
        IuState *s; size_t i;
        if (at_row(w, ev->index, &s, &i)) iw_win_show(w, s, i);
        olui_focus(w->ui, w->input);
    } else if (ev->type == OLUI_ACTIVATE && !strcmp(id, "nicks")) {
        /* a nick double-clicked: a query with it */
        IuState *s = iw_win_shown(w, NULL);
        long q;
        if (s && w->nl && ev->index >= 0 && (size_t)ev->index < w->nl->count
            && (q = iu_open(s, w->nl->n[ev->index].nick)) >= 0) {
            iw_win_show(w, s, (size_t)q);
            olui_focus(w->ui, w->input);
        }
    } else if (ev->type == OLUI_SUBMIT && !strcmp(id, "input")) {
        submit(w);
    } else if (ev->type == OLUI_CLICK) {
        if (!strcmp(id, "close")) {
            size_t i = 0;
            IuState *s = iw_win_shown(w, &i);
            if (s && w->ops.close) w->ops.close(w->user, s, i);
        } else if (!strcmp(id, "connect")) connect_pressed(w);
    }
}

IwWin *iw_win_new(olui *ui, const IwWinOps *ops, void *user, char *err, size_t cap)
{
    IwWin *w = calloc(1, sizeof(*w));
    if (!w) { snprintf(err, cap, "out of memory"); return NULL; }
    w->ui = ui; w->user = user;
    if (ops) w->ops = *ops;
    if (!olui_load(ui, markup, css, err, cap) || !olui_on(ui, NULL, 0, on_event, w)) { free(w); return NULL; }
    w->tabs = olui_find(ui, "tabs");       w->buffers = olui_find(ui, "buffers");
    w->close = olui_find(ui, "close");     w->topic = olui_find(ui, "topic");
    w->log = olui_find(ui, "log");         w->input = olui_find(ui, "input");
    w->nicks = olui_find(ui, "nicks");     w->status = olui_find(ui, "status");
    w->chost = olui_find(ui, "chost");     w->cport = olui_find(ui, "cport");
    w->cnick = olui_find(ui, "cnick");     w->caccount = olui_find(ui, "caccount");
    w->cchannels = olui_find(ui, "cchannels"); w->ctls = olui_find(ui, "ctls");
    w->cwhy = olui_find(ui, "cwhy");
    olui_vlist_set(ui, w->buffers, 0, buffer_cell, w);
    olui_vlist_set(ui, w->nicks, 0, nick_cell, w);
    return w;
}

void iw_win_free(IwWin *w)
{
    size_t i;
    if (!w) return;
    for (i = 0; i < w->nhist; i++) iu_hist_free(&w->hist[i].h);
    free(w->hist);
    free(w);
}
