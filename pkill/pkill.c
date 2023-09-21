/*	$NetBSD: pkill.c,v 1.16 2005/10/10 22:13:20 kleink Exp $	*/

/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * Copyright (c) 2005 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Andrew Doran.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/user.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <paths.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <ctype.h>
#include <fcntl.h>
#ifndef __APPLE__
#include <kvm.h>
#endif
#include <err.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <locale.h>
#ifndef __APPLE__
#include <jail.h>
#endif

#ifdef __APPLE__
#include <xpc/xpc.h>
#include <sys/proc_info.h>
#include <os/assumes.h>
#include <sysmon.h>
#endif

#define	STATUS_MATCH	0
#define	STATUS_NOMATCH	1
#define	STATUS_BADUSAGE	2
#define	STATUS_ERROR	3

#define	MIN_PID	5
#define	MAX_PID	99999

#ifdef __APPLE__
/* Ignore system processes and myself. */
#define	PSKIP(kp)	((pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)) == mypid ||			\
			 ((xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_FLAGS)) & PROC_FLAG_SYSTEM) != 0))
#else
/* Ignore system-processes (if '-S' flag is not specified) and myself. */
#define	PSKIP(kp)	((kp)->ki_pid == mypid ||			\
			 (!kthreads && ((kp)->ki_flag & P_KPROC) != 0))
#endif

enum listtype {
	LT_GENERIC,
	LT_USER,
	LT_GROUP,
	LT_TTY,
	LT_PGRP,
#ifndef __APPLE__
	LT_JAIL,
	LT_SID,
	LT_CLASS
#endif
};

struct list {
	SLIST_ENTRY(list) li_chain;
	long	li_number;
	char	*li_name;
};

SLIST_HEAD(listhead, list);

#ifdef __APPLE__
static sysmon_table_t plist;
#else
static struct kinfo_proc *plist;
#endif
static char	*selected;
static const char *delim = "\n";
static int	nproc;
static int	pgrep;
static int	signum = SIGTERM;
static int	newest;
static int	oldest;
static int	interactive;
static int	inverse;
static int	longfmt;
static int	matchargs;
static int	fullmatch;
#ifndef __APPLE__
static int	kthreads;
#endif
static int	cflags = REG_EXTENDED;
static int	quiet;
#ifndef __APPLE__
static kvm_t	*kd;
#endif
static pid_t	mypid;

static struct listhead euidlist = SLIST_HEAD_INITIALIZER(euidlist);
static struct listhead ruidlist = SLIST_HEAD_INITIALIZER(ruidlist);
static struct listhead rgidlist = SLIST_HEAD_INITIALIZER(rgidlist);
static struct listhead pgrplist = SLIST_HEAD_INITIALIZER(pgrplist);
static struct listhead ppidlist = SLIST_HEAD_INITIALIZER(ppidlist);
static struct listhead tdevlist = SLIST_HEAD_INITIALIZER(tdevlist);
#ifndef __APPLE__
static struct listhead sidlist = SLIST_HEAD_INITIALIZER(sidlist);
static struct listhead jidlist = SLIST_HEAD_INITIALIZER(jidlist);
static struct listhead classlist = SLIST_HEAD_INITIALIZER(classlist);
#endif

static void	usage(void) __attribute__((__noreturn__));
#ifdef __APPLE__
static int	killact(const sysmon_row_t);
static int	grepact(const sysmon_row_t);
#else
static int	killact(const struct kinfo_proc *);
static int	grepact(const struct kinfo_proc *);
#endif
static void	makelist(struct listhead *, enum listtype, char *);
static int	takepid(const char *, int);

