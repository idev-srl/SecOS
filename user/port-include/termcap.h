/* SecOS - <termcap.h> for source ports: the built-in console entry in
 * user/termcap.c (no termcap/terminfo database on SecOS).
 * SPDX-License-Identifier: MIT */
#ifndef _PORT_TERMCAP_H
#define _PORT_TERMCAP_H

extern char PC;
extern char* UP;
extern char* BC;
extern short ospeed;

int   tgetent(char* bp, const char* name);
int   tgetflag(const char* id);
int   tgetnum(const char* id);
char* tgetstr(const char* id, char** area);
char* tgoto(const char* cap, int col, int row);
int   tputs(const char* str, int affcnt, int (*putc_fn)(int));

#endif
