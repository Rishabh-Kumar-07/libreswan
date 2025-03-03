/*
 * Pluto main program
 *
 * Copyright (C) 1997      Angelos D. Keromytis.
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009-2016 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2016 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Kim B. Heino <b@bbbs.net>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2016-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <pthread.h> /* Must be the first include file */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>	/* for unlink(), write(), close(), access(), et.al. */

#include "lswconf.h"
#include "lswfips.h"
#include "lswnss.h"
#include "defs.h"
#include "nss_ocsp.h"
#include "server_fork.h"		/* for init_server_fork() */
#include "server.h"
#include "kernel.h"	/* needs connections.h */
#include "log.h"
#include "keys.h"
#include "secrets.h"    /* for free_remembered_public_keys() */
#include "rnd.h"
#include "fetch.h"
#include "ipsecconf/confread.h"
#include "crypto.h"
#include "vendor.h"
#include "enum_names.h"
#include "virtual_ip.h"
#include "state_db.h"		/* for init_state_db() */
#include "revival.h"		/* for init_revival() */
#include "connection_db.h"	/* for connection_state_db() */
#include "nat_traversal.h"
#include "ike_alg.h"
#include "ikev2_redirect.h"
#include "root_certs.h"		/* for init_root_certs() */
#include "host_pair.h"		/* for init_host_pair() */
#include "ikev1.h"		/* for init_ikev1() */
#include "ikev2.h"		/* for init_ikev2() */
#include "crypt_symkey.h"	/* for init_crypt_symkey() */
#include "crl_queue.h"		/* for free_crl_queue() */
#include "iface.h"
#include "server_pool.h"

#ifndef IPSECDIR
#define IPSECDIR "/etc/ipsec.d"
#endif

#ifdef HAVE_LIBCAP_NG
# include <cap-ng.h>	/* from libcap-ng devel */
#endif

#include "labeled_ipsec.h"		/* for init_labeled_ipsec() */

# include "pluto_sd.h"		/* for pluto_sd_init() */

#ifdef USE_DNSSEC
#include "dnssec.h"
#endif

#ifdef HAVE_SECCOMP
#include "pluto_seccomp.h"
#endif

static void fatal_opt(int longindex, struct logger *logger, const char *fmt, ...) PRINTF_LIKE(3) NEVER_RETURNS;

static const char *pluto_name;	/* name (path) we were invoked with */

static pthread_t main_thread;

bool in_main_thread(void)
{
	return pthread_equal(pthread_self(), main_thread);
}

static char *rundir = NULL;
char *pluto_listen = NULL;
static bool fork_desired = USE_FORK || USE_DAEMON;
static bool selftest_only = FALSE;

#ifdef FIPS_CHECK
# include <fipscheck.h> /* from fipscheck devel */
static const char *fips_package_files[] = { IPSEC_EXECDIR "/pluto", NULL };
#endif

/* pulled from main for show_setup_plutomain() */
static const struct lsw_conf_options *oco;
static char *coredir;
static char *conffile;
static int pluto_nss_seedbits;
static int nhelpers = -1;
static bool do_dnssec = FALSE;
static char *pluto_dnssec_rootfile = NULL;
static char *pluto_dnssec_trusted = NULL;

static char *ocsp_uri = NULL;
static char *ocsp_trust_name = NULL;
static int ocsp_timeout = OCSP_DEFAULT_TIMEOUT;
static int ocsp_method = OCSP_METHOD_GET;
static int ocsp_cache_size = OCSP_DEFAULT_CACHE_SIZE;
static int ocsp_cache_min_age = OCSP_DEFAULT_CACHE_MIN_AGE;
static int ocsp_cache_max_age = OCSP_DEFAULT_CACHE_MAX_AGE;

static char *pluto_lock_filename = NULL;
static bool pluto_lock_created = false;

void free_pluto_main(void)
{
	/* Some values can be NULL if not specified as pluto argument */
	pfree(pluto_lock_filename);
	pfree(coredir);
	pfree(conffile);
	pfreeany(pluto_stats_binary);
	pfreeany(pluto_listen);
	pfree(pluto_vendorid);
	pfreeany(ocsp_uri);
	pfreeany(ocsp_trust_name);
	pfreeany(curl_iface);
	pfreeany(pluto_log_file);
	pfreeany(pluto_dnssec_rootfile);
	pfreeany(pluto_dnssec_trusted);
	pfreeany(rundir);
	free_global_redirect_dests();
}

/* string naming compile-time options that have interop implications */
static const char compile_time_interop_options[] = ""
	" IKEv2"
#ifdef USE_IKEv1
	" IKEv1"
#endif
#ifdef XFRM_SUPPORT
	" XFRM"
#endif
#ifdef USE_XFRM_INTERFACE
	" XFRMI"
#endif
	" esp-hw-offload"
#if USE_FORK
	" FORK"
#endif
#if USE_VFORK
	" VFORK"
#endif
#if USE_DAEMON
	" DAEMON"
#endif
#if USE_PTHREAD_SETSCHEDPRIO
	" PTHREAD_SETSCHEDPRIO"
#endif
#if defined __GNUC__ && defined __EXCEPTIONS
	" GCC_EXCEPTIONS"
#endif
#ifdef HAVE_BROKEN_POPEN
	" BROKEN_POPEN"
#endif
	" NSS"
#ifdef NSS_REQ_AVA_COPY
	" (AVA copy)"
#endif
#ifdef NSS_IPSEC_PROFILE
	" (IPsec profile)"
#endif
#ifdef USE_NSS_KDF
	" (NSS-PRF)"
#else
	" (native-PRF)"
#endif
#ifdef USE_DNSSEC
	" DNSSEC"
#endif
#ifdef USE_SYSTEMD_WATCHDOG
	" SYSTEMD_WATCHDOG"
#endif
#ifdef FIPS_CHECK
	" FIPS_BINCHECK"
#endif
#ifdef HAVE_LABELED_IPSEC
	" LABELED_IPSEC"
	" (SELINUX)"
#endif
#ifdef HAVE_SECCOMP
	" SECCOMP"
#endif
#ifdef HAVE_LIBCAP_NG
	" LIBCAP_NG"
#endif
#ifdef USE_LINUX_AUDIT
	" LINUX_AUDIT"
#endif
#ifdef USE_PAM_AUTH
	" AUTH_PAM"
#endif
#ifdef HAVE_NM
	" NETWORKMANAGER"
#endif
#ifdef LIBCURL
	" CURL(non-NSS)"
#endif
#ifdef LIBLDAP
	" LDAP(non-NSS)"
#endif
#ifdef USE_EFENCE
	" EFENCE"
#endif
;

/* create lockfile, or die in the attempt */
static int create_lock(struct logger *logger)
{
	if (mkdir(rundir, 0755) != 0) {
		if (errno != EEXIST) {
			fatal_errno(PLUTO_EXIT_LOCK_FAIL, logger, errno,
				    "unable to create lock dir: \"%s\"", rundir);
		}
	}

	unsigned attempt;
	for (attempt = 0; attempt < 2; attempt++) {
		int fd = open(pluto_lock_filename, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
			      S_IRUSR | S_IRGRP | S_IROTH);
		if (fd >= 0) {
			pluto_lock_created = true;
			return fd;
		}
		if (errno != EEXIST) {
			fatal_errno(PLUTO_EXIT_LOCK_FAIL, logger, errno,
				    "unable to create lock file \"%s\"", pluto_lock_filename);
		}
		if (fork_desired) {
			fatal(PLUTO_EXIT_LOCK_FAIL, logger,
			      "lock file \"%s\" already exists", pluto_lock_filename);
		}
		/*
		 * if we did not fork, then we don't really need the pid to
		 * control, so wipe it
		 */
		if (unlink(pluto_lock_filename) == -1) {
			fatal_errno(PLUTO_EXIT_LOCK_FAIL, logger, errno,
				    "lock file \"%s\" already exists and could not be removed",
				    pluto_lock_filename);
		}
		/*
		 * lock file removed, try creating it
		 * again ...
		 */
	}
	fatal(PLUTO_EXIT_LOCK_FAIL, logger, "lock file \"%s\" could not be created after %u attempts",
	      pluto_lock_filename, attempt);
}

/*
 * fill_lock - Populate the lock file with pluto's PID
 *
 * @param lockfd File Descriptor for the lock file
 * @param pid PID (pid_t struct) to be put into the lock file
 * @return bool True if successful
 */
