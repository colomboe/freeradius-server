/*
 * radiusd.c	Main loop of the radius server.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000-2012  The FreeRADIUS server project
 * Copyright 1999,2000  Miquel van Smoorenburg <miquels@cistron.nl>
 * Copyright 2000  Alan DeKok <aland@ox.org>
 * Copyright 2000  Alan Curry <pacman-radius@cqc.com>
 * Copyright 2000  Jeff Carneal <jeff@apex.net>
 * Copyright 2000  Chad Miller <cmiller@surfsouth.com>
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

#include <sys/file.h>

#include <fcntl.h>
#include <ctype.h>

#ifdef HAVE_GETOPT_H
#	include <getopt.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#	include <sys/wait.h>
#endif
#ifndef WEXITSTATUS
#	define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
#endif
#ifndef WIFEXITED
#	define WIFEXITED(stat_val) (((stat_val) & 255) == 0)
#endif

/*
 *  Global variables.
 */
char const *progname = NULL;
char const *radius_dir = NULL;
char const *radacct_dir = NULL;
char const *radlog_dir = NULL;
char const *radlib_dir = NULL;
bool log_stripped_names;
log_debug_t debug_flag = 0;
bool check_config = false;

char const *radiusd_version = "FreeRADIUS Version " RADIUSD_VERSION_STRING
#ifdef RADIUSD_VERSION_COMMIT
" (git #" STRINGIFY(RADIUSD_VERSION_COMMIT) ")"
#endif
", for host " HOSTINFO ", built on " __DATE__ " at " __TIME__;

pid_t radius_pid;

/*
 *  Configuration items.
 */

/*
 *	Static functions.
 */
static void usage(int);

static void sig_fatal (int);
#ifdef SIGHUP
static void sig_hup (int);
#endif

#ifdef WITH_VERIFY_PTR
static void die_horribly(char const *reason)
{
	ERROR("talloc abort: %s\n", reason);
	abort();
}
#endif

/*
 *	The main guy.
 */
int main(int argc, char *argv[])
{
	int rcode = EXIT_SUCCESS;
	int status;
	int argval;
	bool spawn_flag = true;
	bool dont_fork = false;
	bool write_pid = false;
	int flag = 0;
	int from_child[2] = {-1, -1};

	int devnull;

	/*
	 *	If the server was built with debugging enabled always install
	 *	the basic fatal signal handlers.
	 */
#ifndef NDEBUG
	fr_fault_setup(getenv("PANIC_ACTION"), argv[0]);
#endif

#ifdef OSFC2
	set_auth_parameters(argc,argv);
#endif

	if ((progname = strrchr(argv[0], FR_DIR_SEP)) == NULL)
		progname = argv[0];
	else
		progname++;

#ifdef WIN32
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 0), &wsaData)) {
			fprintf(stderr, "%s: Unable to initialize socket library.\n", progname);
			exit(EXIT_FAILURE);
		}
	}
