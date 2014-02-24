/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef HAVE_CAP_NG_H
#include <cap-ng.h>
#endif /* HAVE_CAP_NG_H */

#include "libknot/common.h"
#include "libknot/dnssec/crypto.h"
#include "knot/knot.h"
#include "knot/server/server.h"
#include "knot/ctl/process.h"
#include "knot/ctl/remote.h"
#include "knot/conf/conf.h"
#include "knot/conf/logconf.h"
#include "knot/server/zones.h"
#include "knot/server/tcp-handler.h"

/*----------------------------------------------------------------------------*/

/* Signal flags. */
static volatile short sig_req_stop = 0;
static volatile short sig_req_reload = 0;
static volatile short sig_stopping = 0;

// Cleanup handler
static int do_cleanup(server_t *server, char *configf, char *pidf);

// atexit() handler for server code
static void deinit(void)
{
	knot_crypto_cleanup();
	knot_crypto_cleanup_threads();
}

// SIGINT signal handler
void interrupt_handle(int s)
{
	// Reload configuration
	if (s == SIGHUP) {
		sig_req_reload = 1;
		return;
	}

	// Stop server
	if (s == SIGINT || s == SIGTERM) {
		if (sig_stopping == 0) {
			sig_req_stop = 1;
			sig_stopping = 1;
		} else {
			exit(1);
		}
	}
}

void help(void)
{
	printf("Usage: %sd [parameters]\n",
	       PACKAGE_NAME);
	printf("\nParameters:\n"
	       " -c, --config <file>     Select configuration file.\n"
	       " -d, --daemonize=[dir]   Run server as a daemon.\n"
	       " -v, --verbose           Verbose mode - additional runtime information.\n"
	       " -V, --version           Print version of the server.\n"
	       " -h, --help              Print help and usage.\n");
}

int main(int argc, char **argv)
{
	atexit(deinit);

	// Parse command line arguments
	int c = 0, li = 0;
	int verbose = 0;
	int daemonize = 0;
	char *config_fn = NULL;
	char *daemon_root = NULL;

	/* Long options. */
	struct option opts[] = {
		{"config",    required_argument, 0, 'c'},
		{"daemonize", optional_argument, 0, 'd'},
		{"verbose",   no_argument,       0, 'v'},
		{"version",   no_argument,       0, 'V'},
		{"help",      no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "c:dvVh", opts, &li)) != -1) {
		switch (c)
		{
		case 'c':
			free(config_fn);
			config_fn = strdup(optarg);
			break;
		case 'd':
			daemonize = 1;
			if (optarg) {
				daemon_root = strdup(optarg);
			}
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			free(config_fn);
			free(daemon_root);
			printf("%s, version %s\n", "Knot DNS", PACKAGE_VERSION);
			return 0;
		case 'h':
		case '?':
			free(config_fn);
			free(daemon_root);
			help();
			return 0;
		default:
			free(config_fn);
			free(daemon_root);
			help();
			return 1;
		}
	}

	// Check for non-option parameters.
	if (argc - optind > 0) {
		free(config_fn);
		free(daemon_root);
		help();
		return 1;
	}

	// Now check if we want to daemonize
	if (daemonize) {
		if (daemon(1, 0) != 0) {
			free(config_fn);
			free(daemon_root);
			fprintf(stderr, "Daemonization failed, shutting down...\n");
			return 1;
		}
	}

	// Initialize cryptographic backend
	knot_crypto_init();
	knot_crypto_init_threads();

	// Create server
	server_t server;
	int res = server_init(&server);
	if (res != KNOT_EOK) {
		fprintf(stderr, "Could not initialize server: %s\n", knot_strerror(res));
		free(config_fn);
		free(daemon_root);
		return 1;
	}

	// Register service and signal handler
	struct sigaction emptyset;
	memset(&emptyset, 0, sizeof(struct sigaction));
	emptyset.sa_handler = interrupt_handle;
	sigemptyset(&emptyset.sa_mask);
	emptyset.sa_flags = 0;
	sigaction(SIGALRM, &emptyset, NULL); // Interrupt
	sigaction(SIGPIPE, &emptyset, NULL); // Mask
	rcu_register_thread();

	// Initialize log
	log_init();

	// Verbose mode
	if (verbose) {
		int mask = LOG_MASK(LOG_INFO)|LOG_MASK(LOG_DEBUG);
		log_levels_add(LOGT_STDOUT, LOG_ANY, mask);
	}

	// Initialize pseudorandom number generator
	srand(time(0));

	// Find implicit configuration file
	if (!config_fn) {
		config_fn = conf_find_default();
	}

	// Find absolute path for config file
	if (config_fn[0] != '/')
	{
		// Get absolute path to cwd
		char *rpath = realpath(config_fn, NULL);
		if (rpath == NULL) {
			log_server_error("Couldn't get absolute path for configuration file '%s' - "
			                 "%s.\n", config_fn, strerror(errno));
			free(config_fn);
			free(daemon_root);
			return 1;
		} else {
			free(config_fn);
			config_fn = rpath;
		}
	}

	// Initialize configuration
	rcu_read_lock();
	conf_add_hook(conf(), CONF_LOG, log_reconfigure, 0);
	conf_add_hook(conf(), CONF_ALL, server_reconfigure, &server);
	rcu_read_unlock();

	/* POSIX 1003.1e capabilities. */