static bool fill_lock(int lockfd, pid_t pid)
{
	char buf[30];	/* holds "<pid>\n" */
	int len = snprintf(buf, sizeof(buf), "%u\n", (unsigned int) pid);
	bool ok = len > 0 && write(lockfd, buf, len) == len;

	close(lockfd);
	return ok;
}

/*
 * delete_lock - Delete the lock file
 */
void delete_lock(void)
{
	if (pluto_lock_created) {
		delete_ctl_socket();
		unlink(pluto_lock_filename);	/* is noting failure useful? */
	}
}

/*
 * parser.l and keywords.c need these global variables
 * FIXME: move them to confread_load() parameters
 */
int verbose = 0;

/* Read config file. exit() on error. */
static struct starter_config *read_cfg_file(char *configfile, long longindex, struct logger *logger)
{
	struct starter_config *cfg = NULL;
	starter_errors_t errl = { NULL };

	cfg = confread_load(configfile, &errl, NULL /* ctl_addr.sun_path? */, true, logger);
	if (cfg == NULL) {
		/*
		 * note: fatal_opt() never returns so we will have a
		 * technical leak of errl.errors
		 */
		fatal_opt(longindex, logger, "%s", errl.errors);
	}

	if (errl.errors != NULL) {
		llog(RC_LOG, logger, "pluto --config '%s', ignoring: %s",
			    configfile, errl.errors);
		pfree(errl.errors);
	}

	return cfg;
}

/*
 * Helper function for config file mapper: set string option value.
 * Values passed in are expected to have been allocated using our
 * own functions.
 */
static void set_cfg_string(char **target, char *value)
{
	/* Do nothing if value is unset. */
	if (value == NULL || *value == '\0')
		return;

	/* Don't free previous target, it might be statically set. */
	*target = clone_str(value, "(ignore) set_cfg_string item");
}

/*
 * This function MUST NOT be used for anything else!
 * It is used to seed the NSS PRNG based on --seedbits pluto argument
 * or the seedbits= * config setup option in ipsec.conf.
 * Everything else that needs random MUST use get_rnd_bytes()
 * This function MUST NOT be changed to use /dev/urandom.
 */
static void get_bsi_random(size_t nbytes, unsigned char *buf, struct logger *logger)
{
	size_t ndone;
	int dev;
	ssize_t got;
	const char *device = "/dev/random";

	dev = open(device, 0);
	if (dev < 0) {
		fatal_errno(PLUTO_EXIT_NSS_FAIL, logger, errno,
			    "could not open %s", device);
	}

	ndone = 0;
	dbg("need %d bits random for extra seeding of the NSS PRNG",
	    (int) nbytes * BITS_PER_BYTE);

	while (ndone < nbytes) {
		got = read(dev, buf + ndone, nbytes - ndone);
		if (got < 0) {
			fatal_errno(PLUTO_EXIT_NSS_FAIL, logger, errno,
				    "read error on %s", device);
		}
		if (got == 0) {
			fatal(PLUTO_EXIT_NSS_FAIL, logger, "EOF on %s!?!\n",  device);
		}
		ndone += got;
	}
	close(dev);
	dbg("read %zu bytes from /dev/random for NSS PRNG", nbytes);
}

static void pluto_init_nss(const char *nssdir, struct logger *logger)
{
	diag_t d = lsw_nss_setup(nssdir, LSW_NSS_READONLY, logger);
	if (d != NULL) {
		fatal_diag(PLUTO_EXIT_NSS_FAIL, logger, &d, "%s", "");
	}
	llog(RC_LOG, logger, "NSS crypto library initialized");

	/*
	 * This exists purely to make the BSI happy.
	 * We do not inflict this on other users
	 */
	if (pluto_nss_seedbits != 0) {
		int seedbytes = BYTES_FOR_BITS(pluto_nss_seedbits);
		unsigned char *buf = alloc_bytes(seedbytes, "TLA seedmix");

		get_bsi_random(seedbytes, buf, logger); /* much TLA, very blocking */
		SECStatus rv = PK11_RandomUpdate(buf, seedbytes);
		llog(RC_LOG, logger, "seeded %d bytes into the NSS PRNG", seedbytes);
		passert(rv == SECSuccess);
		messupn(buf, seedbytes);
		pfree(buf);
	}
}

/* 0 is special and default: do not check crls dynamically */
deltatime_t crl_check_interval = DELTATIME_INIT(0);

/*
 * Attribute Type "constant" for Security Context
 *
 * Originally, we assigned the value 10, but that properly belongs to ECN_TUNNEL.
 * We then assigned 32001 which is in the private range RFC 2407.
 * Unfortunately, we feel we have to support 10 as an option for backward
 * compatibility.
 * This variable specifies (globally!!) which we support: 10 or 32001.
 * That makes migration to 32001 all or nothing.
 */
uint16_t secctx_attr_type = SECCTX;

/*
 * Table of Pluto command-line options.
 *
 * For getopt_long(3), but with twists.
 *
 * We never find that letting getopt set an option makes sense
 * so flag is always NULL.
 *
 * Trick: we split each "name" string with an explicit '\0'.
 * Before the '\0' is the option name, as seen by getopt_long.
 * After the '\0' is meta-information:
 * - _ means: obsolete due to _ in name: replace _ with -
 * - > means: obsolete spelling; use spelling from rest of string
 * - ! means: obsolete and ignored (no replacement)
 * - anything else is a description of the options argument (printed by --help)
 *   If it starts with ^, that means start a newline in the --help output.
 *
 * The table should be ordered to maximize the clarity of --help.
 *
 */

enum {
	OPT_OFFSET = 256, /* larger than largest char */
	OPT_EFENCE_PROTECT,
	OPT_DEBUG,
	OPT_IMPAIR,
	OPT_DNSSEC_ROOTKEY_FILE,
	OPT_DNSSEC_TRUSTED,
};

