/* GFOpenIRC chat window, drawn through GFOpenLayout (gfopenlayout_ui.h):
 * the buffers (each server with its channels and queries under it) in a
 * list on the left that works as the tabs, the topic over the log, the nick
 * list on the right, the input line under them, and a Connect tab. It never
 * calls graphics or layers: olui lays it out and the Amiga glue draws it.
 *
 * The window shows the models (ui_model.h) the program owns and tells the
 * program what was asked through IwWinOps. Plain C99: the host tests drive
 * it with olui's own input calls.
 *
 * Keys go through iw_win_key, not olui_key: in the input line Up and Down
 * walk the buffer's history and Tab completes a nick; every other key goes
 * on to olui.
 *
 * The Connect tab asks for no password: the program asks gfopenkeys.library
 * for it when it connects (ui_model.h).
 *
 * MIT licence, (c) 2026 AmiCradle Team. */
#ifndef GFOPENIRC_UI_WINDOW_H
#define GFOPENIRC_UI_WINDOW_H

#include <stddef.h>
#include "gfopenlayout_ui.h"
#include "gfopenirc/ui_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tabs, from 0 */
enum { IW_TAB_CHAT = 0, IW_TAB_CONNECT };

#define IW_MAX_SERVERS 8

typedef struct IwConnect {
    char host[256];
    int  port;                   /* 6697 with TLS, 6667 without, when left empty */
    int  tls;
    char nick[64];
    char account[64];            /* SASL account; "" for none */
    char channels[256];          /* joined once registered: "#amiga,#amigachrome" */
} IwConnect;

typedef struct IwWinOps {
    /* Return in the input line: text as typed ("/join #x" or what to say),
     * for buffer of s. The program sends it (ir_command) and, for a
     * message, adds it with iu_own. */
    void (*command)(void *user, IuState *s, size_t buffer, const char *text);
    /* A buffer is shown (chosen in the list, or a query opened): the
     * program gives the window its nick list with iw_win_nicks. */
    void (*shown)(void *user, IuState *s, size_t buffer);
    /* Close was pressed on a buffer: the program parts the channel (or, for
     * the server, quits) and closes it with iu_close or iw_win_remove. */
    void (*close)(void *user, IuState *s, size_t buffer);
    /* Connect was pressed with the form filled (host and nick given). */
    void (*connect)(void *user, const IwConnect *c);
} IwWinOps;

typedef struct IwWin IwWin;

/* Loads the window's contents into ui (a new olui; the theme CSS goes in
 * first, as for any window). NULL with why in err. */
IwWin *iw_win_new(olui *ui, const IwWinOps *ops, void *user, char *err, size_t cap);
void   iw_win_free(IwWin *w);

/* A server's buffers, shown in the list (the program keeps s). 0 when the
 * window has IW_MAX_SERVERS already. The first one added is shown. */
int    iw_win_add(IwWin *w, IuState *s);
/* Takes a server's buffers out of the list (before the program frees s). */
void   iw_win_remove(IwWin *w, IuState *s);
/* The models changed (lines came, buffers opened or closed): the list, the
 * topic and the log are shown again. The log stays at its end unless it was
 * scrolled back. */
void   iw_win_refresh(IwWin *w);
/* Shows buffer of s (iu_show), and tells the program (shown). */
void   iw_win_show(IwWin *w, IuState *s, size_t buffer);
/* The shown buffer: its server and index. NULL when none is. */
IuState *iw_win_shown(const IwWin *w, size_t *buffer);
/* The nick list of the shown buffer (the program keeps it; NULL: none). It
 * is what Tab completes from. */
void   iw_win_nicks(IwWin *w, const IuNicks *nicks, int casemap);
/* The line under the window: connection, lag, errors. */
void   iw_win_status(IwWin *w, const char *text);
/* Fills the Connect tab (from the program's settings). */
void   iw_win_set_connect(IwWin *w, const IwConnect *c);

/* A key for the window (OLUI_KEY_... or a character, OLUI_QUAL_...). 1 when
 * something took it. */
int    iw_win_key(IwWin *w, unsigned code, unsigned qualifiers);

#ifdef __cplusplus
}
#endif
#endif