#ifdef __APPLE__
static sysmon_table_t
copy_process_info(void)
{
	dispatch_semaphore_t sema;
	sysmon_request_t request;
	__block sysmon_table_t result = NULL;

	sema = dispatch_semaphore_create(0);
	request = sysmon_request_create_with_error(SYSMON_REQUEST_TYPE_PROCESS, ^(sysmon_table_t table, const char *error_str) {
		if (table) {
			result = sysmon_retain(table);
		} else {
			fprintf(stderr, "sysmon request failed with error: %s\n", error_str);
		}
		dispatch_semaphore_signal(sema);
	});
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_PID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_FLAGS);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_UID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_COMM);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_ARGUMENTS);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_RUID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_RGID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_PPID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_PGID);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_TDEV);
	sysmon_request_add_attribute(request, SYSMON_ATTR_PROC_START);
	sysmon_request_execute(request);
	dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
	dispatch_release(sema);
	sysmon_release(request);

	return result;
}
#endif /* __APPLE__ */

int
main(int argc, char **argv)
{
#ifdef __APPLE__
	char *buf, *bufp, *mstr, *p, *q, *pidfile;
	xpc_object_t pargv;
#else
	char *buf, *mstr, **pargv, *p, *q, *pidfile;
	const char *execf, *coref;
#endif
	size_t bufsz;
	int ancestors, debug_opt, did_action;
	int i, ch, bestidx, rv, criteria, pidfromfile, pidfilelock;
#ifdef __APPLE__
	__block size_t jsz;
	int (*action)(const sysmon_row_t);
	sysmon_row_t kp;
#else
	size_t jsz;
	int (*action)(const struct kinfo_proc *);
	struct kinfo_proc *kp;
#endif
	struct list *li;
#ifdef __APPLE__
	int64_t best_tval;
#else
	struct timeval best_tval;
#endif
	regex_t reg;
	regmatch_t regmatch;
	pid_t pid;

	setlocale(LC_ALL, "");

	if (strcmp(getprogname(), "pgrep") == 0) {
		action = grepact;
		pgrep = 1;
	} else {
		action = killact;
		p = argv[1];

		if (argc > 1 && p[0] == '-') {
			p++;
			i = (int)strtol(p, &q, 10);
			if (*q == '\0') {
				signum = i;
				argv++;
				argc--;
			} else {
				if (strncasecmp(p, "SIG", 3) == 0)
					p += 3;
				for (i = 1; i < NSIG; i++)
					if (strcasecmp(sys_signame[i], p) == 0)
						break;
				if (i != NSIG) {
					signum = i;
					argv++;
					argc--;
				}
			}
		}
	}

	buf = NULL;
	ancestors = 0;
	criteria = 0;
	debug_opt = 0;
	pidfile = NULL;
	pidfilelock = 0;
	quiet = 0;
#ifndef __APPLE__
	execf = NULL;
	coref = _PATH_DEVNULL;
#endif

#ifdef __APPLE__
	while ((ch = getopt(argc, argv, "DF:G:ILP:U:ac:d:fg:ilnoqt:u:vx")) != -1)
#else
	while ((ch = getopt(argc, argv, "DF:G:ILM:N:P:SU:ac:d:fg:ij:lnoqs:t:u:vx")) != -1)
#endif
		switch (ch) {
		case 'D':
			debug_opt++;
			break;
		case 'F':
			pidfile = optarg;
			criteria = 1;
			break;
		case 'G':
			makelist(&rgidlist, LT_GROUP, optarg);
			criteria = 1;
			break;
		case 'I':
			if (pgrep)
				usage();
			interactive = 1;
			break;
		case 'L':
			pidfilelock = 1;
			break;
#ifndef __APPLE__
		case 'M':
			coref = optarg;
			break;
		case 'N':
			execf = optarg;
			break;
#endif
		case 'P':
			makelist(&ppidlist, LT_GENERIC, optarg);
			criteria = 1;
			break;
#ifndef __APPLE__
		case 'S':
			if (!pgrep)
				usage();
			kthreads = 1;
			break;
#endif
		case 'U':
			makelist(&ruidlist, LT_USER, optarg);
			criteria = 1;
			break;
		case 'a':
			ancestors++;
			break;
#ifndef __APPLE__
		case 'c':
			makelist(&classlist, LT_CLASS, optarg);
			criteria = 1;
			break;
#endif
		case 'd':
			if (!pgrep)
				usage();
			delim = optarg;
			break;
		case 'f':
			matchargs = 1;
			break;
		case 'g':
			makelist(&pgrplist, LT_PGRP, optarg);
			criteria = 1;
			break;
		case 'i':
			cflags |= REG_ICASE;
			break;
#ifndef __APPLE__
		case 'j':
			makelist(&jidlist, LT_JAIL, optarg);
			criteria = 1;
			break;
#endif
		case 'l':
			longfmt = 1;
			break;
		case 'n':
			newest = 1;
			criteria = 1;
			break;
		case 'o':
			oldest = 1;
			criteria = 1;
			break;
		case 'q':
			if (!pgrep)
				usage();
			quiet = 1;
			break;
#ifndef __APPLE__
		case 's':
			makelist(&sidlist, LT_SID, optarg);
			criteria = 1;
			break;
#endif /* !__APPLE__ */
		case 't':
			makelist(&tdevlist, LT_TTY, optarg);
			criteria = 1;
			break;
		case 'u':
			makelist(&euidlist, LT_USER, optarg);
			criteria = 1;
			break;
		case 'v':
			inverse = 1;
			break;
		case 'x':
			fullmatch = 1;
			break;
		default:
			usage();
			/* NOTREACHED */
		}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		criteria = 1;
	if (!criteria)
		usage();
	if (newest && oldest)
		errx(STATUS_ERROR, "Options -n and -o are mutually exclusive");
	if (pidfile != NULL)
		pidfromfile = takepid(pidfile, pidfilelock);
	else {
		if (pidfilelock) {
			errx(STATUS_ERROR,
			    "Option -L doesn't make sense without -F");
		}
		pidfromfile = -1;
	}

	mypid = getpid();

	/*
	 * If we're not matching args, we only need a buffer large enough to
	 * hold some relatively short error strings.  Otherwise, we have to
	 * assume we'll need up to ARG_MAX bytes for arguments.
	 */
	bufsz = _POSIX2_LINE_MAX;
	if (matchargs) {
		long arg_max;

		arg_max = sysconf(_SC_ARG_MAX);
		if (arg_max == -1)
			arg_max = ARG_MAX;

		/*
		 * The absolute worst case scenario is ARG_MAX single-byte
		 * arguments which we'll then separate with spaces and NUL
		 * terminate.
		 */
		bufsz = (arg_max * 2) + 1;
	}
	buf = malloc(bufsz);
	if (buf == NULL)
		err(STATUS_ERROR, "calloc");

#ifdef __APPLE__
	plist = copy_process_info();
	if (plist == NULL) {
		errx(STATUS_ERROR, "Cannot get process list");
	}
	nproc = sysmon_table_get_count(plist);
#else
	/*
	 * Retrieve the list of running processes from the kernel.
	 */
	kd = kvm_openfiles(execf, coref, NULL, O_RDONLY, buf);
	if (kd == NULL)
		errx(STATUS_ERROR, "Cannot open kernel files (%s)", buf);

	/*
	 * Use KERN_PROC_PROC instead of KERN_PROC_ALL, since we
	 * just want processes and not individual kernel threads.
	 */
	if (pidfromfile >= 0)
		plist = kvm_getprocs(kd, KERN_PROC_PID, pidfromfile, &nproc);
	else
		plist = kvm_getprocs(kd, KERN_PROC_PROC, 0, &nproc);
	if (plist == NULL) {
		errx(STATUS_ERROR, "Cannot get process list (%s)",
		    kvm_geterr(kd));
	}
#endif

	/*
	 * Allocate memory which will be used to keep track of the
	 * selection.
	 */
	if ((selected = malloc(nproc)) == NULL) {
		err(STATUS_ERROR, "Cannot allocate memory for %d processes",
		    nproc);
	}
	memset(selected, 0, nproc);

	/*
	 * Refine the selection.
	 */
	for (; *argv != NULL; argv++) {
		if ((rv = regcomp(&reg, *argv, cflags)) != 0) {
			regerror(rv, &reg, buf, bufsz);
			errx(STATUS_BADUSAGE,
			    "Cannot compile regular expression `%s' (%s)",
			    *argv, buf);
		}

#ifdef __APPLE__
		for (i = 0; i < nproc; i++) {
			kp = sysmon_table_get_row(plist, i);
#else
		for (i = 0, kp = plist; i < nproc; i++, kp++) {
#endif
			if (PSKIP(kp)) {
				if (debug_opt > 0)
				    fprintf(stderr, "* Skipped %5d %3d %s\n",
#ifdef __APPLE__
					(pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)),
					(uid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_UID)),
					xpc_string_get_string_ptr(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_COMM)));
#else
					kp->ki_pid, kp->ki_uid, kp->ki_comm);
#endif
				continue;
			}

