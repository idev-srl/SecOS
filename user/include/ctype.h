/* <ctype.h> — SecOS libc. SPDX-License-Identifier: MIT */
#ifndef _CTYPE_H
#define _CTYPE_H
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int isgraph(int c);
int ispunct(int c);
int iscntrl(int c);
int isblank(int c);
int toupper(int c);
int tolower(int c);
#endif
