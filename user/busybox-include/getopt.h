/* SecOS BusyBox overlay: <getopt.h> (musl getopt/getopt_long, compiled by
 * tools/port-busybox.sh). SPDX-License-Identifier: MIT */
#ifndef _BB_GETOPT_H
#define _BB_GETOPT_H
extern char* optarg;
extern int optind, opterr, optopt, optreset;
struct option { const char* name; int has_arg; int* flag; int val; };
#define no_argument       0
#define required_argument 1
#define optional_argument 2
int getopt(int argc, char* const argv[], const char* optstring);
int getopt_long(int argc, char* const* argv, const char* opts, const struct option* lo, int* idx);
int getopt_long_only(int argc, char* const* argv, const char* opts, const struct option* lo, int* idx);
#endif