static const struct option long_opts[] = {
	/* name, has_arg, flag, val */
	{ "help\0", no_argument, NULL, 'h' },
	{ "version\0", no_argument, NULL, 'v' },
	{ "config\0<filename>", required_argument, NULL, 'z' },
	{ "nofork\0", no_argument, NULL, '0' },
	{ "stderrlog\0", no_argument, NULL, 'e' },
	{ "logfile\0<filename>", required_argument, NULL, 'g' },
#ifdef USE_DNSSEC
	{ "dnssec-rootkey-file\0<filename>", required_argument, NULL,
		OPT_DNSSEC_ROOTKEY_FILE },
	{ "dnssec-trusted\0<filename>", required_argument, NULL,
		OPT_DNSSEC_TRUSTED },
#endif
	{ "log-no-time\0", no_argument, NULL, 't' }, /* was --plutostderrlogtime */
	{ "log-no-append\0", no_argument, NULL, '7' },
	{ "log-no-ip\0", no_argument, NULL, '<' },
	{ "log-no-audit\0", no_argument, NULL, 'a' },
	{ "force-busy\0", no_argument, NULL, 'D' },
	{ "force-unlimited\0", no_argument, NULL, 'U' },
	{ "crl-strict\0", no_argument, NULL, 'r' },
	{ "ocsp-strict\0", no_argument, NULL, 'o' },
	{ "ocsp-enable\0", no_argument, NULL, 'O' },
	{ "ocsp-uri\0", required_argument, NULL, 'Y' },
	{ "ocsp-timeout\0", required_argument, NULL, 'T' },
	{ "ocsp-trustname\0", required_argument, NULL, 'J' },
	{ "ocsp-cache-size\0", required_argument, NULL, 'E' },
	{ "ocsp-cache-min-age\0", required_argument, NULL, 'G' },
	{ "ocsp-cache-max-age\0", required_argument, NULL, 'H' },
	{ "ocsp-method\0", required_argument, NULL, 'B' },
	{ "crlcheckinterval\0", required_argument, NULL, 'x' },
	{ "uniqueids\0", no_argument, NULL, 'u' },
	{ "no-dnssec\0", no_argument, NULL, 'R' },
	{ "use-xfrm\0", no_argument, NULL, 'K' },
	{ "use-bsdkame\0",   no_argument, NULL, 'F' },
	{ "interface\0<ifname|ifaddr>", required_argument, NULL, 'i' },
	{ "curl-iface\0<ifname|ifaddr>", required_argument, NULL, 'Z' },
	{ "curl-timeout\0<secs>", required_argument, NULL, 'I' },
	{ "listen\0<ifaddr>", required_argument, NULL, 'L' },
	{ "listen-tcp\0", no_argument, NULL, 'm' },
	{ "no-listen-udp\0", no_argument, NULL, 'p' },
	{ "ike-socket-bufsize\0<buf-size>", required_argument, NULL, 'W' },
	{ "ike-socket-no-errqueue\0", no_argument, NULL, '1' },
	{ "nflog-all\0<group-number>", required_argument, NULL, 'G' },
	{ "rundir\0<path>", required_argument, NULL, 'b' }, /* was ctlbase */
	{ "secretsfile\0<secrets-file>", required_argument, NULL, 's' },
	{ "global-redirect\0", required_argument, NULL, 'Q'},
	{ "global-redirect-to\0", required_argument, NULL, 'y'},
	{ "coredir\0>dumpdir", required_argument, NULL, 'C' },	/* redundant spelling */
	{ "dumpdir\0<dirname>", required_argument, NULL, 'C' },
	{ "statsbin\0<filename>", required_argument, NULL, 'S' },
	{ "ipsecdir\0<ipsec-dir>", required_argument, NULL, 'f' },
	{ "foodgroupsdir\0>ipsecdir", required_argument, NULL, 'f' },	/* redundant spelling */
	{ "nssdir\0<path>", required_argument, NULL, 'd' },	/* nss-tools use -d */
	{ "keep-alive\0<delay_secs>", required_argument, NULL, '2' },
	{ "virtual-private\0<network_list>", required_argument, NULL, '6' },
	{ "nhelpers\0<number>", required_argument, NULL, 'j' },
	{ "expire-shunt-interval\0<secs>", required_argument, NULL, '9' },
	{ "seedbits\0<number>", required_argument, NULL, 'c' },
	/* really an attribute type, not a value */
	{ "ikev1-secctx-attr-type\0<number>", required_argument, NULL, 'w' },
	{ "ikev1-reject\0", no_argument, NULL, 'k' },
	{ "ikev1-drop\0", no_argument, NULL, 'l' },
#ifdef HAVE_SECCOMP
	{ "seccomp-enabled\0", no_argument, NULL, '3' },
	{ "seccomp-tolerant\0", no_argument, NULL, '4' },
#endif
	{ "vendorid\0<vendorid>", required_argument, NULL, 'V' },

	{ "selftest\0", no_argument, NULL, '5' },

	{ "leak-detective\0", no_argument, NULL, 'X' },
	{ "efence-protect\0", required_argument, NULL, OPT_EFENCE_PROTECT, },
	{ "debug-none\0^", no_argument, NULL, 'N' },
	{ "debug-all\0", no_argument, NULL, 'A' },
	{ "debug\0", required_argument, NULL, OPT_DEBUG, },
	{ "impair\0", required_argument, NULL, OPT_IMPAIR, },

	{ 0, 0, 0, 0 }
};

/*
 * HACK: check UGH, and if it is bad, log it along with the option.
 */

static void fatal_opt(int longindex, struct logger *logger, const char *fmt, ...)
{
	passert(longindex >= 0);
	const char *optname = long_opts[longindex].name;
	LLOG_JAMBUF(RC_LOG, logger, buf) {
		if (optarg == NULL) {
			jam(buf, "option --%s invalid: ", optname);
		} else {
			jam(buf, "option --%s \"%s\" invalid: ", optname, optarg);
		}
		va_list ap;
		va_start(ap, fmt);
		jam_va_list(buf, fmt, ap);
		va_end(ap);
	}
	/* not exit_pluto as pluto isn't yet up and running? */
	exit(PLUTO_EXIT_FAIL);
}

static void check_err(err_t ugh, int longindex, struct logger *logger)
{
	if (ugh != NULL) {
		fatal_opt(longindex, logger, "%s", ugh);
	}
}

/* print full usage (from long_opts[]) */
static void usage(FILE *stream)
{
	const struct option *opt;
	char line[72];
	size_t lw;

	snprintf(line, sizeof(line), "Usage: %s", pluto_name);
	lw = strlen(line);

	for (opt = long_opts; opt->name != NULL; opt++) {
		const char *nm = opt->name;
		const char *meta = nm + strlen(nm) + 1;
		bool force_nl = FALSE;
		char chunk[sizeof(line) - 1];
		int cw;

		switch (*meta) {
		case '_':
		case '>':
		case '!':
			/* ignore these entries */
			break;
		case '^':
			force_nl = TRUE;
			meta++;	/* eat ^ */
			/* FALL THROUGH */
		default:
			if (*meta == '\0')
				snprintf(chunk, sizeof(chunk),  "[--%s]", nm);
			else
				snprintf(chunk, sizeof(chunk),  "[--%s %s]", nm, meta);
			cw = strlen(chunk);

			if (force_nl || lw + cw + 2 >= sizeof(line)) {
				fprintf(stream, "%s\n", line);
				line[0] = '\t';
				lw = 1;
			} else {
				line[lw++] = ' ';
			}
			passert(lw + cw + 1 < sizeof(line));
			strcpy(&line[lw], chunk);
			lw += cw;
		}
	}

	fprintf(stream, "%s\n", line);

	fprintf(stream, "Libreswan %s\n", ipsec_version_code());
}

#ifdef USE_DNSSEC
static void set_dnssec_file_names (struct starter_config *cfg)
{
	if (cfg->setup.strings[KSF_PLUTO_DNSSEC_ROOTKEY_FILE][0] != '\0') {
		pfreeany(pluto_dnssec_rootfile);
		set_cfg_string(&pluto_dnssec_rootfile,
				cfg->setup.strings[KSF_PLUTO_DNSSEC_ROOTKEY_FILE]);
	} else {
		/* unset the global one config file unset it */
		pfreeany(pluto_dnssec_rootfile);
		pluto_dnssec_rootfile = NULL;
	}
	if (cfg->setup.strings[KSF_PLUTO_DNSSEC_ANCHORS] != NULL &&
			cfg->setup.strings[KSF_PLUTO_DNSSEC_ANCHORS][0] != '\0') {
		set_cfg_string(&pluto_dnssec_trusted,
				cfg->setup.strings[KSF_PLUTO_DNSSEC_ANCHORS]);
	}
}
#endif

#ifdef USE_EFENCE
extern int EF_PROTECT_BELOW;
extern int EF_PROTECT_FREE;
#endif