#ifdef __APPLE__
			if (matchargs &&
			    (pargv = sysmon_row_get_value(kp, SYSMON_ATTR_PROC_ARGUMENTS)) != NULL) {
				jsz = 0;
				os_assert(bufsz >= _POSIX2_LINE_MAX);
				bufp = buf;
				xpc_array_apply(pargv, ^(size_t index, xpc_object_t value) {
					if (jsz >= bufsz) {
						return (bool)false;
					}
					jsz += snprintf(bufp + jsz,
					    bufsz - jsz,
					    index < xpc_array_get_count(pargv) - 1 ? "%s " : "%s",
					    xpc_string_get_string_ptr(value));
					return (bool)true;
				});
#else
			if (matchargs &&
			    (pargv = kvm_getargv(kd, kp, 0)) != NULL) {
				jsz = 0;
				while (jsz < bufsz && *pargv != NULL) {
					jsz += snprintf(buf + jsz,
					    bufsz - jsz,
					    pargv[1] != NULL ? "%s " : "%s",
					    pargv[0]);
					pargv++;
				}
#endif
				mstr = buf;
			} else
#ifdef __APPLE__
			{
				/*
				 * comm is limited to 15 bytes (MAXCOMLEN - 1).
				 * Try to use argv[0] (trimmed) if available.
				 */
				mstr = NULL;
				pargv = sysmon_row_get_value(kp, SYSMON_ATTR_PROC_ARGUMENTS);
				if (pargv != NULL && xpc_array_get_count(pargv) > 0) {
					const char *tmp = xpc_array_get_string(pargv, 0);
					if (tmp != NULL) {
						mstr = strrchr(tmp, '/');
						if (mstr != NULL) {
							mstr++;
						} else {
							mstr = (char *)tmp;
						}
					}
				}

				/* Fall back to "comm" if we failed to get argv[0]. */
				if (mstr == NULL || *mstr == '\0') {
					mstr = (char *)xpc_string_get_string_ptr(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_COMM));
				}

				/* Couldn't find process name, it probably exited. */
				if (mstr == NULL) {
					continue;
				}
			}
