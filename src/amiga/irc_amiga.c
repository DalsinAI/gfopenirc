/* GFOpenIRC command-line client for AmigaOS 3.x: the same protocol core as
 * the host build (parse.c, client.c), over bsdsocket.library and
 * gfopentls.library, with the SASL password from gfopenkeys.library.
 *
 *   GFOpenIRC SERVER/A,PORT/N,NICK/A,USER/K,REALNAME/K,SASLUSER/K,JOIN/K/M,
 *             PROXY/K,NOTLS/S,INSECURE/S,ALLOWPLAIN/S,SASLREQUIRED/S
 *
 * TLS on port 6697 by default, with the certificate checked against the
 * system trust store. NOTLS connects in plain text (port 6667); a password
 * is then refused unless ALLOWPLAIN is given. INSECURE skips the
 * certificate check (encryption without knowing who is at the other end).
 * The SASL password is the gfopenkeys entry irc-sasl/<server>/<sasluser>; with no
 * gfopenkeys.library, or no entry, the client goes on without SASL (or
 * stops, with SASLREQUIRED).
 *
 * Lines typed in the Shell are commands ("/join #amiga", "/msg nick hi",
 * "/quit") or text for the current channel. Ctrl-C quits.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <libraries/gfopentls.h>
#include <proto/gfopentls.h>
#ifdef HAVE_GFOPENKEYS
#include <libraries/gfopenkeys.h>
#include <proto/gfopenkeys.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfopennetkit/netkit_amiga.h"
#include "gfopenirc/irc.h"

static const char VERSTAG[] = "$VER: GFOpenIRC 0.1 (13.11.2026)";

struct Library *SocketBase;
struct Library *OpenTLSBase;
#ifdef HAVE_GFOPENKEYS
struct Library *GFOpenKeysBase;
#endif

#define TEMPLATE "SERVER/A,PORT/N,NICK/A,USER/K,REALNAME/K,SASLUSER/K,JOIN/K/M," \
                 "PROXY/K,NOTLS/S,INSECURE/S,ALLOWPLAIN/S,SASLREQUIRED/S"
enum { A_SERVER, A_PORT, A_NICK, A_USER, A_REAL, A_SASLUSER, A_JOIN, A_PROXY,
       A_NOTLS, A_INSECURE, A_ALLOWPLAIN, A_SASLREQ, A_COUNT };

static const char *const EV_NAMES[] = {
    "", "registered", "message", "notice", "action", "ctcp", "join", "part",
    "quit", "nick", "kick", "topic", "names", "mode", "away", "account",
    "invite", "server", "error", "sasl", "caps"
};

typedef struct {
    IrClient *c;
    char      current[200];
} App;

static uint32_t now_ms(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (uint32_t)ds.ds_Days * 86400000u + (uint32_t)ds.ds_Minute * 60000u +
           (uint32_t)ds.ds_Tick * 20u;
}

static void on_event(void *u, const IrEvent *e)
{
    App *a = (App *)u;
    switch (e->type) {
    case IR_EV_MESSAGE:
        printf("[%s] <%s> %s\n", e->target ? e->target : "*", e->nick ? e->nick : "", e->text);
        break;
    case IR_EV_ACTION:
        printf("[%s] * %s %s\n", e->target ? e->target : "*", e->nick ? e->nick : "", e->text);
        break;
    default:
        printf("[%s] %s %s %s%s%s\n", e->target ? e->target : "*", EV_NAMES[e->type],
               e->nick ? e->nick : "", e->text ? e->text : "",
               e->extra ? " " : "", e->extra ? e->extra : "");
        break;
    }
    if (e->type == IR_EV_JOIN && e->self && e->target)
        snprintf(a->current, sizeof(a->current), "%s", e->target);
    fflush(stdout);
}

/* waits until the socket is readable or writable, or `ms` pass; answers
 * the signals that arrived (Ctrl-C) */