int main(int argc, char **argv)
{
	struct log_param log_param = default_log_param;

	/*
	 * Some options should to be processed before the first
	 * malloc() call, so scan for them here.
	 *
	 * - leak-detective is immutable, it must come before the
	 *   first malloc()
	 *
	 * - efence-protect seems to be less strict, but enabling it
	 *   early must be a good thing (TM) right
	 */
	for (int i = 1; i < argc; ++i) {
		if (streq(argv[i], "--leak-detective"))
			leak_detective = TRUE;
#ifdef USE_EFENCE
		else if (streq(argv[i], "--efence-protect")) {
			EF_PROTECT_BELOW = 1;
			EF_PROTECT_FREE = 1;
		}
#endif
	}

	/*
	 * Identify the main thread.
	 *
	 * Also used as a reserved thread for code wanting to
	 * determine if it is running on an aux thread.
	 */
	main_thread = pthread_self();

	int lockfd;

	/*
	 * We read the intentions for how to log from command line options
	 * and the config file. Then we prepare to be able to log, but until
	 * then log to stderr (better then nothing). Once we are ready to
	 * actually do logging according to the methods desired, we set the
	 * variables for those methods
	 */
	bool log_to_stderr_desired = FALSE;
	bool log_to_file_desired = FALSE;

	/*
	 * Start with the program name logger.
	 */
	pluto_name = argv[0];
	struct logger *logger = string_logger(null_fd, HERE, "%s: ", pluto_name); /* must free */

	conffile = clone_str(IPSEC_CONF, "conffile in main()");
	coredir = clone_str(DEFAULT_RUNDIR, "coredir in main()");
	rundir = clone_str(DEFAULT_RUNDIR, "rundir");
	pluto_vendorid = clone_str(ipsec_version_vendorid(), "vendorid in main()");
#ifdef USE_DNSSEC
	pluto_dnssec_rootfile = clone_str(DEFAULT_DNSSEC_ROOTKEY_FILE, "root.key file");
#endif
	pluto_lock_filename = clone_str(DEFAULT_RUNDIR "/pluto.pid", "lock file");

	deltatime_t keep_alive = DELTATIME_INIT(0);

	/* Overridden by virtual_private= in ipsec.conf */
	char *virtual_private = NULL;

	/* handle arguments */
	for (;; ) {
		/*
		 * Note: we don't like the way short options get parsed
		 * by getopt_long, so we simply pass an empty string as
		 * the list.  It could be "hvdenp:l:s:" "NARXPECK".
		 */
		int longindex = -1;
		int c = getopt_long(argc, argv, "", long_opts, &longindex);
		if (c < 0)
			break;

		if (longindex >= 0) {
			passert(c != '?' && c != ':'); /* no error */
			const char *optname = long_opts[longindex].name;
			const char *optmeta = optname + strlen(optname) + 1;	/* after '\0' */
			switch (optmeta[0]) {
			case '_':
				llog(RC_LOG, logger,
					    "warning: option \"--%s\" with '_' in its name is obsolete; use '-'",
					    optname);
				break;
			case '>':
				llog(RC_LOG, logger,
					    "warning: option \"--%s\" is obsolete; use \"--%s\"", optname, optmeta + 1);
				break;
			case '!':
				llog(RC_LOG, logger,
					    "warning: option \"--%s\" is obsolete; ignored", optname);
				continue;	/* ignore it! */
			}
		}

		switch (c) {

		case 0:
			/*
			 * Long option already handled by getopt_long.
			 * Not currently used since we always set flag to NULL.
			 */
			passert_fail(logger, HERE, "unexpected 0 returned by getopt_long()");

		case ':':	/* diagnostic already printed by getopt_long */
		case '?':	/* diagnostic already printed by getopt_long */
			fprintf(stderr, "For usage information: %s --help\n", pluto_name);
			fprintf(stderr, "Libreswan %s\n", ipsec_version_code());
			exit(PLUTO_EXIT_FAIL);

		case 'h':	/* --help */
			usage(stdout); /* so <<| more>> works */
			exit(PLUTO_EXIT_OK);

		case 'X':	/* --leak-detective */
			/*
			 * This flag was already processed at the start of main()
			 * because leak_detective must be immutable from before
			 * the first alloc().
			 * If this option is specified, we must have already
			 * set it at the start of main(), so assert it.
			 */
			passert(leak_detective);
			continue;

		case 'C':	/* --coredir */
			pfree(coredir);
			coredir = clone_str(optarg, "coredir via getopt");
			continue;

		case 'V':	/* --vendorid */
			pfree(pluto_vendorid);
			pluto_vendorid = clone_str(optarg, "pluto_vendorid via getopt");
			continue;

		case 'S':	/* --statsdir */
			pfreeany(pluto_stats_binary);
			pluto_stats_binary = clone_str(optarg, "statsbin");
			continue;

		case 'v':	/* --version */
			printf("%s%s\n", ipsec_version_string(), /* ok */
			       compile_time_interop_options);
			/* not exit_pluto because we are not initialized yet */
			exit(PLUTO_EXIT_OK);

		case 'j':	/* --nhelpers */
			if (streq(optarg, "-1")) {
				nhelpers = -1;
			} else {
				unsigned long u;
				check_err(ttoulb(optarg, 0, 10, 1000, &u),
					  longindex, logger);
				nhelpers = u;
			}
			continue;

		case 'c':	/* --seedbits */
			pluto_nss_seedbits = atoi(optarg);
			if (pluto_nss_seedbits == 0) {
				llog(RC_LOG, logger, "seedbits must be an integer > 0");
				/* not exit_pluto because we are not initialized yet */
				exit(PLUTO_EXIT_NSS_FAIL);
			}
			continue;

#ifdef HAVE_LABELED_IPSEC
		case 'w':	/* --secctx-attr-type */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 0, 0xFFFF, &u),
				  longindex, logger);
			if (u != SECCTX && u != ECN_TUNNEL_or_old_SECCTX) {
				fatal_opt(longindex, logger,
					  "must be a positive 32001 (default) or 10 (for backward compatibility)");
			}
			secctx_attr_type = u;
			continue;
		}
#endif

		case 'k':	/* --ikev1-reject */
			pluto_ikev1_pol = GLOBAL_IKEv1_REJECT;
			continue;

		case 'l':	/* --ikev1-drop */
			pluto_ikev1_pol = GLOBAL_IKEv1_DROP;
			continue;

		case '0':	/* --nofork*/
			fork_desired = FALSE;
			continue;

		case 'e':	/* --stderrlog */
			log_to_stderr_desired = TRUE;
			continue;

		case 'g':	/* --logfile */
			pluto_log_file = clone_str(optarg, "pluto_log_file");
			log_to_file_desired = TRUE;
			continue;
#ifdef USE_DNSSEC
		case OPT_DNSSEC_ROOTKEY_FILE:	/* --dnssec-rootkey-file */
			if (optarg[0] != '\0') {
				pfree(pluto_dnssec_rootfile);
				pluto_dnssec_rootfile = clone_str(optarg,
						"dnssec_rootkey_file");
			}
			continue;

		case OPT_DNSSEC_TRUSTED:	/* --dnssec-trusted */
			pluto_dnssec_trusted = clone_str(optarg, "pluto_dnssec_trusted");
			continue;
#endif  /* USE_DNSSEC */

		case 't':	/* --log-no-time */
			log_param.log_with_timestamp = FALSE;
			continue;

		case '7':	/* --log-no-append */
			log_append = FALSE;
			continue;

		case '<':	/* --log-no-ip */
			log_ip = FALSE;
			continue;

		case 'a':	/* --log-no-audit */
			log_to_audit = FALSE;
			continue;

		case '8':	/* --drop-oppo-null */
			pluto_drop_oppo_null = TRUE;
			continue;

		case '9':	/* --expire-shunt-interval <interval> */
		{
			unsigned long d = 0;
			check_err(ttoulb(optarg, 0, 10, 1000, &d), longindex, logger);
			bare_shunt_interval = deltatime(d);
			continue;
		}

		case 'L':	/* --listen ip_addr */
		{
			ip_address lip;
			err_t e = ttoaddress_num(shunk1(optarg), NULL/*UNSPEC*/, &lip);

			if (e != NULL) {
				/*
				 * ??? should we continue on failure?
				 */
				llog(RC_LOG, logger,
					    "invalid listen argument ignored: %s\n", e);
			} else {
				pluto_listen = clone_str(optarg, "pluto_listen");
				llog(RC_LOG, logger,
					    "bind() will be filtered for %s\n",
					    pluto_listen);
			}
			continue;
		}

		case 'F':	/* --use-bsdkame */
#ifdef BSDKAME_SUPPORT
			kernel_ops = &bsdkame_kernel_ops;
#else
			llog(RC_LOG, logger, "--use-bsdkame not supported");
#endif
			continue;

		case 'K':	/* --use-netkey */
#ifdef XFRM_SUPPORT
			kernel_ops = &xfrm_kernel_ops;
#else
			llog(RC_LOG, logger, "--use-xfrm not supported");
#endif
			continue;

		case 'D':	/* --force-busy */
			pluto_ddos_mode = DDOS_FORCE_BUSY;
			continue;
		case 'U':	/* --force-unlimited */
			pluto_ddos_mode = DDOS_FORCE_UNLIMITED;
			continue;

#ifdef HAVE_SECCOMP
		case '3':	/* --seccomp-enabled */
			pluto_seccomp_mode = SECCOMP_ENABLED;
			continue;
		case '4':	/* --seccomp-tolerant */
			pluto_seccomp_mode = SECCOMP_TOLERANT;
			continue;