#ifdef HAVE_CAP_NG_H

	/* Drop all capabilities. */
	if (capng_have_capability(CAPNG_EFFECTIVE, CAP_SETPCAP)) {
		capng_clear(CAPNG_SELECT_BOTH);

		/* Retain ability to set capabilities and FS access. */
		capng_type_t tp = CAPNG_EFFECTIVE|CAPNG_PERMITTED;
		capng_update(CAPNG_ADD, tp, CAP_SETPCAP);
		capng_update(CAPNG_ADD, tp, CAP_DAC_OVERRIDE);
		capng_update(CAPNG_ADD, tp, CAP_CHOWN); /* Storage ownership. */

		/* Allow binding to privileged ports.
		 * (Not inheritable)
		 */
		capng_update(CAPNG_ADD, tp, CAP_NET_BIND_SERVICE);

		/* Allow setuid/setgid. */
		capng_update(CAPNG_ADD, tp, CAP_SETUID);
		capng_update(CAPNG_ADD, tp, CAP_SETGID);

		/* Allow priorities changing. */
		capng_update(CAPNG_ADD, tp, CAP_SYS_NICE);

		/* Apply */
		if (capng_apply(CAPNG_SELECT_BOTH) < 0) {
			log_server_error("Couldn't set process capabilities - "
			                 "%s.\n", strerror(errno));
		}
	} else {
		log_server_info("User uid=%d is not allowed to set "
		                "capabilities, skipping.\n", getuid());
	}
