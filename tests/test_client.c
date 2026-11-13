/* The client against a scripted server: CAP LS 302 over two lines, SASL
 * PLAIN (good, failed, required), nick in use, ISUPPORT, JOIN and NAMES
 * with multi-prefix, MODE, NICK, PART, KICK, QUIT, PRIVMSG to a channel
 * and a query, CTCP, echo-message, server-time, throttling, keepalive.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <stdio.h>
#include <stdlib.h>
#include "gfopenirc/irc.h"
#include "mock_transport.h"

typedef struct {
    int  n;
    int  type[64];
    char line[64][300];
} Log;

static void on_ev(void *u, const IrEvent *e)
{
    Log *l = (Log *)u;
    if (l->n >= 64) return;
    l->type[l->n] = e->type;
    snprintf(l->line[l->n], sizeof(l->line[0]), "%s|%s|%s|%s|%s|%d",
             e->target ? e->target : "-", e->nick ? e->nick : "-", e->text ? e->text : "-",
             e->extra ? e->extra : "-", e->time ? e->time : "-", e->self);
    l->n++;
}

static int has_ev(const Log *l, int type, const char *line)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->type[i] == type && (!line || !strcmp(l->line[i], line)))
            return 1;
    return 0;
}

/* was this exact line sent? */
static int sent(const Mock *m, const char *line)
{
    size_t n = strlen(line), i = 0;
    while (i < m->out_len) {
        const char *e = memchr(m->out + i, '\n', m->out_len - i);
        size_t ll = e ? (size_t)(e - (m->out + i)) : m->out_len - i;
        if (ll && m->out[i + ll - 1] == '\r') ll--;
        if (ll == n && !memcmp(m->out + i, line, n))
            return 1;
        i += ll + 2;
    }
    return 0;
}

static int count_sent(const Mock *m, const char *prefix)
{
    size_t n = strlen(prefix), i = 0;
    int k = 0;
    while (i < m->out_len) {
        const char *e = memchr(m->out + i, '\n', m->out_len - i);
        size_t ll = e ? (size_t)(e - (m->out + i)) + 1 : m->out_len - i;
        if (ll >= n && !memcmp(m->out + i, prefix, n)) k++;
        i += ll;
    }
    return k;
}

static void feed(IrClient *c, Mock *m, const char *s, uint32_t now)
{
    m->in = s;
    m->in_len = strlen(s);
    m->in_pos = 0;
    CHECK_EQ(ir_pump(c, now), NK_OK);
}

static IrClient *start(Mock *m, Log *l, const IrConfig *cfg)
{
    NKTransport t;
    NKConn *conn;
    IrClient *c;
    memset(m, 0, sizeof(*m));
    memset(l, 0, sizeof(*l));
    t = mock_transport(m);
    conn = nk_conn_new(0, t, 0);
    c = ir_new(conn, cfg, on_ev, l, 0);
    if (!c) nk_conn_free(conn);
    return c;
}