#endif

	debug_flag = 0;
	radius_dir = talloc_strdup(NULL, RADIUS_DIR);

	/*
	 *	Ensure that the configuration is initialized.
	 */
	memset(&mainconfig, 0, sizeof(mainconfig));
	mainconfig.myip.af = AF_UNSPEC;
	mainconfig.port = -1;
	mainconfig.name = "radiusd";

	/*
	 *	Don't put output anywhere until we get told a little
	 *	more.
	 */
	default_log.dest = L_DST_NULL;
	default_log.fd = -1;
	mainconfig.log_file = NULL;

	/*  Process the options.  */
	while ((argval = getopt(argc, argv, "Cd:D:fhi:l:mMn:p:PstvxX")) != EOF) {

		switch(argval) {
			case 'C':
				check_config = true;
				spawn_flag = false;
				dont_fork = true;
				break;

			case 'd':
				if (radius_dir) {
					rad_const_free(radius_dir);
				}
				radius_dir = talloc_strdup(NULL, optarg);
				break;

			case 'D':
				mainconfig.dictionary_dir = talloc_strdup(NULL, optarg);
				break;

			case 'f':
				dont_fork = true;
				break;

			case 'h':
				usage(0);
				break;

			case 'l':
				if (strcmp(optarg, "stdout") == 0) {
					goto do_stdout;
				}
				mainconfig.log_file = strdup(optarg);
				default_log.dest = L_DST_FILES;
				default_log.fd = open(mainconfig.log_file,
							    O_WRONLY | O_APPEND | O_CREAT, 0640);
				if (default_log.fd < 0) {
					fprintf(stderr, "radiusd: Failed to open log file %s: %s\n", mainconfig.log_file, fr_syserror(errno));
					exit(EXIT_FAILURE);
				}
				fr_log_fp = fdopen(default_log.fd, "a");
				break;

			case 'i':
				if (ip_hton(optarg, AF_UNSPEC, &mainconfig.myip) < 0) {
					fprintf(stderr, "radiusd: Invalid IP Address or hostname \"%s\"\n", optarg);
					exit(EXIT_FAILURE);
				}
				flag |= 1;
				break;

			case 'n':
				mainconfig.name = optarg;
				break;

			case 'm':
				mainconfig.debug_memory = true;
				break;

			case 'M':
				mainconfig.memory_report = true;
				mainconfig.debug_memory = true;
				break;

			case 'p':
				mainconfig.port = atoi(optarg);
				if ((mainconfig.port <= 0) ||
				    (mainconfig.port >= 65536)) {
					fprintf(stderr, "radiusd: Invalid port number %s\n", optarg);
					exit(EXIT_FAILURE);
				}
				flag |= 2;
				break;

			case 'P':
				/* Force the PID to be written, even in -f mode */
				write_pid = true;
				break;

			case 's':	/* Single process mode */
				spawn_flag = false;
				dont_fork = true;
				break;

			case 't':	/* no child threads */
				spawn_flag = false;
				break;

			case 'v':
				/* Don't print timestamps */
				debug_flag += 2;
				fr_log_fp = stdout;
				default_log.dest = L_DST_STDOUT;
				default_log.fd = STDOUT_FILENO;

				version();
				exit(EXIT_SUCCESS);
			case 'X':
				spawn_flag = false;
				dont_fork = true;
				debug_flag += 2;
				mainconfig.log_auth = true;
				mainconfig.log_auth_badpass = true;
				mainconfig.log_auth_goodpass = true;
		do_stdout:
				fr_log_fp = stdout;
				default_log.dest = L_DST_STDOUT;
				default_log.fd = STDOUT_FILENO;
				break;

			case 'x':
				debug_flag++;
				break;

			default:
				usage(1);
				break;
		}
	}

	if (mainconfig.memory_report) {
		talloc_enable_null_tracking();
#ifdef WITH_VERIFY_PTR
		talloc_set_abort_fn(die_horribly);
#endif
	}
	talloc_set_log_fn(log_talloc);

	/*
	 *	Mismatch between the binary and the libraries it depends on
	 */
	if (fr_check_lib_magic(RADIUSD_MAGIC_NUMBER) < 0) {
		fr_perror("radiusd");
		exit(EXIT_FAILURE);
	}

	if (rad_check_lib_magic(RADIUSD_MAGIC_NUMBER) < 0) {
		exit(EXIT_FAILURE);
	}

	/*
	 *	Mismatch between build time OpenSSL and linked SSL,
	 *	better to die here than segfault later.
	 */
#ifdef HAVE_OPENSSL_CRYPTO_H
	if (ssl_check_version() < 0) {
		exit(EXIT_FAILURE);
	}

	/*
	 *	Initialising OpenSSL once, here, is safer than having individual
	 *	modules do it.
	 */
	tls_global_init();
#endif

	if (flag && (flag != 0x03)) {
		fprintf(stderr, "radiusd: The options -i and -p cannot be used individually.\n");
		exit(EXIT_FAILURE);
	}

	if (debug_flag) {
		version();
	}

	/*  Read the configuration files, BEFORE doing anything else.  */
	if (read_mainconfig(0) < 0) {
		exit(EXIT_FAILURE);
	}

	/* Set the panic action (if required) */
	if (mainconfig.panic_action &&
#ifndef NDEBUG
	    !getenv("PANIC_ACTION") &&
#endif
	    (fr_fault_setup(mainconfig.panic_action, argv[0]) < 0)) {
		exit(EXIT_FAILURE);
	}