#else
				mstr = kp->ki_comm;
#endif

			rv = regexec(&reg, mstr, 1, &regmatch, 0);
			if (rv == 0) {
				if (fullmatch) {
					if (regmatch.rm_so == 0 &&
					    regmatch.rm_eo ==
					    (off_t)strlen(mstr))
						selected[i] = 1;
				} else
					selected[i] = 1;
			} else if (rv != REG_NOMATCH) {
				regerror(rv, &reg, buf, bufsz);
				errx(STATUS_ERROR,
				    "Regular expression evaluation error (%s)",
				    buf);
			}
			if (debug_opt > 1) {
				const char *rv_res = "NoMatch";
				if (selected[i])
					rv_res = "Matched";
				fprintf(stderr, "* %s %5d %3d %s\n", rv_res,
#ifdef __APPLE__
				    (pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)),
				    (uid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_UID)),
				    mstr);
#else
				    kp->ki_pid, kp->ki_uid, mstr);
#endif
			}
		}

		regfree(&reg);
	}

#ifdef __APPLE__
	for (i = 0; i < nproc; i++) {
		kp = sysmon_table_get_row(plist, i);
#else
	for (i = 0, kp = plist; i < nproc; i++, kp++) {
#endif
		if (PSKIP(kp))
			continue;

#ifdef __APPLE__
		if (pidfromfile >= 0 && (pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)) != pidfromfile) {
#else
		if (pidfromfile >= 0 && kp->ki_pid != pidfromfile) {
#endif
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &ruidlist, li_chain)
#ifdef __APPLE__
			if ((uid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_RUID)) == (uid_t)li->li_number)
#else
			if (kp->ki_ruid == (uid_t)li->li_number)
#endif
				break;
		if (SLIST_FIRST(&ruidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &rgidlist, li_chain)
#ifdef __APPLE__
			if ((gid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_RGID)) == (gid_t)li->li_number)
#else
			if (kp->ki_rgid == (gid_t)li->li_number)
#endif
				break;
		if (SLIST_FIRST(&rgidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &euidlist, li_chain)
#ifdef __APPLE__
			if ((uid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_UID)) == (uid_t)li->li_number)
#else
			if (kp->ki_uid == (uid_t)li->li_number)
#endif
				break;
		if (SLIST_FIRST(&euidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &ppidlist, li_chain)
#ifdef __APPLE__
			if ((pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PPID)) == (pid_t)li->li_number)
#else
			if (kp->ki_ppid == (pid_t)li->li_number)
#endif
				break;
		if (SLIST_FIRST(&ppidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &pgrplist, li_chain)
#ifdef __APPLE__
			if ((pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PGID)) == (pid_t)li->li_number)
#else
			if (kp->ki_pgid == (pid_t)li->li_number)
#endif
				break;
		if (SLIST_FIRST(&pgrplist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &tdevlist, li_chain) {
			if (li->li_number == -1 &&
#ifdef __APPLE__
			    (xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_FLAGS)) & PROC_FLAG_CONTROLT) == 0)
#else
			    (kp->ki_flag & P_CONTROLT) == 0)