int main(void)
{
    static Mock m;
    static Log l;
    IrConfig cfg;
    IrClient *c;
    const IrMember *mem;

    /* ---- registration with SASL ---- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nick = "taken";
    cfg.realname = "Krissie Kochanski";
    cfg.sasl_user = "dale";
    cfg.sasl_pass = "s3cret";
    cfg.tls = 1;
    cfg.burst = 3;
    cfg.interval_ms = 1000;
    c = start(&m, &l, &cfg);
    CHECK(c != 0);
    CHECK_EQ(ir_pump(c, 0), NK_OK);
    CHECK(sent(&m, "CAP LS 302"));
    CHECK(sent(&m, "NICK taken"));
    CHECK(sent(&m, "USER taken 0 * :Krissie Kochanski"));
    feed(c, &m, ":srv CAP * LS * :sasl=EXTERNAL,PLAIN server-time foo=bar\r\n"
                ":srv 433 * taken :Nickname is already in use\r\n", 10);
    CHECK(!sent(&m, "CAP REQ :sasl server-time"));      /* waits for the last LS line */
    CHECK(sent(&m, "NICK taken_"));
    CHECK(!strcmp(ir_nick(c), "taken_"));
    feed(c, &m, ":srv CAP * LS :multi-prefix echo-message extended-join away-notify\r\n", 20);
    CHECK(sent(&m, "CAP REQ :sasl server-time multi-prefix away-notify extended-join echo-message"));
    CHECK(!sent(&m, "CAP END"));
    feed(c, &m, ":srv CAP taken_ ACK :sasl server-time multi-prefix away-notify extended-join echo-message\r\n", 30);
    CHECK(sent(&m, "AUTHENTICATE PLAIN"));
    CHECK(ir_has_cap(c, "echo-message") && ir_has_cap(c, "sasl") && !ir_has_cap(c, "foo"));
    feed(c, &m, "AUTHENTICATE +\r\n", 40);
    CHECK(sent(&m, "AUTHENTICATE ZGFsZQBkYWxlAHMzY3JldA=="));
    feed(c, &m, ":srv 900 taken_ taken_!u@h dale :You are now logged in as dale\r\n"
                ":srv 903 taken_ :SASL authentication successful\r\n", 50);
    CHECK(sent(&m, "CAP END"));
    CHECK(has_ev(&l, IR_EV_SASL, 0));
    CHECK(has_ev(&l, IR_EV_CAPS, "-|-|sasl server-time multi-prefix away-notify extended-join echo-message|-|-|0"));
    feed(c, &m, ":srv 001 taken_ :Welcome\r\n"
                ":srv 005 taken_ CHANTYPES=# PREFIX=(ov)@+ CASEMAPPING=rfc1459 NICKLEN=16 :are supported\r\n"
                "PING :tok123\r\n", 60);
    CHECK(ir_registered(c));
    CHECK(sent(&m, "PONG :tok123"));
    CHECK_EQ(ir_support(c)->nicklen, 16);
    CHECK(has_ev(&l, IR_EV_REGISTERED, "-|-|Welcome|-|-|0"));

    /* ---- join, names with multi-prefix, topic, mode ---- */
    CHECK_EQ(ir_command(c, 0, "/join #Amiga"), NK_OK);
    CHECK_EQ(ir_pump(c, 60), NK_OK);
    CHECK(sent(&m, "JOIN #Amiga"));
    feed(c, &m, "@time=2026-11-13T19:00:00.000Z :taken_!u@h JOIN #Amiga dale :Krissie\r\n"
                ":srv 332 taken_ #amiga :Amiga talk\r\n"
                ":srv 353 taken_ = #amiga :@+op +voice taken_\r\n"
                ":srv 353 taken_ = #amiga :plain\r\n"
                ":srv 366 taken_ #amiga :End\r\n"
                ":other!o@h JOIN #amiga * :Someone\r\n"
                ":srv MODE #amiga +o-v+l voice voice 50\r\n", 70);
    CHECK(has_ev(&l, IR_EV_JOIN, "#Amiga|taken_|-|dale|2026-11-13T19:00:00.000Z|1"));
    CHECK_EQ(ir_channel_count(c), 1);
    CHECK(!strcmp(ir_topic(c, "#AMIGA"), "Amiga talk"));
    CHECK_EQ(ir_member_count(c, "#amiga"), 5);
    mem = ir_find_member(c, "#amiga", "OP");
    CHECK(mem && !strcmp(mem->prefixes, "@+"));
    mem = ir_find_member(c, "#amiga", "voice");
    CHECK(mem && !strcmp(mem->prefixes, "@"));
    mem = ir_find_member(c, "#amiga", "other");
    CHECK(mem && !strcmp(mem->account, "*"));
    CHECK(has_ev(&l, IR_EV_NAMES, 0));
    CHECK(has_ev(&l, IR_EV_MODE, "#amiga|srv|+o-v+l voice voice 50|-|-|0"));
    /* a NAMES refresh replaces the list */
    feed(c, &m, ":srv 353 taken_ = #amiga :taken_ op\r\n:srv 366 taken_ #amiga :End\r\n", 75);
    CHECK_EQ(ir_member_count(c, "#amiga"), 2);
    feed(c, &m, ":srv 353 taken_ = #amiga :@+op +voice taken_ other plain\r\n:srv 366 taken_ #amiga :End\r\n", 76);

    /* ---- messages ---- */
    l.n = 0;
    feed(c, &m, ":op!o@h PRIVMSG #amiga :hi taken_\r\n"
                ":friend!f@h PRIVMSG taken_ :psst\r\n"
                ":friend!f@h PRIVMSG taken_ :\001ACTION waves\001\r\n"
                ":srv NOTICE * :*** Looking up your hostname\r\n"
                ":op!o@h PRIVMSG @#amiga :ops only\r\n"
                ":friend!f@h PRIVMSG taken_ :\001VERSION\001\r\n"
                ":friend!f@h PRIVMSG taken_ :\001VERSION\001\r\n", 80);
    CHECK(has_ev(&l, IR_EV_MESSAGE, "#amiga|op|hi taken_|-|-|0"));
    CHECK(has_ev(&l, IR_EV_MESSAGE, "friend|friend|psst|-|-|0"));
    CHECK(has_ev(&l, IR_EV_ACTION, "friend|friend|waves|-|-|0"));
    CHECK(has_ev(&l, IR_EV_NOTICE, "-|srv|*** Looking up your hostname|-|-|0"));
    CHECK(has_ev(&l, IR_EV_MESSAGE, "#amiga|op|ops only|-|-|0"));
    CHECK(has_ev(&l, IR_EV_CTCP, "friend|friend|VERSION|-|-|0"));
    CHECK_EQ(count_sent(&m, "NOTICE friend :\001VERSION GFOpenIRC"), 1);    /* one reply in 2 s */

    /* echo-message: our own lines come back from the server, no local echo */
    l.n = 0;
    CHECK_EQ(ir_command(c, "#amiga", "hello all"), NK_OK);
    CHECK_EQ(ir_pump(c, 90), NK_OK);
    CHECK(sent(&m, "PRIVMSG #amiga :hello all"));
    CHECK_EQ(l.n, 0);
    feed(c, &m, ":taken_!u@h PRIVMSG #amiga :hello all\r\n"
                ":taken_!u@h PRIVMSG friend :back to you\r\n", 90);
    CHECK(has_ev(&l, IR_EV_MESSAGE, "#amiga|taken_|hello all|-|-|1"));
    CHECK(has_ev(&l, IR_EV_MESSAGE, "friend|taken_|back to you|-|-|1"));

    /* commands */
    CHECK_EQ(ir_command(c, "#amiga", "/me dances"), NK_OK);
    CHECK_EQ(ir_command(c, "#amiga", "//slash"), NK_OK);
    CHECK_EQ(ir_command(c, 0, "plain text in the server tab"), NK_EARG);
    CHECK_EQ(ir_command(c, 0, "/bogus x"), NK_EARG);
    CHECK_EQ(ir_command(c, 0, "/msg"), NK_EARG);
    CHECK_EQ(ir_command(c, 0, "/raw PRIVMSG x :a\rQUIT"), NK_EARG);
    CHECK_EQ(ir_say(c, "a b", "x"), NK_EARG);
    CHECK_EQ(ir_join(c, "#a\r\nQUIT", 0), NK_EARG);
    CHECK_EQ(ir_set_nick(c, "a:b"), NK_EARG);
    /* throttle: burst of 3 then one a second */
    CHECK_EQ(ir_command(c, 0, "/msg friend one"), NK_OK);
    CHECK_EQ(ir_command(c, 0, "/msg friend two"), NK_OK);
    CHECK_EQ(ir_command(c, 0, "/msg friend three"), NK_OK);
    CHECK(ir_queued(c) > 0);
    CHECK(!sent(&m, "PRIVMSG friend :three"));
    CHECK_EQ(ir_pump(c, 2000), NK_OK);
    CHECK_EQ(ir_pump(c, 5000), NK_OK);
    CHECK_EQ(ir_queued(c), 1);           /* 5 lines over 3 s of credit */
    CHECK_EQ(ir_pump(c, 9000), NK_OK);
    CHECK_EQ(ir_queued(c), 0);
    CHECK(sent(&m, "PRIVMSG #amiga :\001ACTION dances\001"));
    CHECK(sent(&m, "PRIVMSG #amiga :/slash"));
    CHECK(sent(&m, "PRIVMSG friend :three"));
    /* a long message is split at spaces, each piece fits one line */
    {
        char text[1200];
        size_t i, pos = 0;
        for (i = 0; i < 120; i++)
            pos += (size_t)sprintf(text + pos, "word%03u ", (unsigned)i);
        text[pos - 1] = 0;
        m.out_len = 0;
        CHECK_EQ(ir_say(c, "#amiga", text), NK_OK);
        CHECK_EQ(ir_pump(c, 20000), NK_OK);
        CHECK_EQ(ir_pump(c, 40000), NK_OK);
        CHECK_EQ(count_sent(&m, "PRIVMSG #amiga :word000 "), 1);
        CHECK(count_sent(&m, "PRIVMSG #amiga :") >= 3);
        {
            size_t at = 0;
            int ok = 1;
            while (at < m.out_len) {
                const char *e = memchr(m.out + at, '\n', m.out_len - at);
                size_t ll = (size_t)(e - (m.out + at)) + 1;
                /* with ":taken_!taken@<63-byte host> " in front: 512 */
                if (ll + 1 + 6 + 1 + 5 + 1 + 63 + 1 > 512) ok = 0;
                at += ll;
            }
            CHECK(ok);
        }
    }

    /* ---- nick changes, parts, kicks, quits ---- */
    l.n = 0;
    feed(c, &m, ":op!o@h NICK :Op2\r\n"
                ":TAKEN_!u@h NICK :krissie\r\n"
                ":plain!p@h PART #amiga :bye\r\n"
                ":Op2!o@h KICK #amiga voice :behave\r\n"
                ":other!o@h QUIT :Ping timeout\r\n"
                ":other!o@h AWAY :lunch\r\n"
                ":Op2!o@h AWAY :gone\r\n"
                ":Op2!o@h ACCOUNT op2acct\r\n", 41000);
    CHECK(!strcmp(ir_nick(c), "krissie"));
    CHECK(has_ev(&l, IR_EV_NICK, "#Amiga|op|-|Op2|-|0"));
    CHECK(has_ev(&l, IR_EV_PART, "#amiga|plain|bye|-|-|0"));
    CHECK(has_ev(&l, IR_EV_KICK, "#amiga|Op2|behave|voice|-|0"));
    CHECK(has_ev(&l, IR_EV_QUIT, "#Amiga|other|Ping timeout|-|-|0"));
    mem = ir_find_member(c, "#amiga", "op2");
    CHECK(mem && !strcmp(mem->prefixes, "@+") && mem->away && !strcmp(mem->account, "op2acct"));
    CHECK(ir_find_member(c, "#amiga", "krissie") != 0);
    CHECK_EQ(ir_member_count(c, "#amiga"), 2);
    feed(c, &m, ":Op2!o@h KICK #amiga krissie :out\r\n", 41010);
    CHECK_EQ(ir_channel_count(c), 0);
    /* errors land in the server tab, or the channel they name */
    l.n = 0;
    feed(c, &m, ":srv 404 krissie #amiga :Cannot send to channel\r\n"
                ":srv 433 krissie Op2 :Nickname is already in use\r\n"
                "ERROR :Closing link\r\n", 41020);
    CHECK(has_ev(&l, IR_EV_ERROR, "#amiga|srv|Cannot send to channel|#amiga|-|0"));
    CHECK(has_ev(&l, IR_EV_ERROR, "-|srv|Nickname is already in use|Op2|-|0"));
    CHECK(has_ev(&l, IR_EV_ERROR, "-|-|Closing link|ERROR|-|0"));

    /* keepalive: a PING after quiet, a timeout when it isn't answered */
    m.out_len = 0;
    CHECK_EQ(ir_pump(c, 41020 + 89999), NK_OK);
    CHECK(!sent(&m, "PING :gfopenirc"));
    CHECK_EQ(ir_pump(c, 41020 + 90000), NK_OK);
    CHECK(sent(&m, "PING :gfopenirc"));
    feed(c, &m, ":srv PONG srv :gfopenirc\r\n", 41020 + 100000);
    CHECK_EQ(ir_pump(c, 41020 + 100000 + 90000), NK_OK);     /* PINGs again */
    CHECK_EQ(ir_pump(c, 41020 + 100000 + 180000), NK_ETIMEDOUT);
    CHECK_EQ(ir_quit(c, "bye"), NK_OK);
    CHECK(ir_quitting(c));
    ir_free(c);

    /* ---- SASL refused but not required: carry on without ---- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nick = "k";
    cfg.sasl_user = "dale";
    cfg.sasl_pass = "wrong";
    cfg.tls = 1;
    c = start(&m, &l, &cfg);
    ir_pump(c, 0);
    feed(c, &m, ":srv CAP * LS :sasl\r\n:srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n"
                ":srv 904 k :SASL authentication failed\r\n", 1);
    CHECK(has_ev(&l, IR_EV_SASL, "-|-|SASL authentication failed|failed|-|0"));
    CHECK(sent(&m, "CAP END"));
    CHECK(!ir_quitting(c));
    ir_free(c);

    /* ---- SASL required and refused: quit ---- */
    cfg.sasl_required = 1;
    c = start(&m, &l, &cfg);
    ir_pump(c, 0);
    feed(c, &m, ":srv CAP * LS :sasl\r\n:srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n"
                ":srv 904 k :SASL authentication failed\r\n", 1);
    CHECK(ir_quitting(c));
    CHECK(!sent(&m, "CAP END"));
    ir_free(c);

    /* ---- required, but the server has no SASL PLAIN ---- */
    c = start(&m, &l, &cfg);
    ir_pump(c, 0);
    feed(c, &m, ":srv CAP * LS :sasl=EXTERNAL server-time\r\n:srv CAP * ACK :server-time\r\n", 1);
    CHECK(sent(&m, "CAP REQ :server-time"));
    CHECK(ir_quitting(c));
    ir_free(c);

    /* ---- a server with no CAP at all; no echo-message: local echo ---- */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nick = "old";
    c = start(&m, &l, &cfg);
    ir_pump(c, 0);
    feed(c, &m, ":srv 421 old CAP :Unknown command\r\n:srv 001 old :Hi\r\n", 1);
    CHECK(ir_registered(c));
    l.n = 0;
    CHECK_EQ(ir_say(c, "#x", "local"), NK_OK);
    CHECK(has_ev(&l, IR_EV_MESSAGE, "#x|old|local|-|-|1"));
    ir_free(c);

    /* ---- the password is sent in 400-byte pieces, "+" after an exact fit ---- */
    {
        char pw[300];
        memset(pw, 'p', 286);       /* 4+1+4+1+286 = 296 bytes -> 396 b64: one piece */
        pw[286] = 0;
        memset(&cfg, 0, sizeof(cfg));
        cfg.nick = "k";
        cfg.sasl_user = "dale";
        cfg.sasl_pass = pw;
        cfg.tls = 1;
        c = start(&m, &l, &cfg);
        ir_pump(c, 0);
        feed(c, &m, ":srv CAP * LS :sasl\r\n:srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n", 1);
        CHECK_EQ(count_sent(&m, "AUTHENTICATE "), 2);   /* PLAIN + one piece */
        ir_free(c);
        memset(pw, 'p', 290);       /* 300 bytes -> 400 b64: one piece then "+" */
        pw[290] = 0;
        c = start(&m, &l, &cfg);
        ir_pump(c, 0);
        feed(c, &m, ":srv CAP * LS :sasl\r\n:srv CAP * ACK :sasl\r\nAUTHENTICATE +\r\n", 1);
        CHECK(sent(&m, "AUTHENTICATE +"));
        CHECK_EQ(count_sent(&m, "AUTHENTICATE "), 3);
        ir_free(c);
    }

    /* a password over plain TCP is refused unless asked for */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nick = "k";
    cfg.sasl_user = "dale";
    cfg.sasl_pass = "s3cret";
    CHECK_EQ(ir_config_check(&cfg), NK_EREFUSED);
    CHECK(start(&m, &l, &cfg) == 0);
    cfg.sasl_user = 0;
    cfg.server_pass = "serverpw";
    CHECK_EQ(ir_config_check(&cfg), NK_EREFUSED);
    cfg.allow_plain_auth = 1;
    CHECK_EQ(ir_config_check(&cfg), NK_OK);
    c = start(&m, &l, &cfg);
    CHECK(c != 0);
    ir_pump(c, 0);
    CHECK(sent(&m, "PASS serverpw"));
    ir_free(c);
    cfg.allow_plain_auth = 0;
    cfg.tls = 1;
    CHECK_EQ(ir_config_check(&cfg), NK_OK);
    cfg.server_pass = 0;
    cfg.tls = 0;
    CHECK_EQ(ir_config_check(&cfg), NK_OK);        /* no password: fine */
    cfg.server_pass = "a\r\nQUIT";
    cfg.tls = 1;
    CHECK_EQ(ir_config_check(&cfg), NK_EARG);

    /* bad configuration */
    memset(&cfg, 0, sizeof(cfg));
    cfg.nick = "a b";
    CHECK(start(&m, &l, &cfg) == 0);
    cfg.nick = "ok";
    cfg.realname = "x\r\nQUIT";
    CHECK(start(&m, &l, &cfg) == 0);
    TEST_DONE();
}