#ifndef __MINGW32__

	devnull = open("/dev/null", O_RDWR);
	if (devnull < 0) {
		ERROR("Failed opening /dev/null: %s", fr_syserror(errno));
		exit(EXIT_FAILURE);
	}

	/*
	 *  Disconnect from session
	 */
	if (dont_fork == false) {
		pid_t pid;

		/*
		 *  Really weird things happen if we leave stdin open and call things like
		 *  system() later.
		 */
		if (dont_fork == false) {
			dup2(devnull, STDIN_FILENO);
		}

		if (pipe(from_child) != 0) {
			ERROR("Couldn't open pipe for child status: %s", fr_syserror(errno));
			exit(EXIT_FAILURE);
		}

		pid = fork();
		if (pid < 0) {
			ERROR("Couldn't fork: %s", fr_syserror(errno));
			exit(EXIT_FAILURE);
		}

		/*
		 *  The parent exits, so the child can run in the background.
		 *
		 *  As the child can still encounter an error during initialisation
		 *  we do a blocking read on a pipe between it and the parent.
		 *
		 *  Just before entering the event loop the child will send a success
		 *  or failure message to the parent, via the pipe.
		 */
		if (pid > 0) {
			uint8_t ret = 0;
			int stat_loc;

			/* So the pipe is correctly widowed if the child exits */
			close(from_child[1]);

			/*
			 *	The child writes a 0x01 byte on
			 *	success, and closes the pipe on error.
			 */
			if ((read(from_child[0], &ret, 1) < 0)) {
				ret = 0;
			}

			/* For cleanliness... */
			close(from_child[0]);

			/* Don't turn children into zombies */
			if (!ret) {
				waitpid(pid, &stat_loc, WNOHANG);
				exit(EXIT_FAILURE);
			}

			exit(EXIT_SUCCESS);
		}

		/* so the pipe is correctly widowed if the parent exits?! */
		close(from_child[0]);
#  ifdef HAVE_SETSID
		setsid();
#  endif
	}
#endif

	if (default_log.dest == L_DST_STDOUT) {
		setlinebuf(stdout);
		default_log.fd = STDOUT_FILENO;
	} else if (debug_flag) {
		dup2(devnull, STDOUT_FILENO);
	}

	if (default_log.dest == L_DST_STDERR) {
		setlinebuf(stdout);
		default_log.fd = STDERR_FILENO;
	} else {
		dup2(devnull, STDERR_FILENO);
	}

	/* Libraries may write messages to stderr or stdout */
	if (debug_flag) {
		dup2(default_log.fd, STDOUT_FILENO);
		dup2(default_log.fd, STDERR_FILENO);
	}

	close(devnull);

	/*
	 *  Ensure that we're using the CORRECT pid after forking,
	 *  NOT the one we started with.
	 */
	radius_pid = getpid();

	/*
	 *	Initialize the event pool, including threads.
	 */
	radius_event_init(mainconfig.config, spawn_flag);

	/*
	 *	Now that we've set everything up, we can install the signal
	 *	handlers.  Before this, if we get any signal, we don't know
	 *	what to do, so we might as well do the default, and die.
	 */
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	if ((fr_set_signal(SIGHUP, sig_hup) < 0) ||
	    (fr_set_signal(SIGTERM, sig_fatal) < 0)) {
	    	ERROR("%s", fr_strerror());
	 	exit(EXIT_FAILURE);
	}

	/*
	 *	If we're debugging, then a CTRL-C will cause the
	 *	server to die immediately.  Use SIGTERM to shut down
	 *	the server cleanly in that case.
	 */
	if (mainconfig.debug_memory || (debug_flag == 0)) {
		if ((fr_set_signal(SIGINT, sig_fatal) < 0)
#ifdef SIGQUIT
		|| (fr_set_signal(SIGQUIT, sig_fatal) < 0)
#endif
		) {
			ERROR("%s", fr_strerror());
			exit(EXIT_FAILURE);
		}
	}

	/*
	 *	Everything seems to have loaded OK, exit gracefully.
	 */
	if (check_config) {
		DEBUG("Configuration appears to be OK");

		/* for -C -m|-M */
		if (mainconfig.debug_memory) {
			goto cleanup;
		}

		exit(EXIT_SUCCESS);
	}

#ifdef WITH_STATS
	radius_stats_init(0);