static ULONG wait_socket(int fd, int want_write, long ms)
{
    fd_set r, w;
    struct timeval tv;
    ULONG sigs = SIGBREAKF_CTRL_C;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(fd, &r);
    if (want_write)
        FD_SET(fd, &w);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    WaitSelect(fd + 1, &r, &w, 0, &tv, &sigs);
    return sigs;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* the password for irc-sasl/<host>/<user>: 1 found, 0 not */
static int find_password(const char *host, const char *user, char *buf, ULONG cap)
{
#ifdef HAVE_GFOPENKEYS
    ULONG len = cap - 1;
    char h[256];
    size_t i;
    LONG r;
    if (!GFOpenKeysBase)
        return 0;
    for (i = 0; host[i] && i < sizeof(h) - 1; i++)
        h[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 32 : host[i]);
    h[i] = 0;
    if ((r = GK_Find((CONST_STRPTR)GKS_IRC_SASL, (CONST_STRPTR)h, (CONST_STRPTR)user,
                     (UBYTE *)buf, &len, 0)) != GKE_OK) {
        if (r != GKE_NOTFOUND)
            printf("GFOpenIRC: GFOpenKeys: %s\n", (const char *)GK_ErrorString(r));
        wipe(buf, cap);
        return 0;
    }
    buf[len < cap ? len : cap - 1] = 0;
    return 1;
#else
    (void)host; (void)user; (void)buf; (void)cap;
    return 0;
#endif
}


int main(void)
{
    LONG args[A_COUNT];
    struct RDArgs *rd;
    struct OTContext *ctx = 0;
    NKTransport tcp, t;
    NKConn *conn = 0;
    IrConfig cfg;
    App app;
    char pw[512], in[512], host[256];
    int r, rc = RETURN_FAIL, joined = 0, use_tls, fd;
    uint16_t port;
    STRPTR *joins;

    (void)VERSTAG;
    memset(args, 0, sizeof(args));
    memset(&cfg, 0, sizeof(cfg));
    memset(&app, 0, sizeof(app));
    pw[0] = 0;
    rd = ReadArgs((CONST_STRPTR)TEMPLATE, args, 0);
    if (!rd) {
        PrintFault(IoErr(), (CONST_STRPTR)"GFOpenIRC");
        return RETURN_FAIL;
    }
    use_tls = !args[A_NOTLS];
    port = args[A_PORT] ? (uint16_t)*(LONG *)args[A_PORT] : (uint16_t)(use_tls ? 6697 : 6667);
    snprintf(host, sizeof(host), "%s", (char *)args[A_SERVER]);
    joins = (STRPTR *)args[A_JOIN];

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        printf("GFOpenIRC: no TCP/IP stack (bsdsocket.library)\n");
        goto out;
    }
    if (use_tls && !(OpenTLSBase = OpenLibrary((CONST_STRPTR)OPENTLSLIB_NAME, OPENTLSLIB_VERSION))) {
        printf("GFOpenIRC: needs %s (or NOTLS)\n", OPENTLSLIB_NAME);
        goto out;
    }
#ifdef HAVE_GFOPENKEYS
    GFOpenKeysBase = OpenLibrary((CONST_STRPTR)GFOPENKEYS_NAME, 1);
#endif

    cfg.nick = (const char *)args[A_NICK];
    cfg.user = (const char *)args[A_USER];
    cfg.realname = (const char *)args[A_REAL];
    cfg.tls = use_tls;
    cfg.allow_plain_auth = args[A_ALLOWPLAIN] != 0;
    cfg.sasl_required = args[A_SASLREQ] != 0;
    if (args[A_SASLUSER]) {
        if (find_password(host, (const char *)args[A_SASLUSER], pw, sizeof(pw))) {
            cfg.sasl_user = (const char *)args[A_SASLUSER];
            cfg.sasl_pass = pw;
        } else if (cfg.sasl_required) {
            printf("GFOpenIRC: no password for irc-sasl/%s/%s in GFOpenKeys\n", host, (char *)args[A_SASLUSER]);
            goto out;
        } else
            printf("GFOpenIRC: no password for irc-sasl/%s/%s in GFOpenKeys: going on without SASL\n",
                   host, (char *)args[A_SASLUSER]);
    }
    if ((r = ir_config_check(&cfg)) != NK_OK) {
        printf(r == NK_EREFUSED
               ? "GFOpenIRC: not sending a password without TLS (ALLOWPLAIN to do it anyway)\n"
               : "GFOpenIRC: bad NICK, USER or REALNAME\n");
        goto out;
    }

    /* TCP, through the SOCKS5 proxy when asked */
    if (args[A_PROXY]) {
        char ph[256];
        const char *p = (const char *)args[A_PROXY], *colon = strrchr(p, ':');
        NKSocks5 s;
        size_t hl = colon ? (size_t)(colon - p) : strlen(p);
        if (hl >= sizeof(ph)) hl = sizeof(ph) - 1;
        memcpy(ph, p, hl);
        ph[hl] = 0;
        if (nk_amiga_tcp(SocketBase, ph, (uint16_t)(colon ? atoi(colon + 1) : 1080), &tcp) != NK_OK) {
            printf("GFOpenIRC: cannot reach proxy %s\n", p);
            goto out;
        }
        nk_socks5_init(&s, host, port, 0, 0);
        while ((r = nk_socks5_step(&s, &tcp)) == NK_WOULDBLOCK)
            if (wait_socket(tcp.fd(tcp.ctx), 0, 1000) & SIGBREAKF_CTRL_C) break;
        if (r != 1) {
            printf("GFOpenIRC: proxy: %s\n", nk_strerror(r));
            nk_transport_close(&tcp);
            goto out;
        }
    } else if (nk_amiga_tcp(SocketBase, host, port, &tcp) != NK_OK) {
        printf("GFOpenIRC: cannot reach %s:%u\n", host, (unsigned)port);
        goto out;
    }
    fd = tcp.fd(tcp.ctx);
    if (use_tls) {
        struct OTContextConfig cc;
        LONG err = 0;
        const char *why = 0;
        memset(&cc, 0, sizeof(cc));
        cc.Size = sizeof(cc);
        ctx = OT_NewContext(&cc, &err);
        if (!ctx || nk_amiga_tls(OpenTLSBase, ctx, host, args[A_INSECURE] ? OTV_NONE : OTV_FULL,
                                 0, &tcp, &t) != NK_OK) {
            printf("GFOpenIRC: TLS: %s\n", ctx ? "cannot start" : (char *)OT_ErrorString(err));
            nk_transport_close(&tcp);
            goto out;
        }
        while ((r = nk_tls_handshake(&t)) == NK_WOULDBLOCK)
            if (wait_socket(fd, 0, 100) & SIGBREAKF_CTRL_C) break;
        if (r != NK_OK) {
            nk_tls_error(&t, &why);
            printf("GFOpenIRC: TLS: %s\n", why ? why : nk_strerror(r));
            nk_transport_close(&t);
            goto out;
        }
    } else
        t = tcp;

    conn = nk_conn_new(0, t, now_ms());
    app.c = conn ? ir_new(conn, &cfg, on_event, &app, now_ms()) : 0;
    wipe(pw, sizeof(pw));
    if (!app.c) {
        printf("GFOpenIRC: out of memory\n");
        goto out;
    }
    conn = 0;                          /* the client owns it now */
    for (;;) {
        ULONG sigs = wait_socket(fd, nk_conn_outq(ir_conn(app.c)) != 0, ir_queued(app.c) ? 100 : 250);
        if (sigs & SIGBREAKF_CTRL_C) {
            if (ir_quitting(app.c))
                break;
            ir_quit(app.c, 0);
        }
        if ((r = ir_pump(app.c, now_ms())) != NK_OK) {
            if (r == NK_EOF && nk_tls_truncated(&t))
                printf("GFOpenIRC: the connection was cut (no TLS close)\n");
            rc = ir_quitting(app.c) && r == NK_EOF ? RETURN_OK : RETURN_ERROR;
            if (rc != RETURN_OK)
                printf("GFOpenIRC: connection ended: %s\n", nk_strerror(r));
            break;
        }
        if (ir_registered(app.c) && !joined) {
            joined = 1;
            while (joins && *joins)
                ir_join(app.c, (const char *)*joins++, 0);
        }
        /* a line typed in the Shell */
        if (ir_registered(app.c) && WaitForChar(Input(), 0)) {
            LONG n = Read(Input(), in, sizeof(in) - 1);
            if (n <= 0) {
                ir_quit(app.c, 0);
                continue;
            }
            in[n] = 0;
            while (n > 0 && (in[n - 1] == '\n' || in[n - 1] == '\r'))
                in[--n] = 0;
            ir_input_target(in, app.current, sizeof(app.current));
            if (in[0] && ir_command(app.c, app.current[0] ? app.current : 0, in) != NK_OK)
                printf("GFOpenIRC: cannot do: %s\n", in);
        }
    }
out:
    if (app.c) ir_free(app.c);
    if (conn) nk_conn_free(conn);
    if (ctx) OT_FreeContext(ctx);
    wipe(pw, sizeof(pw));
#ifdef HAVE_GFOPENKEYS
    if (GFOpenKeysBase) CloseLibrary(GFOpenKeysBase);
#endif
    if (OpenTLSBase) CloseLibrary(OpenTLSBase);
    if (SocketBase) CloseLibrary(SocketBase);
    FreeArgs(rd);
    return rc;
}
