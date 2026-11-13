/* Chat window model tests: buffers and the lines events make, the nick
 * list, history and nick completion. MIT licence, (c) 2026 AmiCradle Team. */
#include "gfopenirc/ui_model.h"
#include "nk_test.h"

static IrEvent ev(int type, const char *target, const char *nick, const char *text, const char *extra, int self)
{
    IrEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type; e.target = target; e.nick = nick; e.text = text; e.extra = extra; e.self = self;
    return e;
}

static const char *last(IuState *s, long i)
{
    IuBuffer *b = iu_buffer(s, (size_t)i);
    const NKTvLine *l = b ? nk_tv_line(b->log, nk_tv_count(b->log) - 1) : NULL;
    return l ? l->text : "(none)";
}

static int last_kind(IuState *s, long i)
{
    IuBuffer *b = iu_buffer(s, (size_t)i);
    const NKTvLine *l = nk_tv_line(b->log, nk_tv_count(b->log) - 1);
    return l ? l->kind : -1;
}

static const char *label(IuState *s, size_t i)
{
    static char b[256];
    iu_label(s, i, b, sizeof(b));
    return b;
}

static void buffers(void)
{
    IuState *s = iu_new("irc.example.org");
    IrEvent e;
    long a, q;
    int m;
    char t[6];

    iu_set_nick(s, "kim");
    CHECK_EQ(iu_count(s), 1);
    CHECK(!strcmp(label(s, 0), "irc.example.org"));
    iu_set_server(s, "ExampleNet");
    CHECK(!strcmp(label(s, 0), "ExampleNet"));

    /* our join makes the channel's buffer; another's join to a channel we don't show is dropped */
    e = ev(IR_EV_JOIN, "#Amiga", "kim", NULL, NULL, 1);
    a = iu_event(s, &e, "20:30", &m);
    CHECK_EQ(a, 1); CHECK_EQ(m, 1);
    CHECK(!strcmp(last(s, a), "[20:30] --> You joined #Amiga"));
    CHECK(iu_buffer(s, 1)->kind == IU_CHANNEL && iu_buffer(s, 1)->joined);
    e = ev(IR_EV_JOIN, "#other", "bob", NULL, NULL, 0);
    CHECK_EQ(iu_event(s, &e, "20:30", &m), -1);
    CHECK_EQ(iu_count(s), 2);
    CHECK_EQ(iu_find(s, "#amiga"), 1);        /* any case */
    CHECK_EQ(iu_find(s, "#AMIGA"), 1);

    /* messages, unread counts and highlights while another buffer shows */
    e = ev(IR_EV_MESSAGE, "#amiga", "bob", "hello all", NULL, 0);
    e.time = "2026-11-13T20:31:05.000Z";
    CHECK_EQ(iu_event(s, &e, "20:30", &m), 1);
    CHECK(!strcmp(last(s, 1), "[20:31] <bob> hello all"));
    CHECK_EQ(last_kind(s, 1), IU_L_MESSAGE);
    CHECK_EQ(iu_buffer(s, 1)->unread, 1);
    CHECK(!strcmp(label(s, 1), "  #Amiga (1)"));
    e = ev(IR_EV_MESSAGE, "#amiga", "bob", "Kim: are you there?", NULL, 0);
    iu_event(s, &e, "20:32", &m);
    CHECK_EQ(last_kind(s, 1), IU_L_HIGHLIGHT);
    CHECK(!strcmp(label(s, 1), "  #Amiga (2!)"));
    e = ev(IR_EV_MESSAGE, "#amiga", "bob", "kimberly isn't you", NULL, 0);
    iu_event(s, &e, "20:32", &m);
    CHECK_EQ(last_kind(s, 1), IU_L_MESSAGE);
    iu_show(s, 1);
    CHECK(!strcmp(label(s, 1), "  #Amiga"));
    CHECK_EQ(iu_shown(s), 1);
    e = ev(IR_EV_MESSAGE, "#amiga", "bob", "seen", NULL, 0);
    iu_event(s, &e, "20:33", &m);
    CHECK_EQ(iu_buffer(s, 1)->unread, 0);   /* shown: nothing to count */
    iu_show(s, IU_NONE);                    /* another server's buffer shown: this one counts */
    CHECK(iu_shown(s) == IU_NONE);
    iu_event(s, &e, "20:33", &m);
    CHECK_EQ(iu_buffer(s, 1)->unread, 1);
    iu_show(s, 1);
    CHECK_EQ(iu_buffer(s, 1)->unread, 0);

    /* a private message opens a query, counted as a highlight */
    e = ev(IR_EV_MESSAGE, "Bob", "Bob", "psst", NULL, 0);
    q = iu_event(s, &e, "20:34", &m);
    CHECK_EQ(q, 2);
    CHECK(iu_buffer(s, 2)->kind == IU_QUERY);
    CHECK(!strcmp(label(s, 2), "  Bob (1!)"));
    e = ev(IR_EV_ACTION, "Bob", "Bob", "waves", NULL, 0);
    iu_event(s, &e, "20:34", &m);
    CHECK(!strcmp(last(s, q), "[20:34] * Bob waves"));
    e = ev(IR_EV_NOTICE, NULL, "NickServ", "This nick is registered", NULL, 0);
    CHECK_EQ(iu_event(s, &e, "20:35", &m), 0);
    CHECK(!strcmp(last(s, 0), "[20:35] -NickServ- This nick is registered"));

    /* our own lines (no echo-message) */
    CHECK_EQ(iu_own(s, "#amiga", "kim", "hi bob", 0, "20:36"), 1);
    CHECK(!strcmp(last(s, 1), "[20:36] <kim> hi bob"));
    CHECK_EQ(last_kind(s, 1), IU_L_OWN);
    CHECK_EQ(iu_own(s, "#amiga", "kim", "nods", 1, "20:36"), 1);
    CHECK(!strcmp(last(s, 1), "[20:36] * kim nods"));
    CHECK_EQ(iu_note(s, NULL, "Connecting to irc.example.org:6697", "20:00"), 0);
    CHECK(!strcmp(last(s, 0), "[20:00] -- Connecting to irc.example.org:6697"));

    /* joins, parts, quits, nicks, kicks, topics, modes: lines and nick list changes */
    e = ev(IR_EV_JOIN, "#amiga", "carl", NULL, "carl_acct", 0);
    iu_event(s, &e, "20:40", &m);
    CHECK(!strcmp(last(s, 1), "[20:40] --> carl joined #amiga") && m == 1);
    e = ev(IR_EV_PART, "#amiga", "carl", "bye", NULL, 0);
    iu_event(s, &e, "20:41", &m);
    CHECK(!strcmp(last(s, 1), "[20:41] <-- carl left #amiga (bye)") && m == 1);
    e = ev(IR_EV_QUIT, "#amiga", "dave", "", NULL, 0);
    iu_event(s, &e, "20:42", &m);
    CHECK(!strcmp(last(s, 1), "[20:42] <-- dave quit"));
    e = ev(IR_EV_NICK, "#amiga", "bob", NULL, "bobby", 0);
    iu_event(s, &e, "20:43", &m);
    CHECK(!strcmp(last(s, 1), "[20:43] bob is now known as bobby") && m == 1);
    e = ev(IR_EV_NICK, "#amiga", "kim", NULL, "kim_", 1);
    iu_event(s, &e, "20:43", &m);
    e = ev(IR_EV_MESSAGE, "#amiga", "bobby", "kim_ hi", NULL, 0);
    iu_show(s, 0);
    iu_event(s, &e, "20:44", &m);
    CHECK_EQ(last_kind(s, 1), IU_L_HIGHLIGHT);   /* our new nick */
    e = ev(IR_EV_TOPIC, "#amiga", "bobby", "Amiga talk", NULL, 0);
    iu_event(s, &e, "20:45", &m);
    CHECK(!strcmp(last(s, 1), "[20:45] bobby changed the topic to: Amiga talk"));
    CHECK(!strcmp(iu_buffer(s, 1)->topic, "Amiga talk"));
    e = ev(IR_EV_TOPIC, "#amiga", NULL, "Amiga talk", NULL, 0);
    iu_event(s, &e, "20:45", &m);
    CHECK(!strcmp(last(s, 1), "[20:45] Topic: Amiga talk"));
    e = ev(IR_EV_MODE, "#amiga", "bobby", "+o kim_", NULL, 0);
    iu_event(s, &e, "20:46", &m);
    CHECK(!strcmp(last(s, 1), "[20:46] bobby sets mode +o kim_") && m == 1);
    e = ev(IR_EV_NAMES, "#amiga", NULL, NULL, NULL, 0);
    CHECK_EQ(iu_event(s, &e, "20:46", &m), 1);
    CHECK_EQ(m, 1);
    e = ev(IR_EV_KICK, "#amiga", "bobby", "spam", "eve", 0);
    iu_event(s, &e, "20:47", &m);
    CHECK(!strcmp(last(s, 1), "[20:47] bobby kicked eve (spam)"));
    e = ev(IR_EV_KICK, "#amiga", "bobby", "", "KIM_", 0);
    iu_event(s, &e, "20:48", &m);
    CHECK(!strcmp(last(s, 1), "[20:48] You were kicked by bobby"));
    CHECK(!iu_buffer(s, 1)->joined);
    CHECK(!strncmp(label(s, 1), "  -#Amiga (", 11));      /* not in it any more */
    e = ev(IR_EV_JOIN, "#amiga", "kim_", NULL, NULL, 1);
    iu_event(s, &e, "20:49", &m);
    CHECK(iu_buffer(s, 1)->joined);
    e = ev(IR_EV_PART, "#amiga", "kim_", "", NULL, 1);
    iu_event(s, &e, "20:50", &m);
    CHECK(!strcmp(last(s, 1), "[20:50] <-- You left #Amiga") && !iu_buffer(s, 1)->joined);

    /* server lines */
    e = ev(IR_EV_ERROR, NULL, NULL, "Nickname is already in use", NULL, 0);
    iu_event(s, &e, "20:51", &m);
    CHECK(!strcmp(last(s, 0), "[20:51] ! Nickname is already in use") && last_kind(s, 0) == IU_L_ERROR);
    e = ev(IR_EV_SASL, NULL, NULL, NULL, "ok", 0);
    iu_event(s, &e, "20:51", &m);
    CHECK(!strcmp(last(s, 0), "[20:51] SASL login: done"));
    e = ev(IR_EV_CAPS, NULL, NULL, "sasl server-time", NULL, 0);
    iu_event(s, &e, "20:51", &m);
    CHECK(!strcmp(last(s, 0), "[20:51] Capabilities: sasl server-time"));
    e = ev(IR_EV_INVITE, NULL, "bobby", NULL, "#secret", 0);
    iu_event(s, &e, "20:52", &m);
    CHECK(!strcmp(last(s, 0), "[20:52] bobby invites you to #secret"));
    e = ev(IR_EV_SERVER, NULL, NULL, "Welcome to ExampleNet", NULL, 0);
    iu_event(s, &e, "20:52", &m);
    CHECK(!strcmp(last(s, 0), "[20:52] Welcome to ExampleNet"));
    e = ev(IR_EV_CTCP, NULL, "bobby", "VERSION", NULL, 0);
    iu_event(s, &e, "20:52", &m);
    CHECK(!strcmp(last(s, 0), "[20:52] bobby asked for CTCP VERSION"));
    e = ev(IR_EV_MESSAGE, "#amiga", "x", "\x02" "bold" "\x02 and \x03" "4red", NULL, 0);
    iu_event(s, &e, "20:53", &m);
    CHECK(!strcmp(last(s, 1), "[20:53] <x> bold and red"));   /* codes parsed by the text view */

    /* closing: not the server's; the shown one moves */
    iu_show(s, 2);
    CHECK_EQ(iu_close(s, 0), 0);
    CHECK_EQ(iu_close(s, 2), 1);
    CHECK_EQ(iu_count(s), 2);
    CHECK_EQ(iu_shown(s), 1);
    CHECK_EQ(iu_close(s, 9), 0);
    CHECK(iu_buffer(s, 5) == NULL);

    /* chantypes from ISUPPORT decide what is a channel */
    iu_set_chantypes(s, "#!", IR_CM_ASCII);
    CHECK(iu_buffer(s, (size_t)iu_open(s, "!abcde"))->kind == IU_CHANNEL);
    CHECK(iu_buffer(s, (size_t)iu_open(s, "&local"))->kind == IU_QUERY);

    CHECK(!strcmp(iu_time("bad", "09:15", t), "09:15"));
    CHECK(!strcmp(iu_time(NULL, NULL, t), "--:--"));
    CHECK(iu_mentions("hi [kim]!", "[KIM]", IR_CM_ASCII));
    CHECK(iu_mentions("hi {kim}!", "[KIM]", IR_CM_RFC1459));
    CHECK(!iu_mentions("hi {kim}!", "[KIM]", IR_CM_ASCII));
    CHECK(!iu_mentions("kim_x", "kim", IR_CM_ASCII));
    iu_free(s);
}