#endif

	/*
	 *	Write out the PID anyway if were in foreground mode.
	 */
	if (!dont_fork) write_pid = true;

	/*
	 *  Only write the PID file if we're running as a daemon.
	 *
	 *  And write it AFTER we've forked, so that we write the
	 *  correct PID.
	 */
	if (write_pid) {
		FILE *fp;

		fp = fopen(mainconfig.pid_file, "w");
		if (fp != NULL) {
			/*
			 *	FIXME: What about following symlinks,
			 *	and having it over-write a normal file?
			 */
			fprintf(fp, "%d\n", (int) radius_pid);
			fclose(fp);
		} else {
			ERROR("Failed creating PID file %s: %s\n",
			       mainconfig.pid_file, fr_syserror(errno));
			exit(EXIT_FAILURE);
		}
	}

	exec_trigger(NULL, NULL, "server.start", false);

	/*
	 *	Inform the parent (who should still be waiting) that
	 *	the rest of initialisation went OK, and that it should
	 *	exit with a 0 status.  If we don't get this far, then
	 *	we just close the pipe on exit, and the parent gets a
	 *	read failure.
	 */
	if (!dont_fork) {
		if (write(from_child[1], "\001", 1) < 0) {
			WARN("Failed informing parent of successful start: %s",
			     fr_syserror(errno));
		}
		close(from_child[1]);
	}

	/*
	 *	Process requests until HUP or exit.
	 */
	while ((status = radius_event_process()) == 0x80) {
#ifdef WITH_STATS
		radius_stats_init(1);
#endif
		hup_mainconfig();
	}
	if (status < 0) {
		ERROR("Exiting due to internal error: %s", fr_strerror());
		rcode = EXIT_FAILURE;
	} else {
		INFO("Exiting normally");
	}

	exec_trigger(NULL, NULL, "server.stop", false);

	/*
	 *	Ignore the TERM signal: we're
	 *	about to die.
	 */
	signal(SIGTERM, SIG_IGN);

	/*
	 *	Send a TERM signal to all
	 *	associated processes
	 *	(including us, which gets
	 *	ignored.)
	 */
#ifndef __MINGW32__
	if (spawn_flag) kill(-radius_pid, SIGTERM);
#endif

	/*
	 *	We're exiting, so we can delete the PID
	 *	file.  (If it doesn't exist, we can ignore
	 *	the error returned by unlink)
	 */
	if (dont_fork == false) {
		unlink(mainconfig.pid_file);
	}

	radius_event_free();

cleanup:
	/*
	 *	Detach any modules.
	 */
	detach_modules();

	xlat_free();		/* modules may have xlat's */

	/*
	 *	Free the configuration items.
	 */
	free_mainconfig();

	rad_const_free(radius_dir);

#ifdef WIN32
	WSACleanup();
#endif

	if (mainconfig.memory_report) {
		INFO("Allocated memory at time of report:");
		log_talloc_report(NULL);
	}

	return rcode;
}


/*
 *  Display the syntax for starting this program.
 */
static void NEVER_RETURNS usage(int status)
{
	FILE *output = status?stderr:stdout;

	fprintf(output, "Usage: %s [options]\n", progname);
	fprintf(output, "Options:\n");
	fprintf(output, "  -C            Check configuration and exit.\n");
	fprintf(stderr, "  -d <raddb>    Set configuration directory (defaults to " RADDBDIR ").\n");
	fprintf(stderr, "  -D <dictdir>  Set main dictionary directory (defaults to " DICTDIR ").\n");
	fprintf(output, "  -f            Run as a foreground process, not a daemon.\n");
	fprintf(output, "  -h            Print this help message.\n");
	fprintf(output, "  -i <ipaddr>   Listen on ipaddr ONLY.\n");
	fprintf(output, "  -l <log_file> Logging output will be written to this file.\n");
	fprintf(output, "  -m            On SIGINT or SIGQUIT exit cleanly instead of immediately.\n");
	fprintf(output, "  -n <name>     Read raddb/name.conf instead of raddb/radiusd.conf.\n");
	fprintf(output, "  -p <port>     Listen on port ONLY.\n");
	fprintf(output, "  -P            Always write out PID, even with -f.\n");
	fprintf(output, "  -s            Do not spawn child processes to handle requests.\n");
	fprintf(output, "  -t            Disable threads.\n");
	fprintf(output, "  -v            Print server version information.\n");
	fprintf(output, "  -X            Turn on full debugging.\n");
	fprintf(output, "  -x            Turn on additional debugging. (-xx gives more debugging).\n");
	exit(status);
}


/*
 *	We got a fatal signal.
 */
static void sig_fatal(int sig)
{
	if (getpid() != radius_pid) _exit(sig);

	switch(sig) {
	case SIGTERM:
		radius_signal_self(RADIUS_SIGNAL_SELF_TERM);
		break;

	case SIGINT:
#ifdef SIGQUIT
	case SIGQUIT:
#endif
		if (mainconfig.debug_memory || mainconfig.memory_report) {
			radius_signal_self(RADIUS_SIGNAL_SELF_TERM);
			break;
		}
		/* FALL-THROUGH */

	default:
		_exit(sig);
	}
}

#ifdef SIGHUP
/*
 *  We got the hangup signal.
 *  Re-read the configuration files.
 */
static void sig_hup(UNUSED int sig)
{
	reset_signal(SIGHUP, sig_hup);

	radius_signal_self(RADIUS_SIGNAL_SELF_HUP);
}
#endif
