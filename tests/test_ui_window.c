/* Chat window tests: the window on GFOpenLayout's UI, driven with olui's
 * own input calls (the pointer and keys as the Amiga glue gives them).
 * MIT licence, (c) 2026 AmiCradle Team. */
#include "gfopenirc/ui_window.h"
#include "nk_test.h"

/* about 0.54 em a character, as GFOpenLayout's own tests measure */
static int measure(void *userdata, const char *utf8, size_t length, const ol_style *style, ol_text_metrics *m)
{
    size_t i, glyphs = 0;
    (void)userdata;
    for (i = 0; i < length; ++i)
        if (((unsigned char)utf8[i] & 0xc0u) != 0x80u) ++glyphs;
    m->width = (ol_unit)glyphs * style->font_size * 7 / 13;
    m->ascent = style->font_size * 10 / 13;
    m->descent = style->font_size * 3 / 13;
    return 1;
}

/* what the program saw */
static struct {
    char buffer[200], text[512];
    IuState *s;
    int commands, shown, closes, connects;
    IwConnect c;
} seen;

static IwWin *win;
static IuNicks members;

static void command(void *user, IuState *s, size_t buffer, const char *text)
{
    (void)user;
    seen.commands++;
    seen.s = s;
    snprintf(seen.buffer, sizeof(seen.buffer), "%s", iu_buffer(s, buffer)->name);
    snprintf(seen.text, sizeof(seen.text), "%s", text);
    if (text[0] != '/') { iu_own(s, iu_buffer(s, buffer)->name, "me", text, 0, "21:00"); iw_win_refresh(win); }
}

static void shown(void *user, IuState *s, size_t buffer)
{
    (void)user;
    seen.shown++;
    seen.s = s;
    snprintf(seen.buffer, sizeof(seen.buffer), "%s", iu_buffer(s, buffer)->name);
    if (iu_buffer(s, buffer)->kind == IU_CHANNEL) iw_win_nicks(win, &members, IR_CM_RFC1459);
}

static void close_buffer(void *user, IuState *s, size_t buffer)
{
    (void)user;
    seen.closes++;
    if (iu_close(s, buffer)) iw_win_refresh(win);
}

static void connect_to(void *user, const IwConnect *c)
{
    (void)user;
    seen.connects++;
    seen.c = *c;
}

/* text drawn, to see what the lists show */
typedef struct { char text[128][96]; int n, nimg; } drawn;

static void draw_op(void *userdata, const olui_draw_op *op)
{
    drawn *d = (drawn *)userdata;
    if (op->kind == OLUI_DRAW_TEXT && d->n < 128) {
        size_t k = op->length < 95 ? op->length : 95;
        memcpy(d->text[d->n], op->text, k); d->text[d->n][k] = 0; d->n++;
    }
    if (op->kind == OLUI_DRAW_IMAGE) d->nimg++;
}

static int was_drawn(olui *ui, const char *text)
{
    static drawn d;
    int i;
    memset(&d, 0, sizeof(d));
    olui_draw(ui, NULL, draw_op, &d);
    for (i = 0; i < d.n; i++) if (!strcmp(d.text[i], text)) return 1;
    return 0;
}

/* a label's words are drawn one by one: all the text drawn, joined */
static int shows(olui *ui, const char *text)
{
    static drawn d;
    static char all[128 * 96];
    int i;
    memset(&d, 0, sizeof(d));
    olui_draw(ui, NULL, draw_op, &d);
    all[0] = 0;
    for (i = 0; i < d.n; i++) strcat(all, d.text[i]);
    return strstr(all, text) != NULL;
}

static int images_drawn(olui *ui)
{
    static drawn d;
    memset(&d, 0, sizeof(d));
    olui_draw(ui, NULL, draw_op, &d);
    return d.nimg;
}

static void click(olui *ui, const char *id)
{
    olui_rect r;
    CHECK(olui_box(ui, olui_find(ui, id), &r));
    olui_mouse(ui, r.x + r.w / 2, r.y + r.h / 2, OLUI_MOUSE_DOWN);
    olui_mouse(ui, r.x + r.w / 2, r.y + r.h / 2, OLUI_MOUSE_UP);
}