#endif

		case 'Z':	/* --curl-iface */
			curl_iface = clone_str(optarg, "curl_iface");
			continue;

		case 'I':	/* --curl-timeout */
		{
			unsigned long u;
			check_err(ttoulb(optarg, /*not lower-bound*/0, 10, 0xFFFF, &u),
				  longindex, logger);
			if (u == 0) {
				fatal_opt(longindex, logger, "must not be < 1");
			}
			curl_timeout = u;
			continue;
		}

		case 'r':	/* --crl-strict */
			crl_strict = TRUE;
			continue;

		case 'x':	/* --crlcheckinterval <seconds> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, /*not lower-bound*/0, 10, (unsigned long) TIME_T_MAX, &u),
				  longindex, logger);
			crl_check_interval = deltatime(u);
			continue;
		}

		case 'o':
			ocsp_strict = TRUE;
			continue;

		case 'O':
			ocsp_enable = TRUE;
			continue;

		case 'Y':
			ocsp_uri = clone_str(optarg, "ocsp_uri");
			continue;

		case 'J':
			ocsp_trust_name = clone_str(optarg, "ocsp_trust_name");
			continue;

		case 'T':	/* --ocsp_timeout <seconds> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, /*not-lower-bound*/0, 10, 0xFFFF, &u),
				  longindex, logger);
			if (u == 0) {
				fatal_opt(longindex, logger, "must not be 0");
				continue;
			}
			ocsp_timeout = u;
			continue;
		}

		case 'E':	/* --ocsp-cache-size <entries> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 10, 0xFFFF, &u), longindex, logger);
			ocsp_cache_size = u;
			continue;
		}

		case 'G':	/* --ocsp-cache-min-age <seconds> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 10, 0xFFFF, &u), longindex, logger);
			ocsp_cache_min_age = u;
			continue;
		}

		case 'H':	/* --ocsp-cache-max-age <seconds> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 10, 0xFFFF, &u), longindex, logger);
			ocsp_cache_max_age = u;
			continue;
		}

		case 'B':	/* --ocsp-method get|post */
			if (streq(optarg, "post")) {
				ocsp_method = OCSP_METHOD_POST;
				ocsp_post = TRUE;
			} else {
				if (streq(optarg, "get")) {
					ocsp_method = OCSP_METHOD_GET;
				} else {
					fatal_opt(longindex, logger, "ocsp-method is either 'post' or 'get'");
				}
			}
			continue;

		case 'u':	/* --uniqueids */
			uniqueIDs = TRUE;
			continue;

		case 'R':	/* --no-dnssec */
			do_dnssec = FALSE;
			continue;

		case 'i':	/* --interface <ifname|ifaddr> */
			if (!use_interface(optarg)) {
				fatal_opt(longindex, logger, "too many --interface specifications");
			}
			continue;

		case '1':	/* --ike-socket-no-errqueue */
			pluto_sock_errqueue = FALSE;
			continue;

		case 'W':	/* --ike-socket-bufsize <bufsize> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 10, 0xFFFF, &u), longindex, logger);
			if (u == 0) {
				fatal_opt(longindex, logger, "must not be 0");
			}
			pluto_sock_bufsize = u;
			continue;
		}

		case 'p':	/* --no-listen-udp */
			pluto_listen_udp = FALSE;
			continue;

		case 'm':	/* --listen-tcp */
			pluto_listen_tcp = TRUE;
			continue;

		case 'b':	/* --rundir <path> */
			/*
			 * ??? work to be done here:
			 *
			 * snprintf returns the required space if there
			 * isn't enough, not -1.
			 * -1 indicates another kind of error.
			 */
			if (snprintf(ctl_addr.sun_path, sizeof(ctl_addr.sun_path),
				     "%s/pluto.ctl", optarg) == -1) {
				fatal_opt(longindex, logger, "--rundir argument is invalid for sun_path socket");
			}

			pfree(pluto_lock_filename);
			pluto_lock_filename = alloc_printf("%s/pluto.pid", optarg);
			pfreeany(rundir);
			rundir = clone_str(optarg, "rundir");
			continue;

		case 's':	/* --secretsfile <secrets-file> */
			lsw_conf_secretsfile(optarg);
			continue;

		case 'f':	/* --ipsecdir <ipsec-dir> */
			lsw_conf_confddir(optarg, logger);
			continue;

		case 'd':	/* --nssdir <path> */
			lsw_conf_nssdir(optarg, logger);
			continue;

		case 'N':	/* --debug-none */
			cur_debugging = DBG_NONE;
			continue;

		case 'A':	/* --debug-all */
			cur_debugging = DBG_ALL;
			continue;

		case 'y':	/* --global-redirect-to */
		{
			ip_address rip;
			check_err(ttoaddress_dns(shunk1(optarg), NULL/*UNSPEC*/, &rip),
				  longindex, logger);
			set_global_redirect_dests(optarg);
			llog(RC_LOG, logger,
				    "all IKE_SA_INIT requests will from now on be redirected to: %s\n",
				    optarg);
			continue;
		}

		case 'Q':	/* --global-redirect */
		{
			if (streq(optarg, "yes")) {
				global_redirect = GLOBAL_REDIRECT_YES;
			} else if (streq(optarg, "no")) {
				global_redirect = GLOBAL_REDIRECT_NO;
			} else if (streq(optarg, "auto")) {
				global_redirect = GLOBAL_REDIRECT_AUTO;
			} else {
				llog(RC_LOG, logger,
					    "invalid option argument for global-redirect (allowed arguments: yes, no, auto)");
			}
			continue;
		}

		case '2':	/* --keep-alive <delay_secs> */
		{
			unsigned long u;
			check_err(ttoulb(optarg, 0, 10, secs_per_day, &u), longindex, logger);
			keep_alive = deltatime(u);
			continue;
		}

		case '5':	/* --selftest */
			selftest_only = TRUE;
			log_to_stderr_desired = TRUE;
			log_param.log_with_timestamp = FALSE;
			fork_desired = FALSE;
			continue;

		case '6':	/* --virtual-private */
			virtual_private = clone_str(optarg, "virtual_private");
			continue;

		case 'z':	/* --config */
		{
			/*
			 * Config struct to variables mapper. This will
			 * overwrite all previously set options. Keep this
			 * in the same order as long_opts[] is.
			 */
			pfree(conffile);
			conffile = clone_str(optarg, "conffile via getopt");
			/* may not return */
			struct starter_config *cfg = read_cfg_file(conffile, longindex, logger);

			/* leak */
			set_cfg_string(&pluto_log_file,
				       cfg->setup.strings[KSF_LOGFILE]);
#ifdef USE_DNSSEC
			set_dnssec_file_names(cfg);
#endif

			if (pluto_log_file != NULL)
				log_to_syslog = FALSE;
			/* plutofork= no longer supported via config file */
			log_param.log_with_timestamp = cfg->setup.options[KBF_LOGTIME];
			log_append = cfg->setup.options[KBF_LOGAPPEND];
			log_ip = cfg->setup.options[KBF_LOGIP];
			log_to_audit = cfg->setup.options[KBF_AUDIT_LOG];
			pluto_drop_oppo_null = cfg->setup.options[KBF_DROP_OPPO_NULL];
			pluto_ddos_mode = cfg->setup.options[KBF_DDOS_MODE];
			pluto_ikev1_pol = cfg->setup.options[KBF_GLOBAL_IKEv1];
#ifndef USE_IKEv1
			if (pluto_ikev1_pol != GLOBAL_IKEv1_ACCEPT) {
				llog(RC_LOG, logger, "ignoring ikev1-policy= as IKEv1 support is not compiled in. Incoming IKEv1 packets are silently dropped");
				pluto_ikev1_pol = GLOBAL_IKEv1_DROP;
			}
#endif
#ifdef HAVE_SECCOMP
			pluto_seccomp_mode = cfg->setup.options[KBF_SECCOMP];
#endif
			if (cfg->setup.options[KBF_FORCEBUSY]) {
				/* force-busy is obsoleted, translate to ddos-mode= */
				pluto_ddos_mode = cfg->setup.options[KBF_DDOS_MODE] = DDOS_FORCE_BUSY;
			}
			/* ddos-ike-threshold and max-halfopen-ike */
			pluto_ddos_threshold = cfg->setup.options[KBF_DDOS_IKE_THRESHOLD];
			pluto_max_halfopen = cfg->setup.options[KBF_MAX_HALFOPEN_IKE];

			crl_strict = cfg->setup.options[KBF_CRL_STRICT];

			pluto_shunt_lifetime = deltatime(cfg->setup.options[KBF_SHUNTLIFETIME]);

			ocsp_enable = cfg->setup.options[KBF_OCSP_ENABLE];
			ocsp_strict = cfg->setup.options[KBF_OCSP_STRICT];
			ocsp_timeout = cfg->setup.options[KBF_OCSP_TIMEOUT];
			ocsp_method = cfg->setup.options[KBF_OCSP_METHOD];
			ocsp_post = (ocsp_method == OCSP_METHOD_POST);
			ocsp_cache_size = cfg->setup.options[KBF_OCSP_CACHE_SIZE];
			ocsp_cache_min_age = cfg->setup.options[KBF_OCSP_CACHE_MIN];
			ocsp_cache_max_age = cfg->setup.options[KBF_OCSP_CACHE_MAX];

			set_cfg_string(&ocsp_uri,
				       cfg->setup.strings[KSF_OCSP_URI]);
			set_cfg_string(&ocsp_trust_name,
				       cfg->setup.strings[KSF_OCSP_TRUSTNAME]);

			char *tmp_global_redirect = cfg->setup.strings[KSF_GLOBAL_REDIRECT];
			if (tmp_global_redirect == NULL || streq(tmp_global_redirect, "no")) {
				/* NULL means it is not specified so default is no */
				global_redirect = GLOBAL_REDIRECT_NO;
			} else if (streq(tmp_global_redirect, "yes")) {
				global_redirect = GLOBAL_REDIRECT_YES;
			} else if (streq(tmp_global_redirect, "auto")) {
				global_redirect = GLOBAL_REDIRECT_AUTO;
			} else {
				global_redirect = GLOBAL_REDIRECT_NO;
				llog(RC_LOG, logger, "unknown argument for global-redirect option");
			}

			crl_check_interval = deltatime(
				cfg->setup.options[KBF_CRL_CHECKINTERVAL]);
			uniqueIDs = cfg->setup.options[KBF_UNIQUEIDS];
#ifdef USE_DNSSEC
			do_dnssec = cfg->setup.options[KBF_DO_DNSSEC];
#else
			do_dnssec = FALSE;
#endif
			/*
			 * We don't check interfaces= here because that part
			 * has been dealt with in _stackmanager before we
			 * started
			 */
			set_cfg_string(&pluto_listen,
				       cfg->setup.strings[KSF_LISTEN]);

			/* ike-socket-bufsize= */
			pluto_sock_bufsize = cfg->setup.options[KBF_IKEBUF];
			pluto_sock_errqueue = cfg->setup.options[KBF_IKE_ERRQUEUE];

			/* listen-tcp= / listen-udp= */
			pluto_listen_tcp = cfg->setup.options[KBF_LISTEN_TCP];
			pluto_listen_udp = cfg->setup.options[KBF_LISTEN_UDP];

			/* nflog-all= */
			/* only causes nflog nmber to show in ipsec status */
			pluto_nflog_group = cfg->setup.options[KBF_NFLOG_ALL];

			/* only causes nflog nmber to show in ipsec status */
			pluto_xfrmlifetime = cfg->setup.options[KBF_XFRMLIFETIME];

			/* no config option: rundir */
			/* secretsfile= */
			if (cfg->setup.strings[KSF_SECRETSFILE] &&
			    *cfg->setup.strings[KSF_SECRETSFILE]) {
				lsw_conf_secretsfile(cfg->setup.strings[KSF_SECRETSFILE]);
			}
			if (cfg->setup.strings[KSF_IPSECDIR] != NULL &&
			    *cfg->setup.strings[KSF_IPSECDIR] != 0) {
				/* ipsecdir= */
				lsw_conf_confddir(cfg->setup.strings[KSF_IPSECDIR], logger);
			}

			if (cfg->setup.strings[KSF_NSSDIR] != NULL &&
			    *cfg->setup.strings[KSF_NSSDIR] != 0) {
				/* nssdir= */
				lsw_conf_nssdir(cfg->setup.strings[KSF_NSSDIR], logger);
			}

			if (cfg->setup.strings[KSF_CURLIFACE]) {
				pfreeany(curl_iface);
				/* curl-iface= */
				curl_iface = clone_str(cfg->setup.strings[KSF_CURLIFACE],
						       "curl-iface= via --config");
			}

			if (cfg->setup.options[KBF_CURLTIMEOUT])
				curl_timeout = cfg->setup.options[KBF_CURLTIMEOUT];

			if (cfg->setup.strings[KSF_DUMPDIR]) {
				pfree(coredir);
				/* dumpdir= */
				coredir = clone_str(cfg->setup.strings[KSF_DUMPDIR],
						    "coredir via --config");
			}
			/* vendorid= */
			if (cfg->setup.strings[KSF_MYVENDORID]) {
				pfree(pluto_vendorid);
				pluto_vendorid = clone_str(cfg->setup.strings[KSF_MYVENDORID],
							   "pluto_vendorid via --config");
			}

			if (cfg->setup.strings[KSF_STATSBINARY] != NULL) {
				if (access(cfg->setup.strings[KSF_STATSBINARY], X_OK) == 0) {
					pfreeany(pluto_stats_binary);
					/* statsbin= */
					pluto_stats_binary = clone_str(cfg->setup.strings[KSF_STATSBINARY], "statsbin via --config");
					llog(RC_LOG, logger, "statsbinary set to %s", pluto_stats_binary);
				} else {
					llog(RC_LOG, logger, "statsbinary= '%s' ignored - file does not exist or is not executable",
						    pluto_stats_binary);
				}
			}

			pluto_nss_seedbits = cfg->setup.options[KBF_SEEDBITS];
			keep_alive = deltatime(cfg->setup.options[KBF_KEEPALIVE]);

			set_cfg_string(&virtual_private,
				       cfg->setup.strings[KSF_VIRTUALPRIVATE]);

			set_global_redirect_dests(cfg->setup.strings[KSF_GLOBAL_REDIRECT_TO]);

			nhelpers = cfg->setup.options[KBF_NHELPERS];
			secctx_attr_type = cfg->setup.options[KBF_SECCTX];
			cur_debugging = cfg->setup.options[KBF_PLUTODEBUG];

			char *protostack = cfg->setup.strings[KSF_PROTOSTACK];
			passert(kernel_ops != NULL);

			if (!(protostack == NULL || *protostack == '\0')) {
				if (streq(protostack, "auto") || streq(protostack, "native") ||
				    streq(protostack, "nokernel")) {

					llog(RC_LOG, logger, "the option protostack=%s is obsoleted, falling back to protostack=%s",
						    protostack, kernel_ops->kern_name);
				} else if (streq(protostack, "klips")) {
					fatal(PLUTO_EXIT_KERNEL_FAIL, logger,
					      "protostack=klips is obsoleted, please migrate to protostack=xfrm");
#ifdef XFRM_SUPPORT
				} else if (streq(protostack, "xfrm") ||
					   streq(protostack, "netkey")) {
					kernel_ops = &xfrm_kernel_ops;
#endif
#ifdef BSD_KAME
				} else if (streq(protostack, "bsd") ||
					   streq(protostack, "kame") ||
					   streq(protostack, "bsdkame")) {
					kernel_ops = &bsdkame_kernel_ops;
#endif
				} else {
					llog(RC_LOG, logger, "protostack=%s ignored, using default protostack=%s",
						    protostack, kernel_ops->kern_name);
				}
			}

			confread_free(cfg);
			continue;
		}

		case OPT_EFENCE_PROTECT:
#ifdef USE_EFENCE
			/*
			 * This flag was already processed at the
			 * start of main().  While it isn't immutable,
			 * having it apply to all mallocs() is
			 * presumably better.
			 */
			passert(EF_PROTECT_BELOW);
			passert(EF_PROTECT_FREE);
#else
			llog(RC_LOG, logger, "efence support is not enabled; option --efence-protect ignored");
#endif
			continue;

		case OPT_DEBUG:
		{
			lmod_t mod = empty_lmod;
			if (lmod_arg(&mod, &debug_lmod_info, optarg, true/*enable*/)) {
				cur_debugging = lmod(cur_debugging, mod);
			} else {
				llog(RC_LOG, logger, "unrecognized --debug '%s' option ignored",
					    optarg);
			}
			continue;
		}

		case OPT_IMPAIR:
		{
			struct whack_impair impairment;
			switch (parse_impair(optarg, &impairment, true, logger)) {
			case IMPAIR_OK:
				if (!process_impair(&impairment, NULL, true, logger)) {
					fatal_opt(longindex, logger, "not valid from the command line");
				}
				continue;
			case IMPAIR_ERROR:
				/* parse_impair() printed error */
				exit(PLUTO_EXIT_FAIL);
			case IMPAIR_HELP:
				/* parse_impair() printed error */
				exit(PLUTO_EXIT_OK);
			}
			continue;
		}

		default:
			bad_case(c);
		}
	}

	/*
	 * Anything (aka an argument) after all options consumed?
	 */
	if (optind != argc) {
		llog(RC_LOG, logger, "unexpected trailing argument: %s", argv[optind]);
		/* not exit_pluto because we are not initialized yet */
		exit(PLUTO_EXIT_FAIL);
	}

	if (chdir(coredir) == -1) {
		int e = errno;

		llog(RC_LOG, logger, "pluto: warning: chdir(\"%s\") to dumpdir failed (%d: %s)",
			coredir, e, strerror(e));
	}

	oco = lsw_init_options();

	if (!selftest_only)
		lockfd = create_lock(logger);
	else
		lockfd = 0;

	/* select between logging methods */

	if (log_to_stderr_desired || log_to_file_desired)
		log_to_syslog = FALSE;
	if (!log_to_stderr_desired)
		log_to_stderr = FALSE;

	/*
	 * create control socket.
	 * We must create it before the parent process returns so that
	 * there will be no race condition in using it.  The easiest
	 * place to do this is before the daemon fork.
	 */
	if (!selftest_only) {
		diag_t d = init_ctl_socket(logger);
		if (d != NULL) {
			fatal_diag(PLUTO_EXIT_SOCKET_FAIL, logger, &d, "%s", "");
		}
	}

	/* If not suppressed, do daemon fork */
	if (fork_desired) {
#if USE_DAEMON
		if (daemon(TRUE, TRUE) < 0) {
			fatal_errno(PLUTO_EXIT_FORK_FAIL, logger, "daemon failed");
		}
		/*
		 * Parent just exits, so need to fill in our own PID
		 * file.  This is racy, since the file won't be
		 * created until after the parent has exited.
		 *
		 * Since "ipsec start" invokes pluto with --nofork, it
		 * is probably safer to leave this feature disabled
		 * then implement it using the daemon call.
		 */
		(void) fill_lock(lockfd, getpid());
#elif USE_FORK
		{
			pid_t pid = fork();

			if (pid < 0) {
				fatal_errno(PLUTO_EXIT_FORK_FAIL, logger, errno,
					    "fork failed");
			}

			if (pid == 0) {
				/*
				 * parent fills in the PID.
				 */
				close(lockfd);
			} else {
				/*
				 * parent: die, after filling PID into lock
				 * file.
				 * must not use exit_pluto: lock would be
				 * removed!
				 */
				exit(fill_lock(lockfd, pid) ? 0 : 1);
			}
		}
#else
		fatal(PLUTO_EXIT_FORK_FAIL, logger, "fork/daemon not supported; specify --nofork");
#endif
		if (setsid() < 0) {
			fatal_errno(PLUTO_EXIT_FAIL, logger, errno,
				    "setsid() failed in main()");
		}
	} else {
		/* no daemon fork: we have to fill in lock file */
		(void) fill_lock(lockfd, getpid());

		if (isatty(fileno(stdout))) {
			fprintf(stdout, "Pluto initialized\n");
			fflush(stdout);
		}
	}

	/*
	 * Close stdin, stdout, and when not needed, stderr.  This
	 * should just leave CTL_FD.
	 *
	 * Follow that by directing the closed file descriptors at
	 * /dev/null.  UNIX always uses the lowest file descriptor
	 * when opening a file.
	 */
	close(STDIN_FILENO);/*stdin*/
	close(STDOUT_FILENO);/*stdout*/
	if (!log_to_stderr) {
		close(STDERR_FILENO); /*stderr*/
	}
	/* make sure that stdin, stdout, stderr are reserved */
	passert(open("/dev/null", O_RDONLY) == STDIN_FILENO);
	/* open("/dev/null", O_WRONLY) == STDOUT_FILENO? */
	passert(dup2(0, STDOUT_FILENO) == STDOUT_FILENO);
	/* dup2(STDOUT_FILENO, STDERR_FILENO) == STDERR_FILENO? */
	passert(log_to_stderr || dup2(0, STDERR_FILENO) == STDERR_FILENO);

	/*
	 * Check for no unexpected file descriptors.
	 */
	if (DBGP(DBG_BASE)) {/* even set? */
		for (int fd = getdtablesize() - 1; fd >= 0; fd--) {
			if (fd == ctl_fd ||
			    fd == STDIN_FILENO ||
			    fd == STDOUT_FILENO ||
			    fd == STDERR_FILENO) {
				continue;
			}
			struct stat s;
			if (fstat(fd, &s) == 0) {
				llog(RC_LOG_SERIOUS, logger, "pluto: unexpected open file descriptor %d", fd);
			}
		}
	}

	/*
	 * Initialize logging then switch to the real logger.
	 */
	pluto_init_log(log_param);
	struct logger global_logger = GLOBAL_LOGGER(null_fd);
	/*
	 * The string_logger() dbg_alloc() message went down a rabit
	 * hole (aka the console) so fake one up here.
	 */
	dbg_alloc("logger", logger, HERE);
	free_logger(&logger, HERE);
	logger = &global_logger;

	init_constants();
	init_pluto_constants();

	pluto_init_nss(oco->nssdir, logger);
	if (libreswan_fipsmode()) {
		/*
		 * clear out --debug-crypt if set
		 *
		 * impairs are also not allowed but cannot come in via
		 * ipsec.conf, only whack
		 */
		if (cur_debugging & DBG_PRIVATE) {
			cur_debugging &= ~DBG_PRIVATE;
			llog(RC_LOG_SERIOUS, logger, "FIPS mode: debug-private disabled as such logging is not allowed");
		}
		/*
		 * clear out --debug-crypt if set
		 *
		 * impairs are also not allowed but cannot come in via
		 * ipsec.conf, only whack
		 */
		if (cur_debugging & DBG_CRYPT) {
			cur_debugging &= ~DBG_CRYPT;
			llog(RC_LOG_SERIOUS, logger, "FIPS mode: debug-crypt disabled as such logging is not allowed");
		}
	}

	init_crypt_symkey(logger);

	/*
	 * If impaired, force the mode change; and verify the
	 * consequences.  Always run the tests as combinations such as
	 * NSS in fips mode but as out of it could be bad.
	 */

	if (impair.force_fips) {
		llog(RC_LOG, logger, "IMPAIR: forcing FIPS checks to true to emulate FIPS mode");
		lsw_set_fips_mode(LSW_FIPS_ON);
	}

	bool nss_fips_mode = PK11_IsFIPS();
	if (libreswan_fipsmode()) {
		llog(RC_LOG, logger, "FIPS mode enabled for pluto daemon");
		if (nss_fips_mode) {
			llog(RC_LOG, logger, "NSS library is running in FIPS mode");
		} else {
			fatal(PLUTO_EXIT_FIPS_FAIL, logger, "pluto in FIPS mode but NSS library is not");
		}
	} else {
		llog(RC_LOG, logger, "FIPS mode disabled for pluto daemon");
		if (nss_fips_mode) {
			llog(RC_LOG_SERIOUS, logger, "Warning: NSS library is running in FIPS mode");
		}
	}

	if (ocsp_enable) {
		/* may not return */
		diag_t d = init_nss_ocsp(ocsp_uri, ocsp_trust_name,
					 ocsp_timeout, ocsp_strict, ocsp_cache_size,
					 ocsp_cache_min_age, ocsp_cache_min_age,
					 (ocsp_method == OCSP_METHOD_POST), logger);
		if (d != NULL) {
			fatal_diag(PLUTO_EXIT_NSS_FAIL, logger, &d, "initializing NSS OCSP failed: ");
		}
		llog(RC_LOG, logger, "NSS OCSP started");
	}