#endif
				break;
#ifdef __APPLE__
			if ((dev_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_TDEV)) == (dev_t)li->li_number)
#else
			if (kp->ki_tdev == (dev_t)li->li_number)
#endif
				break;
		}
		if (SLIST_FIRST(&tdevlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

#ifndef __APPLE__
		SLIST_FOREACH(li, &sidlist, li_chain)
			if (kp->ki_sid == (pid_t)li->li_number)
				break;
		if (SLIST_FIRST(&sidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &jidlist, li_chain) {
			/* A particular jail ID, including 0 (not in jail) */
			if (kp->ki_jid == (int)li->li_number)
				break;
			/* Any jail */
			if (kp->ki_jid > 0 && li->li_number == -1)
				break;
		}
		if (SLIST_FIRST(&jidlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}

		SLIST_FOREACH(li, &classlist, li_chain) {
			/*
			 * We skip P_SYSTEM processes to match ps(1) output.
			 */
			if ((kp->ki_flag & P_SYSTEM) == 0 &&
			    strcmp(kp->ki_loginclass, li->li_name) == 0)
				break;
		}
		if (SLIST_FIRST(&classlist) != NULL && li == NULL) {
			selected[i] = 0;
			continue;
		}
#endif /* !__APPLE__ */

		if (argc == 0)
			selected[i] = 1;
	}

	if (!ancestors) {
		pid = mypid;
		while (pid) {
#ifdef __APPLE__
			for (i = 0; i < nproc; i++) {
				kp = sysmon_table_get_row(plist, i);
#else
			for (i = 0, kp = plist; i < nproc; i++, kp++) {
#endif
				if (PSKIP(kp))
					continue;
#ifdef __APPLE__
				if ((pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)) == pid) {
					selected[i] = 0;
					pid = (pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PPID));
					break;
				}
#else
				if (kp->ki_pid == pid) {
					selected[i] = 0;
					pid = kp->ki_ppid;
					break;
				}
#endif
			}
			if (i == nproc) {
				if (pid == mypid)
					pid = getppid();
				else
					break;	/* Maybe we're in a jail ? */
			}
		}
	}

	if (newest || oldest) {
#ifdef __APPLE__
		best_tval = 0;
#else
		best_tval.tv_sec = 0;
		best_tval.tv_usec = 0;
#endif
		bestidx = -1;

#ifdef __APPLE__
		for (i = 0; i < nproc; i++) {
			kp = sysmon_table_get_row(plist, i);
#else
		for (i = 0, kp = plist; i < nproc; i++, kp++) {
#endif
			if (!selected[i])
				continue;
			if (bestidx == -1) {
				/* The first entry of the list which matched. */
				;
#ifdef __APPLE__
			} else if (xpc_date_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_START)) > best_tval) {
#else
			} else if (timercmp(&kp->ki_start, &best_tval, >)) {
#endif
				/* This entry is newer than previous "best". */
				if (oldest)	/* but we want the oldest */
					continue;
			} else {
				/* This entry is older than previous "best". */
				if (newest)	/* but we want the newest */
					continue;
			}
			/* This entry is better than previous "best" entry. */
#ifdef __APPLE__
			best_tval = xpc_date_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_START));
#else
			best_tval.tv_sec = kp->ki_start.tv_sec;
			best_tval.tv_usec = kp->ki_start.tv_usec;
#endif
			bestidx = i;
		}

		memset(selected, 0, nproc);
		if (bestidx != -1)
			selected[bestidx] = 1;
	}

	/*
	 * Take the appropriate action for each matched process, if any.
	 */
	did_action = 0;