static void nicks(void)
{
    IrMember m[5];
    const IrMember *p[5];
    IuNicks l;
    char b[64];
    int i;
    memset(m, 0, sizeof(m));
    strcpy(m[0].nick, "zed");
    strcpy(m[1].nick, "Ann");   strcpy(m[1].prefixes, "+");
    strcpy(m[2].nick, "bob");   strcpy(m[2].prefixes, "@+");
    strcpy(m[3].nick, "Carl");  m[3].away = 1;
    strcpy(m[4].nick, "al");    strcpy(m[4].prefixes, "@");
    for (i = 0; i < 5; i++) p[i] = &m[i];
    iu_nicks_init(&l);
    CHECK_EQ(iu_nicks_set(&l, p, 5, "@+", IR_CM_RFC1459), NK_OK);
    CHECK_EQ(l.count, 5);
    iu_nicks_label(&l, 0, b, sizeof(b)); CHECK(!strcmp(b, "@al"));
    iu_nicks_label(&l, 1, b, sizeof(b)); CHECK(!strcmp(b, "@bob"));
    iu_nicks_label(&l, 2, b, sizeof(b)); CHECK(!strcmp(b, "+Ann"));
    iu_nicks_label(&l, 3, b, sizeof(b)); CHECK(!strcmp(b, "(Carl)"));
    iu_nicks_label(&l, 4, b, sizeof(b)); CHECK(!strcmp(b, "zed"));
    CHECK_EQ(iu_nicks_label(&l, 5, b, sizeof(b)), 0);
    /* a prefix the server doesn't list is shown as none */
    strcpy(m[0].prefixes, "~");
    iu_nicks_set(&l, p, 5, "@+", IR_CM_RFC1459);
    iu_nicks_label(&l, 4, b, sizeof(b)); CHECK(!strcmp(b, "zed"));
    iu_nicks_free(&l);
}

