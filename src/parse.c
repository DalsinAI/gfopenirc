/* IRC message parsing, case mapping, ISUPPORT and small helpers.
 * MIT licence, (c) 2026 AmiCradle Team. */
#include <stdlib.h>
#include <string.h>
#include "gfopenirc/irc.h"

static void unescape_tag(char *s)
{
    char *o = s;
    while (*s) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s) {
        case ':': *o++ = ';'; break;
        case 's': *o++ = ' '; break;
        case '\\': *o++ = '\\'; break;
        case 'r': case 'n': break;   /* no CR/LF in a value: it would break
                                        the one-line-per-event output */
        case 0: *o = 0; return;      /* a lone backslash at the end is dropped */
        default: *o++ = *s; break;
        }
        s++;
    }
    *o = 0;
}

int ir_parse(IrMsg *m, const char *line, size_t len)
{
    char *p, *e;
    memset(m, 0, offsetof(IrMsg, buf));
    if (len > IR_LINE_MAX)
        return NK_EPROTO;
    memcpy(m->buf, line, len);
    m->buf[len] = 0;
    /* a NUL inside ends the line; trailing CR/LF are not ours to keep */
    len = strlen(m->buf);
    while (len && (m->buf[len - 1] == '\r' || m->buf[len - 1] == '\n'))
        m->buf[--len] = 0;
    p = m->buf;
    if (*p == '@') {
        char *tags = ++p;
        p = strchr(p, ' ');
        if (!p) return NK_EPROTO;
        *p++ = 0;
        while (tags && *tags) {
            char *next = strchr(tags, ';'), *eq;
            if (next) *next++ = 0;
            eq = strchr(tags, '=');
            if (eq) {
                *eq++ = 0;
                unescape_tag(eq);
            }
            if (*tags && m->ntags < IR_MAX_TAGS) {
                m->tag_key[m->ntags] = tags;
                m->tag_val[m->ntags++] = eq ? eq : "";
            }
            tags = next;
        }
    }
    while (*p == ' ') p++;
    if (*p == ':') {
        char *pre = ++p, *bang, *at;
        p = strchr(p, ' ');
        if (!p) return NK_EPROTO;
        *p++ = 0;
        at = strchr(pre, '@');
        if (at) { *at = 0; m->host = at + 1; }
        bang = strchr(pre, '!');
        if (bang) { *bang = 0; m->user = bang + 1; }
        m->nick = pre;
    }
    while (*p == ' ') p++;
    if (!*p) return NK_EPROTO;
    m->cmd = p;
    e = strchr(p, ' ');
    if (e) *e++ = 0;
    for (; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    p = e;
    while (p && *p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == ':' || m->nparams == IR_MAX_PARAMS - 1) {
            m->params[m->nparams++] = *p == ':' ? p + 1 : p;
            break;
        }
        m->params[m->nparams++] = p;
        e = strchr(p, ' ');
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    return NK_OK;
}

const char *ir_tag(const IrMsg *m, const char *key)
{
    int i;
    for (i = 0; i < m->ntags; i++)
        if (!strcmp(m->tag_key[i], key))
            return m->tag_val[i];
    return 0;
}

/* ---- case mapping ------------------------------------------------------------ */
static int fold(int cm, int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch + 32;
    if (cm == IR_CM_ASCII) return ch;
    if (ch >= '[' && ch <= ']') return ch + 32;      /* [\] -> {|} */
    if (ch == '^' && cm == IR_CM_RFC1459) return '~';
    return ch;
}

int ir_casecmp(int cm, const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = fold(cm, (unsigned char)*a), y = fold(cm, (unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
}

void ir_casefold(int cm, char *s)
{
    for (; *s; s++)
        *s = (char)fold(cm, (unsigned char)*s);
}

/* ---- ISUPPORT -------------------------------------------------------------------- */
static void copy(char *d, size_t cap, const char *s, size_t n)
{
    if (n >= cap) n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}

void ir_isupport_init(IrSupport *s)
{
    memset(s, 0, sizeof(*s));
    strcpy(s->chantypes, "#&");
    strcpy(s->prefix_modes, "ov");
    strcpy(s->prefix_chars, "@+");
    strcpy(s->chanmodes[0], "beI");
    strcpy(s->chanmodes[1], "k");
    strcpy(s->chanmodes[2], "l");
    strcpy(s->chanmodes[3], "imnpst");
    s->casemap = IR_CM_RFC1459;
    s->nicklen = 9;
}

void ir_isupport_apply(IrSupport *s, const IrMsg *m)
{
    int i;
    /* 005 me TOKEN TOKEN ... :are supported */
    for (i = 1; i < m->nparams - 1; i++) {
        const char *t = m->params[i], *v = strchr(t, '=');
        size_t kl = v ? (size_t)(v - t) : strlen(t);
        if (v) v++;
        if (*t == '-') continue;                      /* withdrawn: keep defaults */
        if (kl == 9 && !strncmp(t, "CHANTYPES", 9) && v) {
            copy(s->chantypes, sizeof(s->chantypes), v, strlen(v));
        } else if (kl == 6 && !strncmp(t, "PREFIX", 6) && v) {
            const char *close = strchr(v, ')');
            if (*v == '(' && close) {
                size_t nm = (size_t)(close - v - 1), nc = strlen(close + 1);
                if (nm == nc) {
                    copy(s->prefix_modes, sizeof(s->prefix_modes), v + 1, nm);
                    copy(s->prefix_chars, sizeof(s->prefix_chars), close + 1, nc);
                }
            } else if (!*v) {
                s->prefix_modes[0] = s->prefix_chars[0] = 0;
            }
        } else if (kl == 9 && !strncmp(t, "CHANMODES", 9) && v) {
            int k;
            for (k = 0; k < 4; k++) {
                const char *c = strchr(v, ',');
                size_t n = c ? (size_t)(c - v) : strlen(v);
                copy(s->chanmodes[k], sizeof(s->chanmodes[k]), v, n);
                if (!c) { for (k++; k < 4; k++) s->chanmodes[k][0] = 0; break; }
                v = c + 1;
            }
        } else if (kl == 11 && !strncmp(t, "CASEMAPPING", 11) && v) {
            s->casemap = !strcmp(v, "ascii") ? IR_CM_ASCII :
                         !strcmp(v, "strict-rfc1459") ? IR_CM_STRICT_RFC1459 : IR_CM_RFC1459;
        } else if (kl == 7 && !strncmp(t, "NICKLEN", 7) && v) {
            int n = atoi(v);
            if (n > 0 && n < 64) s->nicklen = n;
        } else if (kl == 7 && !strncmp(t, "NETWORK", 7) && v) {
            copy(s->network, sizeof(s->network), v, strlen(v));
        }
    }
}

int ir_is_channel(const IrSupport *s, const char *name)
{
    return name && *name && strchr(s->chantypes, *name) != 0;
}

int ir_mode_has_arg(const IrSupport *s, char m, int set)
{
    if (strchr(s->prefix_modes, m)) return 1;
    if (strchr(s->chanmodes[0], m) || strchr(s->chanmodes[1], m)) return 1;
    if (strchr(s->chanmodes[2], m)) return set;
    return 0;
}

/* ---- helpers -------------------------------------------------------------------- */
size_t ir_b64(const void *in, size_t n, char *out, size_t cap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0, i;
    if ((n + 2) / 3 * 4 > cap)
        return 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out[o++] = T[v >> 18]; out[o++] = T[v >> 12 & 63];
        out[o++] = T[v >> 6 & 63]; out[o++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0);
        out[o++] = T[v >> 18]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = '=';
    }
    return o;
}

size_t ir_split_at(const char *text, size_t len, size_t room)
{
    size_t cut, k;
    if (len <= room)
        return len;
    cut = room;
    /* not inside a UTF-8 sequence: back off continuation bytes */
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
        cut--;
    /* prefer the last space in the later half */
    for (k = cut; k > room / 2; k--)
        if (text[k - 1] == ' ')
            return k;
    return cut ? cut : room;
}

/* the first n bytes of s are `w` (lower case), in any case */
static int word_is(const char *s, const char *w, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (fold(IR_CM_ASCII, (unsigned char)s[i]) != w[i])
            return 0;
    return 1;
}

int ir_input_target(const char *input, char *current, size_t cap)
{
    const char *to;
    size_t tl, k;
    if (!input || !current || !cap)
        return 0;
    if (input[0] != '/')
        return 0;
    for (k = 1; input[k] && input[k] != ' '; k++)
        ;
    if (!(k == 4 && word_is(input, "/msg", 4)) && !(k == 6 && word_is(input, "/query", 6)))
        return 0;
    to = input + k;
    while (*to == ' ')
        to++;
    tl = strcspn(to, " ");
    if (!tl || tl >= cap)
        return 0;
    memcpy(current, to, tl);
    current[tl] = 0;
    return 1;
}
