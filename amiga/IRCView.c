/* IRCView: the GFOpenIRC chat window on one server, over plain TCP. It is
 * the window's check on the Amiga (the lab steps are in the README) until
 * the chat program joins it to TLS and the key store: it asks for no
 * password and sends none, so it is for a test server (netkit's fake IRC
 * server) or a network that takes guests.
 *
 *     IRCView HOST=192.168.1.10 PORT=6667 NICK=amiga CHANNELS=#amiga
 *
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/socket.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfopenlayout_ui.h"
#include "gfopenlayout_ui_amiga.h"
#include "gfopennetkit/netkit.h"
#include "gfopennetkit/netkit_amiga.h"
#include "gfopenirc/irc.h"
#include "gfopenirc/ui_window.h"

static const char version[] = "$VER: IRCView 0.1 (13.11.2026) AmiCradle Team";

struct Library *SocketBase;

typedef struct App {
    IwWin    *win;
    IrClient *c;
    IuState  *s;
    IuNicks   nicks;
    char      join[256];
    int       dirty, joined, gone;
} App;

static uint32_t now_ms(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (uint32_t)d.ds_Days * 86400000UL + (uint32_t)d.ds_Minute * 60000UL + (uint32_t)d.ds_Tick * 20UL;
}

static const char *clock_now(char out[6])
{
    struct DateStamp d;
    unsigned h, m;
    DateStamp(&d);
    h = (unsigned)(d.ds_Minute / 60) % 24;      /* by hand: GCC can't see they fit in "HH:MM" */
    m = (unsigned)(d.ds_Minute % 60);
    out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10); out[2] = ':';
    out[3] = (char)('0' + m / 10); out[4] = (char)('0' + m % 10); out[5] = 0;
    return out;
}

/* The nick list of the shown buffer, from the core's channel state. */
static void show_nicks(App *a)
{
    size_t i, n;
    const IrMember **m;
    const IrSupport *sup = ir_support(a->c);
    IuState *s = iw_win_shown(a->win, &i);
    const IuBuffer *b = s ? iu_buffer(s, i) : NULL;
    if (!b || b->kind != IU_CHANNEL) { iw_win_nicks(a->win, NULL, sup->casemap); return; }
    n = ir_member_count(a->c, b->name);
    if (!(m = (const IrMember **)malloc((n ? n : 1) * sizeof(*m)))) return;
    for (i = 0; i < n; i++) m[i] = ir_member(a->c, b->name, i);
    iu_nicks_set(&a->nicks, m, n, sup->prefix_chars, sup->casemap);
    free(m);
    iw_win_nicks(a->win, &a->nicks, sup->casemap);
}

static void on_irc(void *user, const IrEvent *e)
{
    App *a = (App *)user;
    char now[6];
    int members = 0;
    long at;
    size_t shown = 0;
    if (e->type == IR_EV_REGISTERED || e->type == IR_EV_CAPS) {
        const IrSupport *sup = ir_support(a->c);
        iu_set_nick(a->s, ir_nick(a->c));
        iu_set_chantypes(a->s, sup->chantypes, sup->casemap);
    }
    at = iu_event(a->s, e, clock_now(now), &members);
    if (e->type == IR_EV_REGISTERED && a->join[0] && !a->joined) {
        ir_join(a->c, a->join, NULL);
        a->joined = 1;
    }
    a->dirty = 1;
    if (members && at >= 0 && iw_win_shown(a->win, &shown) == a->s && (size_t)at == shown) show_nicks(a);
}

static void command(void *user, IuState *s, size_t buffer, const char *text)
{
    App *a = (App *)user;
    const IuBuffer *b = iu_buffer(s, buffer);
    char now[6];
    int r = ir_command(a->c, buffer ? b->name : NULL, text);
    if (r == NK_EARG) iu_note(s, buffer ? b->name : NULL, "That command isn't known here, or nothing to say it to.", clock_now(now));
    else if (r == NK_OK && buffer && !ir_has_cap(a->c, "echo-message")) {
        if (!strncmp(text, "/me ", 4)) iu_own(s, b->name, ir_nick(a->c), text + 4, 1, clock_now(now));
        else if (text[0] != '/' || text[1] == '/') iu_own(s, b->name, ir_nick(a->c), text + (text[0] == '/'), 0, clock_now(now));
    }
    a->dirty = 1;
}