static void input(void)
{
    IuHistory h;
    IuNicks l;
    IrMember m[3];
    const IrMember *p[3];
    char out[256];
    long c;
    int i;

    iu_hist_init(&h);
    CHECK(iu_hist_up(&h) == NULL);
    iu_hist_add(&h, "one");
    iu_hist_add(&h, "two");
    iu_hist_add(&h, "two");          /* same as the newest: once */
    iu_hist_add(&h, "");
    CHECK_EQ(h.count, 2);
    CHECK(!strcmp(iu_hist_up(&h), "two"));
    CHECK(!strcmp(iu_hist_up(&h), "one"));
    CHECK(iu_hist_up(&h) == NULL);
    CHECK(!strcmp(iu_hist_down(&h), "two"));
    CHECK(!strcmp(iu_hist_down(&h), ""));
    CHECK(iu_hist_down(&h) == NULL);
    for (i = 0; i < IU_HISTORY + 5; i++) { char b[16]; sprintf(b, "l%d", i); iu_hist_add(&h, b); }
    CHECK_EQ(h.count, IU_HISTORY);
    CHECK(!strcmp(h.line[0], "l5"));
    iu_hist_free(&h);

    memset(m, 0, sizeof(m));
    strcpy(m[0].nick, "Bobby"); strcpy(m[0].prefixes, "@");
    strcpy(m[1].nick, "bob");
    strcpy(m[2].nick, "carl");
    for (i = 0; i < 3; i++) p[i] = &m[i];
    iu_nicks_init(&l);
    iu_nicks_set(&l, p, 3, "@+", IR_CM_RFC1459);
    c = iu_complete(&l, "bo", 2, NULL, IR_CM_RFC1459, out, sizeof(out));
    CHECK(c == 7 && !strcmp(out, "Bobby: "));
    c = iu_complete(&l, "bo", 2, "Bobby", IR_CM_RFC1459, out, sizeof(out));
    CHECK(c == 5 && !strcmp(out, "bob: "));
    c = iu_complete(&l, "bo", 2, "bob", IR_CM_RFC1459, out, sizeof(out));   /* round again */
    CHECK(c == 7 && !strcmp(out, "Bobby: "));
    c = iu_complete(&l, "thanks CA for it", 9, NULL, IR_CM_RFC1459, out, sizeof(out));
    CHECK(c == 12 && !strcmp(out, "thanks carl for it"));       /* the space there kept, one only */
    c = iu_complete(&l, "CA is here", 2, NULL, IR_CM_RFC1459, out, sizeof(out));
    CHECK(c == 6 && !strcmp(out, "carl: is here"));
    CHECK_EQ(iu_complete(&l, "zz", 2, NULL, IR_CM_RFC1459, out, sizeof(out)), -1);
    CHECK_EQ(iu_complete(&l, "hi ", 3, NULL, IR_CM_RFC1459, out, sizeof(out)), -1);   /* no word */
    iu_nicks_free(&l);
}

int main(void)
{
    buffers();
    nicks();
    input();
    TEST_DONE();
}
