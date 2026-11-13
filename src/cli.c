/* GFOpenIRC headless client (design step 2): connects, registers (CAP,
 * SASL PLAIN), joins, then reads commands from standard input and prints
 * every event as one line.
 *
 *   gfopenirc-cli [options]
 * options: --server HOST --port N --nick N --user U --realname R
 *          --sasl-user U --pass-env VAR (SASL password from the environment)
 *          --sasl-required --join CHANNEL (repeatable) --socks HOST:PORT
 *          --burst N --interval MS --allow-plain-auth (a password over a
 *          connection without TLS: refused otherwise)
 * Input: "/join #x", "/msg nick text", "/me waves", "/quit bye", ... or
 * plain text to the current channel or query (the last one joined or
 * queried). Output: "<target>\t<event>\t<nick>\t<text>".
 * The password comes from an environment variable for now; with
 * gfopenkeys.library it comes from the keystore (entry irc/HOST/USER).
 *
 * This is the host build (POSIX sockets); the Amiga build uses bsdsocket,
 * gfopentls and ReadArgs. MIT licence, (c) 2026 AmiCradle Team. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gfopenirc/irc.h"

static const char *const EV_NAMES[] = {
    "", "registered", "message", "notice", "action", "ctcp", "join", "part",
    "quit", "nick", "kick", "topic", "names", "mode", "away", "account",
    "invite", "server", "error", "sasl", "caps"
};

typedef struct {
    IrClient *c;
    char      current[200];
    int       sasl_failed;
} App;

static void on_event(void *u, const IrEvent *e)
{
    App *a = (App *)u;
    printf("%s\t%s\t%s\t%s%s%s%s\n", e->target ? e->target : "*", EV_NAMES[e->type],
           e->nick ? e->nick : "", e->text ? e->text : "",
           e->extra ? " [" : "", e->extra ? e->extra : "", e->extra ? "]" : "");
    if (e->type == IR_EV_SASL && e->extra && !strcmp(e->extra, "failed"))
        a->sasl_failed = 1;
    if (e->type == IR_EV_JOIN && e->self && e->target)
        snprintf(a->current, sizeof(a->current), "%s", e->target);
    if (e->type == IR_EV_NAMES && a->c) {
        size_t i, n = ir_member_count(a->c, e->target);
        printf("%s\tmembers\t\t", e->target);
        for (i = 0; i < n; i++) {
            const IrMember *m = ir_member(a->c, e->target, i);
            printf("%s%s%s", i ? " " : "", m->prefixes, m->nick);
        }
        putchar('\n');
    }
    fflush(stdout);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void usage(void)
{
    fputs("usage: gfopenirc-cli [--server H] [--port N] --nick N [--user U] [--realname R]\n"
          "       [--sasl-user U --pass-env VAR] [--sasl-required] [--join #C]...\n"
          "       [--socks H:P] [--burst N] [--interval MS] [--allow-plain-auth]\n", stderr);
}

static int connect_to(const char *server, int port, const char *socks, NKTransport *t)
{
    if (socks) {
        char host[256];
        const char *colon = strrchr(socks, ':');
        NKSocks5 s;
        int k;
        if (!colon || (size_t)(colon - socks) >= sizeof(host)) return NK_EARG;
        memcpy(host, socks, (size_t)(colon - socks));
        host[colon - socks] = 0;
        if (nk_posix_tcp(host, (uint16_t)atoi(colon + 1), t) != NK_OK)
            return NK_EIO;
        if (nk_socks5_init(&s, server, (uint16_t)port, 0, 0) != NK_OK) {
            nk_transport_close(t);
            return NK_EARG;
        }
        while ((k = nk_socks5_step(&s, t)) == NK_WOULDBLOCK) {
            struct pollfd p;
            p.fd = t->fd(t->ctx);
            p.events = POLLIN;
            poll(&p, 1, 1000);
        }
        if (k != 1) {
            nk_transport_close(t);
            return k;
        }
        return NK_OK;
    }
    return nk_posix_tcp(server, (uint16_t)port, t);
}

int main(int argc, char **argv)
{
    const char *server = "localhost", *passenv = 0, *socks = 0, *joins[16];
    int port = 6667, i, njoin = 0, r, stdin_open = 1, joined = 0;
    uint32_t reg_at = 0;
    int reg_seen = 0;
    IrConfig cfg;
    NKTransport t;
    NKConn *conn;
    App app;
    char in[1024];
    size_t inl = 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(&app, 0, sizeof(app));
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sasl-required")) { cfg.sasl_required = 1; continue; }
        if (!strcmp(argv[i], "--allow-plain-auth")) { cfg.allow_plain_auth = 1; continue; }
        if (i + 1 >= argc) { usage(); return 2; }
        if (!strcmp(argv[i], "--server")) server = argv[++i];
        else if (!strcmp(argv[i], "--port")) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nick")) cfg.nick = argv[++i];
        else if (!strcmp(argv[i], "--user")) cfg.user = argv[++i];
        else if (!strcmp(argv[i], "--realname")) cfg.realname = argv[++i];
        else if (!strcmp(argv[i], "--sasl-user")) cfg.sasl_user = argv[++i];
        else if (!strcmp(argv[i], "--pass-env")) passenv = argv[++i];
        else if (!strcmp(argv[i], "--socks")) socks = argv[++i];
        else if (!strcmp(argv[i], "--burst")) cfg.burst = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval")) cfg.interval_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && njoin < 16) joins[njoin++] = argv[++i];
        else { usage(); return 2; }
    }
    if (!cfg.nick) { usage(); return 2; }
    if (cfg.sasl_user) {
        cfg.sasl_pass = passenv ? getenv(passenv) : 0;
        if (!cfg.sasl_pass) { fprintf(stderr, "SASL needs a password (--pass-env)\n"); return 2; }
    }
    /* this host build has no TLS: a password needs --allow-plain-auth */
    if ((r = ir_config_check(&cfg)) != NK_OK) {
        fprintf(stderr, r == NK_EREFUSED
                ? "not sending a password over a connection without TLS (--allow-plain-auth to do it anyway)\n"
                : "bad nick, user or real name\n");
        return 2;
    }
    if ((r = connect_to(server, port, socks, &t)) != NK_OK) {
        fprintf(stderr, "cannot reach %s:%d: %s\n", server, port, nk_strerror(r));
        return 1;
    }
    conn = nk_conn_new(0, t, now_ms());
    app.c = ir_new(conn, &cfg, on_event, &app, now_ms());
    if (!app.c) {
        fprintf(stderr, "bad nick, user or real name\n");
        nk_conn_free(conn);
        return 2;
    }
    for (;;) {
        struct pollfd p[2];
        int np = 1;
        p[0].fd = nk_conn_fd(ir_conn(app.c));
        p[0].events = POLLIN | (nk_conn_outq(ir_conn(app.c)) ? POLLOUT : 0);
        /* commands are read once registered and in the channels asked for
         * (or after 10 s, when a join went wrong) */
        if (ir_registered(app.c) && !reg_seen) {
            reg_seen = 1;
            reg_at = now_ms();
        }
        if (stdin_open && ir_registered(app.c) && !ir_quitting(app.c) &&
            ((int)ir_channel_count(app.c) >= njoin || now_ms() - reg_at > 10000)) {
            p[1].fd = 0;
            p[1].events = POLLIN;
            np = 2;
        }
        poll(p, (nfds_t)np, ir_queued(app.c) ? 100 : 1000);
        if ((r = ir_pump(app.c, now_ms())) != NK_OK)
            break;
        if (ir_registered(app.c) && !joined) {
            joined = 1;
            for (i = 0; i < njoin; i++)
                ir_join(app.c, joins[i], 0);
        }
        if (np == 2 && (p[1].revents & (POLLIN | POLLHUP))) {
            ssize_t k = read(0, in + inl, sizeof(in) - 1 - inl);
            char *nl;
            if (k <= 0) {
                stdin_open = 0;
                ir_quit(app.c, 0);
                continue;
            }
            inl += (size_t)k;
            in[inl] = 0;
            while ((nl = strchr(in, '\n')) != 0) {
                *nl = 0;
                if (nl > in && nl[-1] == '\r') nl[-1] = 0;
                if (in[0]) {
                    ir_input_target(in, app.current, sizeof(app.current));
                    if (ir_command(app.c, app.current[0] ? app.current : 0, in) != NK_OK)
                        printf("*\tinput\t\tcannot do: %s\n", in);
                }
                inl -= (size_t)(nl + 1 - in);
                memmove(in, nl + 1, inl + 1);
            }
            if (inl == sizeof(in) - 1) inl = 0;      /* an over-long line: drop it */
        }
    }
    {
        int quitting = ir_quitting(app.c), failed = app.sasl_failed;
        ir_free(app.c);
        if (failed && cfg.sasl_required) return 4;
        if (quitting && r == NK_EOF) return 0;
        fprintf(stderr, "connection ended: %s\n", nk_strerror(r));
        return 1;
    }
}