#ifdef __APPLE__
	for (i = 0, rv = 0; i < nproc; i++) {
		kp = sysmon_table_get_row(plist, i);
#else
	for (i = 0, rv = 0, kp = plist; i < nproc; i++, kp++) {
#endif
		if (PSKIP(kp))
			continue;
		if (selected[i]) {
			if (longfmt && !pgrep) {
				did_action = 1;
#ifdef __APPLE__
				printf("kill -%d %d\n", signum, (int)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)));
#else
				printf("kill -%d %d\n", signum, kp->ki_pid);
#endif
			}
			if (inverse)
				continue;
		} else if (!inverse)
			continue;
		rv |= (*action)(kp);
	}
	if (rv && pgrep && !quiet)
		putchar('\n');
	if (!did_action && !pgrep && longfmt)
		fprintf(stderr,
		    "No matching processes belonging to you were found\n");

	free(buf);
	exit(rv ? STATUS_MATCH : STATUS_NOMATCH);
}

static void
usage(void)
{
	const char *ustr;

	if (pgrep)
#ifdef __APPLE__
		ustr = "[-Lfilnoqvx] [-d delim]";
#else
		ustr = "[-LSfilnoqvx] [-d delim]";
#endif
	else
		ustr = "[-signal] [-ILfilnovx]";

	fprintf(stderr,
#ifdef __APPLE__
		"usage: %s %s [-F pidfile] [-G gid]\n"
		"             [-P ppid] [-U uid] [-g pgrp] [-t tty] [-u euid]\n"
		"             pattern ...\n",
#else
		"usage: %s %s [-F pidfile] [-G gid] [-M core] [-N system]\n"
		"             [-P ppid] [-U uid] [-c class] [-g pgrp] [-j jail]\n"
		"             [-s sid] [-t tty] [-u euid] pattern ...\n",
#endif
		getprogname(), ustr);

	exit(STATUS_BADUSAGE);
}

static void
#ifdef __APPLE__
show_process(const sysmon_row_t kp)
#else
show_process(const struct kinfo_proc *kp)
#endif
{
#ifdef __APPLE__
	xpc_object_t argv;
#else
	char **argv;
#endif

	if (quiet) {
		assert(pgrep);
		return;
	}
#ifdef __APPLE__
	if ((longfmt || !pgrep) && matchargs &&
	    (argv = sysmon_row_get_value(kp, SYSMON_ATTR_PROC_ARGUMENTS)) != NULL) {
		printf("%d ", (int)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)));
		(void)xpc_array_apply(argv, ^(size_t index, xpc_object_t value) {
			printf("%s", xpc_string_get_string_ptr(value));
			if (index < xpc_array_get_count(argv) - 1)
				putchar(' ');
			return (bool)true;
		});
	} else if (longfmt || !pgrep)
		printf("%d %s",
		    (int)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)),
		    xpc_string_get_string_ptr(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_COMM)));
	else
		printf("%d", (int)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)));