/* a list's row: 18 px each (16 px of line, 1 above and below), from 1 px in */
static void click_row(olui *ui, const char *id, int row, int what)
{
    olui_rect r;
    CHECK(olui_box(ui, olui_find(ui, id), &r));
    olui_mouse(ui, r.x + 20, r.y + 1 + row * 18 + 9, what);
    if (what == OLUI_MOUSE_DOWN) olui_mouse(ui, r.x + 20, r.y + 1 + row * 18 + 9, OLUI_MOUSE_UP);
}

static void type(const char *s)
{
    while (*s) iw_win_key(win, (unsigned char)*s++, 0);
}

static const char *input(olui *ui) { return olui_get_text(olui_find(ui, "input")); }

static void event(IuState *s, int type, const char *target, const char *nick, const char *text, const char *extra, int self)
{
    IrEvent e;
    int m = 0;
    memset(&e, 0, sizeof(e));
    e.type = type; e.target = target; e.nick = nick; e.text = text; e.extra = extra; e.self = self;
    iu_event(s, &e, "20:59", &m);
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}

int main(void)
{
    static const IwWinOps ops = { command, shown, close_buffer, connect_to };
    olui *ui = olui_new();
    char err[128];
    IuState *libera = iu_new("irc.libera.chat"), *other = iu_new("irc.other.example");
    IrMember m[4];
    const IrMember *mp[4];
    IwConnect c;
    int i;

    olui_set_measure(ui, measure, NULL);
    win = iw_win_new(ui, &ops, NULL, err, sizeof(err));
    CHECK(win != NULL);
    if (!win) { fprintf(stderr, "%s\n", err); TEST_DONE(); }
    CHECK(olui_layout(ui, 800, 600));

    /* the panes share the window: buffers left, the log in the middle, nicks right */
    {
        olui_rect b, l, in, n;
        CHECK(olui_box(ui, olui_find(ui, "buffers"), &b) && olui_box(ui, olui_find(ui, "log"), &l)
              && olui_box(ui, olui_find(ui, "input"), &in) && olui_box(ui, olui_find(ui, "nicks"), &n));
        CHECK(b.w >= 160 && b.h > 400);
        CHECK(l.x > b.x + b.w && l.w > 350 && l.h > 400);
        CHECK(in.y >= l.y + l.h && in.x >= l.x && in.x <= l.x + 8 && in.w >= l.w - 16);
        CHECK(n.x > l.x + l.w && n.w >= 130 && n.h > 400);
    }

    /* the nick list the program keeps for #amiga */
    memset(m, 0, sizeof(m));
    strcpy(m[0].nick, "zed");
    strcpy(m[1].nick, "Ann");    strcpy(m[1].prefixes, "+");
    strcpy(m[2].nick, "anders"); strcpy(m[2].prefixes, "@");
    strcpy(m[3].nick, "Carl");   m[3].away = 1;
    for (i = 0; i < 4; i++) mp[i] = &m[i];
    iu_nicks_init(&members);
    CHECK_EQ(iu_nicks_set(&members, mp, 4, "@+", IR_CM_RFC1459), NK_OK);

    /* a server: its buffer is shown first */
    iu_set_nick(libera, "me");
    CHECK(iw_win_add(win, libera));
    CHECK_EQ(seen.shown, 1);
    CHECK(!strcmp(seen.buffer, "irc.libera.chat"));
    CHECK(iw_win_shown(win, NULL) == libera);
    iu_note(libera, NULL, "Connecting to irc.libera.chat port 6697 (TLS)", "20:58");
    iw_win_refresh(win);
    CHECK(ends_with(olui_get_text(olui_find(ui, "log")), "Connecting to irc.libera.chat port 6697 (TLS)"));

    /* joining #amiga makes its buffer under the server; a line there while the server shows counts unread */
    event(libera, IR_EV_JOIN, "#amiga", "me", NULL, NULL, 1);
    event(libera, IR_EV_TOPIC, "#amiga", "anders", "Amiga talk | AmiCradle", NULL, 0);
    event(libera, IR_EV_MESSAGE, "#amiga", "Ann", "hello all", NULL, 0);
    event(libera, IR_EV_MESSAGE, "#amiga", "Ann", "me: are you there?", NULL, 0);
    iw_win_refresh(win);
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 2);
    CHECK(was_drawn(ui, "irc.libera.chat"));
    CHECK(was_drawn(ui, "  #amiga (2!)"));          /* the lines said, our nick in one */
    CHECK(olui_vlist_selected(olui_find(ui, "buffers"), 0) && !olui_vlist_selected(olui_find(ui, "buffers"), 1));

    /* choosing it shows its log, topic and nick list, and clears the count */
    click_row(ui, "buffers", 1, OLUI_MOUSE_DOWN);
    CHECK_EQ(seen.shown, 2);
    CHECK(!strcmp(seen.buffer, "#amiga"));
    CHECK(!strcmp(olui_get_text(olui_find(ui, "topic")), "Amiga talk | AmiCradle"));
    CHECK(ends_with(olui_get_text(olui_find(ui, "log")), "<Ann> me: are you there?"));
    CHECK(strstr(olui_get_text(olui_find(ui, "log")), "<Ann> hello all\n") != NULL);
    CHECK(was_drawn(ui, "  #amiga"));
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "nicks")), 4);
    CHECK(was_drawn(ui, "@anders") && was_drawn(ui, "+Ann") && was_drawn(ui, "(Carl)") && was_drawn(ui, "zed"));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));
    CHECK_EQ(images_drawn(ui), 0);               /* nothing for the program to draw itself */

    /* Return sends what was typed for the buffer; the program adds our line */
    type("hi Ann");
    CHECK(!strcmp(input(ui), "hi Ann"));
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK_EQ(seen.commands, 1);
    CHECK(!strcmp(seen.buffer, "#amiga") && !strcmp(seen.text, "hi Ann"));
    CHECK(!strcmp(input(ui), ""));
    CHECK(ends_with(olui_get_text(olui_find(ui, "log")), "<me> hi Ann"));
    type("/topic");
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK(seen.commands == 2 && !strcmp(seen.text, "/topic"));
    /* an empty line sends nothing */
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK_EQ(seen.commands, 2);

    /* Up and Down walk this buffer's history; Up at the oldest stays */
    iw_win_key(win, OLUI_KEY_UP, 0);
    CHECK(!strcmp(input(ui), "/topic"));
    iw_win_key(win, OLUI_KEY_UP, 0);
    CHECK(!strcmp(input(ui), "hi Ann"));
    iw_win_key(win, OLUI_KEY_UP, 0);
    CHECK(!strcmp(input(ui), "hi Ann"));
    iw_win_key(win, OLUI_KEY_DOWN, 0);
    CHECK(!strcmp(input(ui), "/topic"));
    iw_win_key(win, OLUI_KEY_DOWN, 0);
    CHECK(!strcmp(input(ui), ""));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));

    /* Tab completes from the nick list: ": " at the start, again for the next one */
    type("an");
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "anders: "));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "Ann: "));
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "anders: "));
    /* typing on starts again; in the line, a space after */
    type("ask c");
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "anders: ask Carl "));
    /* in the middle of the line, at the caret */
    olui_set_text(ui, olui_find(ui, "input"), "z is here");
    olui_set_selection(ui, olui_find(ui, "input"), 1, 1);
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "zed: is here"));
    {
        size_t from = 0, to = 0;
        olui_get_selection(olui_find(ui, "input"), &from, &to);
        CHECK_EQ(to, 5);
    }
    /* nothing matches: the line stays, the focus too */
    olui_set_text(ui, olui_find(ui, "input"), "qq");
    iw_win_key(win, OLUI_KEY_TAB, 0);
    CHECK(!strcmp(input(ui), "qq"));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));
    olui_set_text(ui, olui_find(ui, "input"), "");

    /* the server's buffer has its own history */
    click_row(ui, "buffers", 0, OLUI_MOUSE_DOWN);
    CHECK(!strcmp(seen.buffer, "irc.libera.chat"));
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "nicks")), 0);   /* no nick list for a server */
    iw_win_key(win, OLUI_KEY_UP, 0);
    CHECK(!strcmp(input(ui), ""));
    type("/join #amigachrome");
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK(!strcmp(seen.buffer, "irc.libera.chat") && !strcmp(seen.text, "/join #amigachrome"));
    click_row(ui, "buffers", 1, OLUI_MOUSE_DOWN);
    iw_win_key(win, OLUI_KEY_UP, 0);
    CHECK(!strcmp(input(ui), "/topic"));
    iw_win_key(win, OLUI_KEY_DOWN, 0);

    /* double-clicking a nick opens a query with it, shown, under the server */
    click_row(ui, "nicks", 1, OLUI_MOUSE_DOUBLE);
    CHECK(!strcmp(seen.buffer, "Ann"));
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 3);
    CHECK(was_drawn(ui, "  Ann"));
    CHECK(olui_vlist_selected(olui_find(ui, "buffers"), 2) && !olui_vlist_selected(olui_find(ui, "buffers"), 1));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));
    type("hello");
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK(!strcmp(seen.buffer, "Ann") && !strcmp(seen.text, "hello"));
    CHECK(ends_with(olui_get_text(olui_find(ui, "log")), "<me> hello"));

    /* the keyboard way: a nick chosen in the list, then Return, opens its query too */
    click_row(ui, "buffers", 1, OLUI_MOUSE_DOWN);
    CHECK(strcmp(seen.buffer, "Ann") != 0);
    click_row(ui, "nicks", 1, OLUI_MOUSE_DOWN);
    CHECK(olui_focused(ui) == olui_find(ui, "nicks"));
    iw_win_key(win, OLUI_KEY_RETURN, 0);
    CHECK(!strcmp(seen.buffer, "Ann"));                /* Ann's query, already open, is shown */
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 3);
    CHECK(olui_vlist_selected(olui_find(ui, "buffers"), 2));
    CHECK(olui_focused(ui) == olui_find(ui, "input"));

    /* a second server's buffers follow the first's */
    iu_set_nick(other, "me");
    CHECK(iw_win_add(win, other));
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 4);
    CHECK(iw_win_shown(win, NULL) == libera);      /* what was shown stays */
    click_row(ui, "buffers", 3, OLUI_MOUSE_DOWN);
    CHECK(iw_win_shown(win, NULL) == other && seen.s == other);
    CHECK(!strcmp(seen.buffer, "irc.other.example"));
    /* new lines in a buffer not shown: counted in the list */
    event(libera, IR_EV_MESSAGE, "Ann", "Ann", "you there?", NULL, 0);
    iw_win_refresh(win);
    CHECK(was_drawn(ui, "  Ann (1!)") || was_drawn(ui, "  Ann (1)"));

    /* the log stays at its end as lines come, unless scrolled back */
    click_row(ui, "buffers", 1, OLUI_MOUSE_DOWN);
    for (i = 0; i < 80; i++) {
        char t[32];
        snprintf(t, sizeof(t), "line %d", i);
        event(libera, IR_EV_MESSAGE, "#amiga", "zed", t, NULL, 0);
        iw_win_refresh(win);
    }
    {
        olui_el *log = olui_find(ui, "log");
        int32_t end = olui_get_scroll(log);
        CHECK(end > 0);
        CHECK(was_drawn(ui, "[20:59] <zed> line 79"));
        olui_set_scroll(ui, log, 0);
        event(libera, IR_EV_MESSAGE, "#amiga", "zed", "line 80", NULL, 0);
        iw_win_refresh(win);
        CHECK_EQ(olui_get_scroll(log), 0);
        CHECK(ends_with(olui_get_text(log), "<zed> line 80"));
        olui_set_scroll(ui, log, INT32_MAX);
        event(libera, IR_EV_MESSAGE, "#amiga", "zed", "line 81", NULL, 0);
        iw_win_refresh(win);
        CHECK(olui_get_scroll(log) > end);
        CHECK(was_drawn(ui, "[20:59] <zed> line 81"));
    }

    /* Close: the program parts and closes the buffer; the server's is shown */
    click(ui, "close");
    CHECK_EQ(seen.closes, 1);
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 3);
    CHECK(iw_win_shown(win, NULL) == libera);
    CHECK(!strcmp(seen.buffer, "irc.libera.chat"));
    CHECK(olui_vlist_selected(olui_find(ui, "buffers"), 0));
    for (i = 1; i < 3; i++) CHECK(!olui_vlist_selected(olui_find(ui, "buffers"), i));
    CHECK(was_drawn(ui, "  Ann (1!)") || was_drawn(ui, "  Ann (1)"));
    CHECK(!was_drawn(ui, "  #amiga"));

    /* the server taken out (the program frees it after): the next one shows */
    iw_win_remove(win, libera);
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 1);
    CHECK(iw_win_shown(win, NULL) == other);
    iu_free(libera);

    /* Connect: the form checked, no password asked, then the chat tab */
    memset(&c, 0, sizeof(c));
    strcpy(c.host, "irc.libera.chat"); strcpy(c.nick, "me"); c.tls = 1;
    strcpy(c.channels, "#amiga,#amigachrome");
    iw_win_set_connect(win, &c);
    CHECK(olui_find(ui, "cpassword") == NULL);
    olui_tabs_select(ui, olui_find(ui, "tabs"), IW_TAB_CONNECT);
    CHECK(olui_layout(ui, 800, 600));
    CHECK(shows(ui, "The password is kept in the key store, not here."));
    olui_set_text(ui, olui_find(ui, "cnick"), "");
    click(ui, "connect");
    CHECK_EQ(seen.connects, 0);
    CHECK(!strcmp(olui_get_text(olui_find(ui, "cwhy")), "Give a nick (no spaces)."));
    olui_set_text(ui, olui_find(ui, "cnick"), "me");
    olui_set_text(ui, olui_find(ui, "caccount"), "meacct");
    click(ui, "connect");
    CHECK_EQ(seen.connects, 1);
    CHECK(!strcmp(seen.c.host, "irc.libera.chat") && seen.c.port == 6697 && seen.c.tls == 1);
    CHECK(!strcmp(seen.c.nick, "me") && !strcmp(seen.c.account, "meacct"));
    CHECK(!strcmp(seen.c.channels, "#amiga,#amigachrome"));
    CHECK_EQ(olui_tabs_current(olui_find(ui, "tabs")), IW_TAB_CHAT);
    CHECK(!strcmp(olui_get_text(olui_find(ui, "cwhy")), ""));
    /* without TLS and no port given: 6667 */
    olui_tabs_select(ui, olui_find(ui, "tabs"), IW_TAB_CONNECT);
    CHECK(olui_layout(ui, 800, 600));
    olui_set_checked(ui, olui_find(ui, "ctls"), 0);
    olui_set_text(ui, olui_find(ui, "cport"), "");
    click(ui, "connect");
    CHECK(seen.connects == 2 && seen.c.port == 6667 && seen.c.tls == 0);

    /* keys outside the input line go on to olui: Tab moves the focus */
    olui_focus(ui, olui_find(ui, "buffers"));
    CHECK(iw_win_key(win, OLUI_KEY_TAB, 0));
    CHECK(olui_focused(ui) != olui_find(ui, "buffers"));

    iw_win_status(win, "Connected to irc.other.example");
    CHECK(shows(ui, "Connected to irc.other.example"));

    iw_win_remove(win, other);
    CHECK_EQ(olui_vlist_rows(olui_find(ui, "buffers")), 0);
    CHECK(iw_win_shown(win, NULL) == NULL);
    iw_win_key(win, OLUI_KEY_RETURN, 0);           /* nothing shown: nothing sent */
    iw_win_free(win);
    iu_free(other);
    iu_nicks_free(&members);
    olui_free(ui);
    TEST_DONE();
}
