/* Message parsing, tags, case mapping, ISUPPORT, base64 and line splitting.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <stdlib.h>
#include "gfopenirc/irc.h"
#include "nk_test.h"

#define P(m, s) ir_parse(m, s, strlen(s))

int main(void)
{
    static IrMsg m;
    IrSupport s;
    char b[64];

    /* full form */
    CHECK_EQ(P(&m, "@time=2026-11-13T19:00:00.000Z;msgid=abc :dale!d@amiga.example PRIVMSG #amiga :hello there\r\n"), NK_OK);
    CHECK(!strcmp(m.nick, "dale") && !strcmp(m.user, "d") && !strcmp(m.host, "amiga.example"));
    CHECK(!strcmp(m.cmd, "PRIVMSG"));
    CHECK_EQ(m.nparams, 2);
    CHECK(!strcmp(m.params[0], "#amiga") && !strcmp(m.params[1], "hello there"));
    CHECK(!strcmp(ir_tag(&m, "time"), "2026-11-13T19:00:00.000Z"));
    CHECK(!strcmp(ir_tag(&m, "msgid"), "abc"));
    CHECK(ir_tag(&m, "nope") == 0);
    /* no prefix, lower-case command, no trailing */
    CHECK_EQ(P(&m, "ping tok123"), NK_OK);
    CHECK(m.nick == 0 && !strcmp(m.cmd, "PING") && m.nparams == 1 && !strcmp(m.params[0], "tok123"));
    /* server prefix, numeric, empty trailing, extra spaces */
    CHECK_EQ(P(&m, ":irc.example  001  krissie  :"), NK_OK);
    CHECK(!strcmp(m.nick, "irc.example") && m.user == 0 && m.host == 0);
    CHECK(!strcmp(m.cmd, "001") && m.nparams == 2 && !strcmp(m.params[1], ""));
    /* trailing with colons and leading spaces kept */
    CHECK_EQ(P(&m, ":a!b@c PRIVMSG x ::-)  hi"), NK_OK);
    CHECK(!strcmp(m.params[1], ":-)  hi"));
    /* tag escapes and value-less tags */
    CHECK_EQ(P(&m, "@a=x\\:y\\sz\\\\w\\r\\n;b;c=;d=e\\ :n PING x"), NK_OK);
    CHECK(!strcmp(ir_tag(&m, "a"), "x;y z\\w"));     /* \\r \\n dropped */
    CHECK(!strcmp(ir_tag(&m, "b"), "") && !strcmp(ir_tag(&m, "c"), ""));
    CHECK(!strcmp(ir_tag(&m, "d"), "e"));
    /* more than 15 parameters: the rest folded into the last */
    CHECK_EQ(P(&m, "X 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17"), NK_OK);
    CHECK_EQ(m.nparams, 15);
    CHECK(!strcmp(m.params[14], "15 16 17"));
    /* broken lines */
    CHECK_EQ(P(&m, ""), NK_EPROTO);
    CHECK_EQ(P(&m, ":onlyprefix"), NK_EPROTO);
    CHECK_EQ(P(&m, "@tagsonly"), NK_EPROTO);
    CHECK_EQ(P(&m, ":p   "), NK_EPROTO);
    {
        char *big = (char *)malloc(IR_LINE_MAX + 10);
        memset(big, 'A', IR_LINE_MAX + 9);
        CHECK_EQ(ir_parse(&m, big, IR_LINE_MAX + 9), NK_EPROTO);
        free(big);
    }
    /* a NUL inside ends the line */
    CHECK_EQ(ir_parse(&m, "PING a\0b", 8), NK_OK);
    CHECK(!strcmp(m.params[0], "a"));

    /* case mapping */
    CHECK_EQ(ir_casecmp(IR_CM_RFC1459, "Dale[a]^", "dale{a}~"), 0);
    CHECK(ir_casecmp(IR_CM_STRICT_RFC1459, "a^", "A~") != 0);
    CHECK_EQ(ir_casecmp(IR_CM_STRICT_RFC1459, "[x]", "{X}"), 0);
    CHECK(ir_casecmp(IR_CM_ASCII, "[x]", "{x}") != 0);
    CHECK_EQ(ir_casecmp(IR_CM_ASCII, "KrIsSiE", "krissie"), 0);
    strcpy(b, "Nick[1]^");
    ir_casefold(IR_CM_RFC1459, b);
    CHECK(!strcmp(b, "nick{1}~"));

    /* ISUPPORT */
    ir_isupport_init(&s);
    CHECK(ir_is_channel(&s, "#a") && ir_is_channel(&s, "&a") && !ir_is_channel(&s, "dale"));
    CHECK_EQ(P(&m, ":srv 005 me CHANTYPES=#! PREFIX=(qaohv)~&@%+ CHANMODES=beI,k,l,imnst "
                   "CASEMAPPING=ascii NICKLEN=30 NETWORK=Libera.Chat -FOO :are supported"), NK_OK);
    ir_isupport_apply(&s, &m);
    CHECK(!strcmp(s.chantypes, "#!") && !ir_is_channel(&s, "&a"));
    CHECK(!strcmp(s.prefix_modes, "qaohv") && !strcmp(s.prefix_chars, "~&@%+"));
    CHECK_EQ(s.casemap, IR_CM_ASCII);
    CHECK_EQ(s.nicklen, 30);
    CHECK(!strcmp(s.network, "Libera.Chat"));
    CHECK(ir_mode_has_arg(&s, 'o', 0) && ir_mode_has_arg(&s, 'b', 1) && ir_mode_has_arg(&s, 'k', 0));
    CHECK(ir_mode_has_arg(&s, 'l', 1) && !ir_mode_has_arg(&s, 'l', 0) && !ir_mode_has_arg(&s, 'n', 1));
    /* a mismatched PREFIX is ignored; short CHANMODES clears the rest */
    CHECK_EQ(P(&m, ":srv 005 me PREFIX=(ov)@ CHANMODES=b :are supported"), NK_OK);
    ir_isupport_apply(&s, &m);
    CHECK(!strcmp(s.prefix_modes, "qaohv"));
    CHECK(!strcmp(s.chanmodes[0], "b") && !s.chanmodes[1][0] && !s.chanmodes[3][0]);

    /* base64 */
    CHECK_EQ(ir_b64("", 0, b, sizeof(b)), 0);
    b[ir_b64("f", 1, b, sizeof(b))] = 0;
    CHECK(!strcmp(b, "Zg=="));
    b[ir_b64("fo", 2, b, sizeof(b))] = 0;
    CHECK(!strcmp(b, "Zm8="));
    b[ir_b64("foobar", 6, b, sizeof(b))] = 0;
    CHECK(!strcmp(b, "Zm9vYmFy"));
    b[ir_b64("dale\0dale\0s3cret", 16, b, sizeof(b))] = 0;
    CHECK(!strcmp(b, "ZGFsZQBkYWxlAHMzY3JldA=="));
    CHECK_EQ(ir_b64("foobar", 6, b, 7), 0);

    /* splitting: at a space, never inside UTF-8 */
    CHECK_EQ(ir_split_at("hello", 5, 10), 5);
    CHECK_EQ(ir_split_at("hello world again", 17, 13), 12);     /* "hello world " */
    CHECK_EQ(ir_split_at("abcdefghij", 10, 4), 4);
    CHECK_EQ(ir_split_at("ab\xc3\xa9" "cd", 6, 3), 2);           /* not inside é */
    CHECK_EQ(ir_split_at("\xe2\x82\xac\xe2\x82\xac", 6, 4), 3);  /* one euro sign */
    TEST_DONE();
}