#else
	if ((longfmt || !pgrep) && matchargs &&
	    (argv = kvm_getargv(kd, kp, 0)) != NULL) {
		printf("%d ", (int)kp->ki_pid);
		for (; *argv != NULL; argv++) {
			printf("%s", *argv);
			if (argv[1] != NULL)
				putchar(' ');
		}
	} else if (longfmt || !pgrep)
		printf("%d %s", (int)kp->ki_pid, kp->ki_comm);
	else
		printf("%d", (int)kp->ki_pid);
#endif
}

static int
#ifdef __APPLE__
killact(const sysmon_row_t kp)
#else
killact(const struct kinfo_proc *kp)
#endif
{
	int ch, first;

	if (interactive) {
		/*
		 * Be careful, ask before killing.
		 */
		printf("kill ");
		show_process(kp);
		printf("? ");
		fflush(stdout);
		first = ch = getchar();
		while (ch != '\n' && ch != EOF)
			ch = getchar();
		if (first != 'y' && first != 'Y')
			return (1);
	}
#ifdef __APPLE__
	if (kill((pid_t)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)), signum) == -1) {
#else
	if (kill(kp->ki_pid, signum) == -1) {
#endif
		/* 
		 * Check for ESRCH, which indicates that the process
		 * disappeared between us matching it and us
		 * signalling it; don't issue a warning about it.
		 */
		if (errno != ESRCH)
#ifdef __APPLE__
			warn("signalling pid %d", (int)xpc_uint64_get_value(sysmon_row_get_value(kp, SYSMON_ATTR_PROC_PID)));
#else
			warn("signalling pid %d", (int)kp->ki_pid);
#endif
		/*
		 * Return 0 to indicate that the process should not be
		 * considered a match, since we didn't actually get to
		 * signal it.
		 */
		return (0);
	}
	return (1);
}

static int
#ifdef __APPLE__
grepact(const sysmon_row_t kp)
#else
grepact(const struct kinfo_proc *kp)
#endif
{
	static bool first = true;

	if (!quiet && !first)
		printf("%s", delim);
	show_process(kp);
	first = false;
	return (1);
}