#ifdef FIPS_CHECK
	llog(RC_LOG, logger, "FIPS HMAC integrity support [enabled]");
	bool fips_files = FIPSCHECK_verify_files(fips_package_files);
	if (fips_files) {
		llog(RC_LOG, logger, "FIPS HMAC integrity verification self-test passed");
	} else if (libreswan_fipsmode()) {
		fatal(PLUTO_EXIT_FIPS_FAIL, logger, "FIPS HMAC integrity verification self-test FAILED");
	} else {
		llog(RC_LOG_SERIOUS, logger, "FIPS HMAC integrity verification self-test FAILED");
	}
#else
	llog(RC_LOG, logger, "FIPS HMAC integrity support [disabled]");
#endif

#ifdef HAVE_LIBCAP_NG
	/*
	 * Drop capabilities - this generates a false positive valgrind warning
	 * See: http://marc.info/?l=linux-security-module&m=125895232029657
	 *
	 * We drop these after creating the pluto socket or else we can't
	 * create a socket if the parent dir is non-root (eg openstack)
	 */
	capng_clear(CAPNG_SELECT_BOTH);

	capng_updatev(CAPNG_ADD, CAPNG_EFFECTIVE | CAPNG_PERMITTED,
		CAP_NET_BIND_SERVICE, CAP_NET_ADMIN, CAP_NET_RAW,
		CAP_IPC_LOCK, CAP_AUDIT_WRITE,
		/* for google authenticator pam */
		CAP_SETGID, CAP_SETUID,
		CAP_DAC_READ_SEARCH,
		-1);
	/*
	 * We need to retain some capabilities for our children (updown):
	 * CAP_NET_ADMIN to change routes
	 * (we also need it for some setsockopt() calls in main process)
	 * CAP_NET_RAW for iptables -t mangle
	 * CAP_DAC_READ_SEARCH for pam / google authenticator
	 *
	 */
	capng_updatev(CAPNG_ADD, CAPNG_BOUNDING_SET, CAP_NET_ADMIN, CAP_NET_RAW,
			CAP_DAC_READ_SEARCH, -1);
	capng_apply(CAPNG_SELECT_BOTH);
	llog(RC_LOG, logger, "libcap-ng support [enabled]");