#endif /* HAVE_CAP_NG_H */

	// Open configuration
	log_server_info("Reading configuration '%s' ...\n", config_fn);
	int conf_ret = conf_open(config_fn);
	if (conf_ret != KNOT_EOK) {
		if (conf_ret == KNOT_ENOENT) {
			log_server_error("Couldn't open configuration file "
			                 "'%s'.\n", config_fn);
		} else {
			log_server_error("Failed to load configuration '%s'.\n",
			                 config_fn);
		}
		free(daemon_root);
		return do_cleanup(&server, config_fn, NULL);
	} else {
		log_server_info("Configured %d interfaces and %d zones.\n",
				conf()->ifaces_count, conf()->zones_count);
	}

	/* Alter privileges. */
	log_update_privileges(conf()->uid, conf()->gid);
	if (proc_update_privileges(conf()->uid, conf()->gid) != KNOT_EOK)
		return do_cleanup(&server, config_fn, NULL);

	/* Check and create PID file. */
	long pid = (long)getpid();
	char *pidf = NULL;
	char *cwd = NULL;
	if (daemonize) {
		if ((pidf = pid_check_and_create()) == NULL)
			return do_cleanup(&server, config_fn, pidf);
		log_server_info("Server started as a daemon, PID = %ld\n", pid);
		log_server_info("PID stored in '%s'\n", pidf);
		if ((cwd = malloc(PATH_MAX)) != NULL) {
			if (getcwd(cwd, PATH_MAX) == NULL) {
				log_server_info("Cannot get current working directory.\n");
				cwd[0] = '\0';
			}
		}
		if (daemon_root == NULL) {
			daemon_root = strdup("/");
		}
		if (chdir(daemon_root) != 0) {
			log_server_warning("Server can't change working "
			                   "directory to %s.\n", daemon_root);
		} else {
			log_server_info("Server changed directory to %s.\n",
			                daemon_root);
		}
		free(daemon_root);
	} else {
		log_server_info("Server started in foreground, PID = %ld\n", pid);
		log_server_info("Server running without PID file.\n");
	}

	/* Populate zone database and add reconfiguration hook. */
	server_update_zones(conf(), &server);
	conf_add_hook(conf(), CONF_ALL, server_update_zones, &server);

	// Run server
	log_server_info("Starting server...\n");
	if ((server_start(&server)) == KNOT_EOK) {
		if (knot_zonedb_size(server.zone_db) == 0) {
			log_server_warning("Server started, but no zones served.\n");
		}

		// Setup signal handler
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = interrupt_handle;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT,  &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP,  &sa, NULL);
		sigaction(SIGPIPE, &sa, NULL);
		sa.sa_flags = 0;
		pthread_sigmask(SIG_BLOCK, &sa.sa_mask, NULL);

		/* Bind to control interface. */
		uint8_t buf[KNOT_WIRE_MAX_PKTSIZE];
		size_t buflen = sizeof(buf);
		int remote = remote_bind(conf()->ctl.iface);

		/* Run event loop. */
		for(;;) {
			pthread_sigmask(SIG_UNBLOCK, &sa.sa_mask, NULL);
			int ret = remote_poll(remote);
			pthread_sigmask(SIG_BLOCK, &sa.sa_mask, NULL);

			/* Events. */
			if (ret > 0) {
				ret = remote_process(&server, conf()->ctl.iface,
				                     remote, buf, buflen);
				switch(ret) {
				case KNOT_CTL_STOP:
					sig_req_stop = 1;
					break;
				default:
					break;
				}
			}

			/* Interrupts. */
			if (sig_req_stop) {
				sig_req_stop = 0;
				server_stop(&server);
				break;
			}
			if (sig_req_reload) {
				sig_req_reload = 0;
				server_reload(&server, config_fn);
			}
		}
		pthread_sigmask(SIG_UNBLOCK, &sa.sa_mask, NULL);

		/* Close remote control interface */
		remote_unbind(conf()->ctl.iface, remote);

		if ((server_wait(&server)) != KNOT_EOK) {
			log_server_error("An error occured while "
					 "waiting for server to finish.\n");
			res = 1;
		} else {
			log_server_info("Server finished.\n");
		}

	} else {
		log_server_fatal("An error occured while "
				 "starting the server.\n");
		res = 1;
	}

	log_server_info("Shut down.\n");
	log_close();

	/* Cleanup. */
	if (pidf && pid_remove(pidf) < 0) {
		log_server_warning("Failed to remove PID file.\n");
	}
	do_cleanup(&server, config_fn, pidf);

	if (!daemonize) {
		fflush(stdout);
		fflush(stderr);
	}

	/* Return to original working directory. */
	if (cwd) {
		if (chdir(cwd) != 0) {
			log_server_warning("Server can't change working directory.\n");
		}
		free(cwd);
	}

	return res;
}

static int do_cleanup(server_t *server, char *configf, char *pidf)
{
	/* Free alloc'd variables. */
	server_wait(server);
	server_deinit(server);
	free(configf);
	free(pidf);

	/* Unhook from RCU */
	rcu_unregister_thread();

	return 1;
}