static void shown(void *user, IuState *s, size_t buffer)
{
    (void)s; (void)buffer;
    show_nicks((App *)user);
}

static void close_buffer(void *user, IuState *s, size_t buffer)
{
    App *a = (App *)user;
    const IuBuffer *b = iu_buffer(s, buffer);
    if (!buffer) { ir_quit(a->c, "IRCView"); return; }
    if (b->kind == IU_CHANNEL && b->joined) ir_part(a->c, b->name, NULL);
    iu_close(s, buffer);
    a->dirty = 1;
}

static void connect_to(void *user, const IwConnect *c)
{
    App *a = (App *)user;
    (void)c;
    iw_win_status(a->win, "IRCView keeps to the server it was started with.");
}

/* Up, Down and Tab go through the window first (history, completion). */
static int window_key(App *a, struct IntuiMessage *m)
{
    unsigned qual = (m->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? OLUI_QUAL_SHIFT : 0;
    if (m->Class == IDCMP_RAWKEY) {
        unsigned code = m->Code & 0x7f;
        if (code != 0x4c && code != 0x4d && code != 0x42) return 0;
        if (m->Code & 0x80) return 1;              /* the key let go */
        iw_win_key(a->win, code == 0x4c ? OLUI_KEY_UP : code == 0x4d ? OLUI_KEY_DOWN : OLUI_KEY_TAB, qual);
        return 1;
    }
    if (m->Class == IDCMP_VANILLAKEY && m->Code == '\t') {
        iw_win_key(a->win, OLUI_KEY_TAB, qual);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const IwWinOps ops = { command, shown, close_buffer, connect_to };
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((STRPTR)"HOST/A,PORT/N,NICK/A,CHANNELS/K,THEME/K", args, NULL);
    struct Window *w = NULL;
    olui *ui = NULL;
    olui_amiga *glue = NULL;     /* not "amiga": the compiler defines that */
    NKTransport t;
    NKConn *conn;
    IrConfig cfg;
    IwConnect form;
    char err[128], now[6];
    App a;
    int done = 0, rc = RETURN_FAIL, port;
    (void)version;

    memset(&a, 0, sizeof(a));
    iu_nicks_init(&a.nicks);
    if (!rda) { PutStr((STRPTR)"IRCView: HOST=server [PORT=6667] NICK=nick [CHANNELS=#a,#b] [THEME=file]\n"); return RETURN_FAIL; }
    port = args[1] ? (int)*(LONG *)args[1] : 6667;
    if (args[3]) snprintf(a.join, sizeof(a.join), "%s", (const char *)args[3]);
    if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) { PutStr((STRPTR)"IRCView: no TCP/IP stack\n"); goto out; }
    w = OpenWindowTags(NULL, WA_Title, (ULONG)"GFOpenIRC", WA_Width, 640, WA_Height, 440,
                       WA_MinWidth, 400, WA_MinHeight, 260, WA_MaxWidth, ~0UL, WA_MaxHeight, ~0UL,
                       WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
                       WA_Activate, TRUE, WA_SimpleRefresh, TRUE, WA_ReportMouse, TRUE,
                       WA_IDCMP, olui_amiga_idcmp(), TAG_DONE);
    if (!w) { PutStr((STRPTR)"IRCView: no window\n"); goto out; }
    ui = olui_new();
    glue = ui ? olui_amiga_new(ui, w) : NULL;
    a.s = iu_new((const char *)args[0]);
    if (!glue || !a.s) { PutStr((STRPTR)"IRCView: out of memory\n"); goto out; }
    if (!olui_amiga_theme(glue, args[4] ? (const char *)args[4] : NULL))
        PutStr((STRPTR)"IRCView: no theme found, the built-in look\n");
    if (!(a.win = iw_win_new(ui, &ops, &a, err, sizeof(err)))) {
        Printf((STRPTR)"IRCView: %s\n", (ULONG)err);
        goto out;
    }
    memset(&form, 0, sizeof(form));
    snprintf(form.host, sizeof(form.host), "%s", (const char *)args[0]);
    snprintf(form.nick, sizeof(form.nick), "%s", (const char *)args[2]);
    snprintf(form.channels, sizeof(form.channels), "%s", a.join);
    form.port = port;
    iw_win_set_connect(a.win, &form);
    iw_win_add(a.win, a.s);
    if (!olui_amiga_layout(glue)) { PutStr((STRPTR)"IRCView: out of memory\n"); goto out; }

    snprintf(err, sizeof(err), "Connecting to %s port %d", (const char *)args[0], port);
    iu_note(a.s, NULL, err, clock_now(now));
    iw_win_refresh(a.win);
    olui_amiga_draw(glue);
    if (nk_amiga_tcp(SocketBase, (const char *)args[0], (uint16_t)port, &t) != NK_OK) {
        iu_note(a.s, NULL, "Can't connect.", clock_now(now));
        a.gone = 1;
    } else if (!(conn = nk_conn_new(NULL, t, now_ms()))) {
        PutStr((STRPTR)"IRCView: out of memory\n");
        goto out;
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.nick = (const char *)args[2];
        if (!(a.c = ir_new(conn, &cfg, on_irc, &a, now_ms()))) { PutStr((STRPTR)"IRCView: out of memory\n"); goto out; }
    }
    iw_win_refresh(a.win);

    while (!done) {
        struct IntuiMessage *m;
        ULONG sigs = (1UL << w->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C;
        int fd = a.c && !a.gone ? nk_conn_fd(ir_conn(a.c)) : -1;
        if (fd >= 0) {
            fd_set rd, wr;
            struct timeval tv;
            FD_ZERO(&rd); FD_ZERO(&wr);
            FD_SET(fd, &rd);
            if (nk_conn_outq(ir_conn(a.c)) || ir_queued(a.c)) FD_SET(fd, &wr);
            tv.tv_sec = 0; tv.tv_usec = 250000;    /* the throttle and PINGs want time too */
            WaitSelect(fd + 1, &rd, &wr, NULL, &tv, &sigs);
        } else sigs = Wait(sigs);
        if (sigs & SIGBREAKF_CTRL_C) break;
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
            int r = window_key(&a, m) ? OLUI_AMIGA_NONE : olui_amiga_event(glue, m);
            ReplyMsg(&m->ExecMessage);
            if (r == OLUI_AMIGA_CLOSE) done = 1;
        }
        if (a.c && !a.gone) {
            int r = ir_pump(a.c, now_ms());
            if (r != NK_OK) {
                iu_note(a.s, NULL, ir_quitting(a.c) ? "Disconnected." : "The connection was lost.", clock_now(now));
                iw_win_status(a.win, "Not connected");
                a.gone = 1; a.dirty = 1;
            }
        }
        if (a.dirty) { iw_win_refresh(a.win); a.dirty = 0; }
        olui_amiga_draw(glue);
    }
    if (a.c && !a.gone) {      /* say goodbye, briefly */
        uint32_t until = now_ms() + 2000;
        ir_quit(a.c, "IRCView");
        while (now_ms() < until && ir_pump(a.c, now_ms()) == NK_OK && nk_conn_outq(ir_conn(a.c))) Delay(5);
    }
    rc = RETURN_OK;
out:
    iw_win_free(a.win);
    olui_amiga_free(glue);
    olui_free(ui);
    if (w) CloseWindow(w);
    ir_free(a.c);
    iu_free(a.s);
    iu_nicks_free(&a.nicks);
    if (SocketBase) CloseLibrary(SocketBase);
    FreeArgs(rda);
    return rc;
}