#else
	llog(RC_LOG, logger, "libcap-ng support [disabled]");
#endif

#ifdef USE_LINUX_AUDIT
	linux_audit_init(log_to_audit, logger);
#else
	llog(RC_LOG, logger, "Linux audit support [disabled]");
#endif

	{
		const char *vc = ipsec_version_code();
		llog(RC_LOG, logger, "Starting Pluto (Libreswan Version %s%s) pid:%u",
			vc, compile_time_interop_options, getpid());
	}

	llog(RC_LOG, logger, "core dump dir: %s", coredir);
	if (oco->secretsfile && *oco->secretsfile)
		llog(RC_LOG, logger, "secrets file: %s", oco->secretsfile);

	llog(RC_LOG, logger, leak_detective ?
		"leak-detective enabled" : "leak-detective disabled");

	llog(RC_LOG, logger, "NSS crypto [enabled]");

#ifdef USE_PAM_AUTH
	llog(RC_LOG, logger, "XAUTH PAM support [enabled]");
#else
	llog(RC_LOG, logger, "XAUTH PAM support [disabled]");
#endif

	/*
	 * Log impair-* functions that were enabled
	 */
	if (have_impairments()) {
		LLOG_JAMBUF(RC_LOG, logger, buf) {
			jam(buf, "Warning: impairments enabled: ");
			jam_impairments(buf, "+");
		}
	}

/* Initialize all of the various features */

	init_state_db();
	init_connection_db();
	init_server_fork();
	init_server(logger);

	init_rate_log();
	init_nat_traversal(keep_alive, logger);

	init_virtual_ip(virtual_private, logger);
	/* obsoleted by nss code: init_rnd_pool(); */
	init_root_certs();
	init_secret(logger);
#ifdef USE_IKEv1
	init_ikev1(logger);
#endif
	init_ikev2();
	init_states();
	init_revival();
	init_connections();
	init_host_pair();
	init_ike_alg(logger);
	test_ike_alg(logger);

	if (selftest_only) {
		/*
		 * skip pluto_exit()
		 * Not all components were initialized and
		 * no lock files were created.
		 */
		exit(PLUTO_EXIT_OK);
	}

	start_server_helpers(nhelpers, logger);
	init_kernel(logger);
	init_vendorid(logger);
#if defined(LIBCURL) || defined(LIBLDAP)
	start_crl_fetch_helper(logger);
#endif
	init_labeled_ipsec(logger);
#ifdef USE_SYSTEMD_WATCHDOG
	pluto_sd_init(logger);
#endif

#ifdef USE_DNSSEC
	{
		diag_t d = unbound_event_init(get_pluto_event_base(), do_dnssec,
					      pluto_dnssec_rootfile, pluto_dnssec_trusted,
					      logger/*for-warnings*/);
		if (d != NULL) {
			fatal_diag(PLUTO_EXIT_UNBOUND_FAIL, logger, &d, "%s", "");
		}
	}
#endif

	run_server(conffile, logger);
}

void show_setup_plutomain(struct show *s)
{
	show_separator(s);
	show_comment(s, "config setup options:");
	show_separator(s);
	show_comment(s, "configdir=%s, configfile=%s, secrets=%s, ipsecdir=%s",
		oco->confdir,
		conffile, /* oco contains only a copy of hardcoded default */
		oco->secretsfile,
		oco->confddir);

	show_comment(s, "nssdir=%s, dumpdir=%s, statsbin=%s",
		oco->nssdir,
		coredir,
		pluto_stats_binary == NULL ? "unset" :  pluto_stats_binary);

#ifdef USE_DNSSEC
	show_comment(s, "dnssec-rootkey-file=%s, dnssec-trusted=%s",
		pluto_dnssec_rootfile == NULL ? "<unset>" : pluto_dnssec_rootfile,
		pluto_dnssec_trusted == NULL ? "<unset>" : pluto_dnssec_trusted);
#endif

	show_comment(s, "sbindir=%s, libexecdir=%s",
		IPSEC_SBINDIR,
		IPSEC_EXECDIR);

	show_comment(s, "pluto_version=%s, pluto_vendorid=%s, audit-log=%s",
		ipsec_version_code(),
		pluto_vendorid,
		bool_str(log_to_audit));

	show_comment(s,
		"nhelpers=%d, uniqueids=%s, "
		"dnssec-enable=%s, "
		"logappend=%s, logip=%s, shuntlifetime=%jds, xfrmlifetime=%jds",
		nhelpers,
		bool_str(uniqueIDs),
		bool_str(do_dnssec),
		bool_str(log_append),
		bool_str(log_ip),
		deltasecs(pluto_shunt_lifetime),
		(intmax_t) pluto_xfrmlifetime
	);

	show_comment(s,
		"ddos-cookies-threshold=%d, ddos-max-halfopen=%d, ddos-mode=%s, ikev1-policy=%s",
		pluto_ddos_threshold,
		pluto_max_halfopen,
		(pluto_ddos_mode == DDOS_AUTO) ? "auto" :
			(pluto_ddos_mode == DDOS_FORCE_BUSY) ? "busy" : "unlimited",
		pluto_ikev1_pol == GLOBAL_IKEv1_ACCEPT ? "accept" :
			pluto_ikev1_pol == GLOBAL_IKEv1_REJECT ? "reject" : "drop");

	show_comment(s,
		"ikebuf=%d, msg_errqueue=%s, crl-strict=%s, crlcheckinterval=%jd, listen=%s, nflog-all=%d",
		pluto_sock_bufsize,
		bool_str(pluto_sock_errqueue),
		bool_str(crl_strict),
		deltasecs(crl_check_interval),
		pluto_listen != NULL ? pluto_listen : "<any>",
		pluto_nflog_group
		);

	show_comment(s,
		"ocsp-enable=%s, ocsp-strict=%s, ocsp-timeout=%d, ocsp-uri=%s",
		bool_str(ocsp_enable),
		bool_str(ocsp_strict),
		ocsp_timeout,
		ocsp_uri != NULL ? ocsp_uri : "<unset>"
		);
	show_comment(s,
		"ocsp-trust-name=%s",
		ocsp_trust_name != NULL ? ocsp_trust_name : "<unset>"
		);

	show_comment(s,
		"ocsp-cache-size=%d, ocsp-cache-min-age=%d, ocsp-cache-max-age=%d, ocsp-method=%s",
		ocsp_cache_size, ocsp_cache_min_age, ocsp_cache_max_age,
		ocsp_method == OCSP_METHOD_GET ? "get" : "post"
		);

	show_comment(s,
		"global-redirect=%s, global-redirect-to=%s",
		enum_name(&allow_global_redirect_names, global_redirect),
		strlen(global_redirect_to()) > 0 ? global_redirect_to() : "<unset>"
		);

#ifdef HAVE_LABELED_IPSEC
	show_comment(s, "secctx-attr-type=%d", secctx_attr_type);
#else
	show_comment(s, "secctx-attr-type=<unsupported>");
#endif
}