static void
makelist(struct listhead *head, enum listtype type, char *src)
{
	struct list *li;
	struct passwd *pw;
	struct group *gr;
	struct stat st;
	const char *cp;
	char *sp, *ep, buf[MAXPATHLEN];
	int empty;

	empty = 1;

	while ((sp = strsep(&src, ",")) != NULL) {
		if (*sp == '\0')
			usage();

		if ((li = malloc(sizeof(*li))) == NULL) {
			err(STATUS_ERROR, "Cannot allocate %zu bytes",
			    sizeof(*li));
		}

		SLIST_INSERT_HEAD(head, li, li_chain);
		empty = 0;

#ifdef __APPLE__
		li->li_number = (uid_t)strtol(sp, &ep, 0);
		if (*ep == '\0') {
#else
		if (type != LT_CLASS)
			li->li_number = (uid_t)strtol(sp, &ep, 0);

		if (type != LT_CLASS && *ep == '\0') {
#endif
			switch (type) {
			case LT_PGRP:
				if (li->li_number == 0)
					li->li_number = getpgrp();
				break;
#ifndef __APPLE__
			case LT_SID:
				if (li->li_number == 0)
					li->li_number = getsid(mypid);
				break;
			case LT_JAIL:
				if (li->li_number < 0)
					errx(STATUS_BADUSAGE,
					     "Negative jail ID `%s'", sp);
				/* For compatibility with old -j */
				if (li->li_number == 0)
					li->li_number = -1;	/* any jail */
				break;
#endif /* !__APPLE__ */
			case LT_TTY:
				if (li->li_number < 0)
					errx(STATUS_BADUSAGE,
					     "Negative /dev/pts tty `%s'", sp);
				snprintf(buf, sizeof(buf), _PATH_DEV "pts/%s",
				    sp);
				if (stat(buf, &st) != -1)
					goto foundtty;
				if (errno == ENOENT)
					errx(STATUS_BADUSAGE, "No such tty: `"
					    _PATH_DEV "pts/%s'", sp);
				err(STATUS_ERROR, "Cannot access `"
				    _PATH_DEV "pts/%s'", sp);
				break;
			default:
				break;
			}
			continue;
		}

		switch (type) {
		case LT_USER:
			if ((pw = getpwnam(sp)) == NULL)
				errx(STATUS_BADUSAGE, "Unknown user `%s'", sp);
			li->li_number = pw->pw_uid;
			break;
		case LT_GROUP:
			if ((gr = getgrnam(sp)) == NULL)
				errx(STATUS_BADUSAGE, "Unknown group `%s'", sp);
			li->li_number = gr->gr_gid;
			break;
		case LT_TTY:
			if (strcmp(sp, "-") == 0) {
				li->li_number = -1;
				break;
			} else if (strcmp(sp, "co") == 0) {
				cp = "console";
			} else {
				cp = sp;
			}

			snprintf(buf, sizeof(buf), _PATH_DEV "%s", cp);
			if (stat(buf, &st) != -1)
				goto foundtty;

			snprintf(buf, sizeof(buf), _PATH_DEV "tty%s", cp);
			if (stat(buf, &st) != -1)
				goto foundtty;

			if (errno == ENOENT)
				errx(STATUS_BADUSAGE, "No such tty: `%s'", sp);
			err(STATUS_ERROR, "Cannot access `%s'", sp);

foundtty:		if ((st.st_mode & S_IFCHR) == 0)
				errx(STATUS_BADUSAGE, "Not a tty: `%s'", sp);

			li->li_number = st.st_rdev;
			break;
#ifndef __APPLE__
		case LT_JAIL: {
			int jid;

			if (strcmp(sp, "none") == 0)
				li->li_number = 0;
			else if (strcmp(sp, "any") == 0)
				li->li_number = -1;
			else if ((jid = jail_getid(sp)) != -1)
				li->li_number = jid;
			else if (*ep != '\0')
				errx(STATUS_BADUSAGE,
				     "Invalid jail ID or name `%s'", sp);
			break;
		}
		case LT_CLASS:
			li->li_number = -1;
			li->li_name = strdup(sp);
			if (li->li_name == NULL)
				err(STATUS_ERROR, "Cannot allocate memory");
			break;
#endif /* !__APPLE__ */
		default:
			usage();
		}
	}

	if (empty)
		usage();
}

static int
takepid(const char *pidfile, int pidfilelock)
{
	char *endp, line[BUFSIZ];
	FILE *fh;
	long rval;

	fh = fopen(pidfile, "r");
	if (fh == NULL)
		err(STATUS_ERROR, "Cannot open pidfile `%s'", pidfile);

	if (pidfilelock) {
		/*
		 * If we can lock pidfile, this means that daemon is not
		 * running, so would be better not to kill some random process.
		 */
		if (flock(fileno(fh), LOCK_EX | LOCK_NB) == 0) {
			(void)fclose(fh);
			errx(STATUS_ERROR, "File '%s' can be locked", pidfile);
		} else {
			if (errno != EWOULDBLOCK) {
				errx(STATUS_ERROR,
				    "Error while locking file '%s'", pidfile);
			}
		}
	}

	if (fgets(line, sizeof(line), fh) == NULL) {
		if (feof(fh)) {
			(void)fclose(fh);
			errx(STATUS_ERROR, "Pidfile `%s' is empty", pidfile);
		}
		(void)fclose(fh);
		err(STATUS_ERROR, "Cannot read from pid file `%s'", pidfile);
	}
	(void)fclose(fh);

	rval = strtol(line, &endp, 10);
	if (*endp != '\0' && !isspace((unsigned char)*endp))
		errx(STATUS_ERROR, "Invalid pid in file `%s'", pidfile);
	else if (rval < MIN_PID || rval > MAX_PID)
		errx(STATUS_ERROR, "Invalid pid in file `%s'", pidfile);
	return (rval);
}
