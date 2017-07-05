
/*
 * ODISSEY.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "sources/macro.h"
#include "sources/version.h"
#include "sources/list.h"
#include "sources/pid.h"
#include "sources/id.h"
#include "sources/syslog.h"
#include "sources/log.h"
#include "sources/daemon.h"
#include "sources/scheme.h"
#include "sources/lex.h"
#include "sources/config.h"
#include "sources/msg.h"
#include "sources/system.h"
#include "sources/instance.h"
#include "sources/server.h"
#include "sources/server_pool.h"
#include "sources/client.h"
#include "sources/client_pool.h"
#include "sources/route_id.h"
#include "sources/route.h"
#include "sources/route_pool.h"
#include "sources/io.h"
#include "sources/router.h"
#include "sources/console.h"
#include "sources/pooler.h"
#include "sources/periodic.h"
#include "sources/relay.h"
#include "sources/relay_pool.h"
#include "sources/frontend.h"

void od_instance_init(od_instance_t *instance)
{
	od_pid_init(&instance->pid);
	od_syslog_init(&instance->syslog);
	od_log_init(&instance->log, &instance->pid, &instance->syslog);
	od_scheme_init(&instance->scheme);
	od_config_init(&instance->config, &instance->log, &instance->scheme);
	od_idmgr_init(&instance->id_mgr);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &mask, NULL);
}

void od_instance_free(od_instance_t *instance)
{
	if (instance->scheme.pid_file)
		od_pid_unlink(&instance->pid, instance->scheme.pid_file);
	od_scheme_free(&instance->scheme);
	od_config_close(&instance->config);
	od_log_close(&instance->log);
	od_syslog_close(&instance->syslog);
}

static inline void
od_usage(od_instance_t *instance, char *path)
{
	od_log(&instance->log, "odissey (git: %s %s)",
	       OD_VERSION_GIT,
	       OD_VERSION_BUILD);
	od_log(&instance->log, "usage: %s <config_file>", path);
}

int od_instance_main(od_instance_t *instance, int argc, char **argv)
{
	/* validate command line options */
	if (argc != 2) {
		od_usage(instance, argv[0]);
		return 1;
	}
	char *config_file;
	if (argc == 2) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			od_usage(instance, argv[0]);
			return 0;
		}
		config_file = argv[1];
	}
	/* read config file */
	int rc;
	rc = od_config_open(&instance->config, config_file);
	if (rc == -1)
		return 1;
	rc = od_config_parse(&instance->config);
	if (rc == -1)
		return 1;
	/* set log debug on/off */
	od_logset_debug(&instance->log, instance->scheme.log_debug);
	/* run as daemon */
	if (instance->scheme.daemonize) {
		rc = od_daemonize();
		if (rc == -1)
			return 1;
		/* update pid */
		od_pid_init(&instance->pid);
	}
	/* reopen log file after config parsing */
	if (instance->scheme.log_file) {
		rc = od_log_open(&instance->log, instance->scheme.log_file);
		if (rc == -1) {
			od_error(&instance->log, NULL, "failed to open log file '%s'",
			         instance->scheme.log_file);
			return 1;
		}
	}
	/* syslog */
	if (instance->scheme.syslog) {
		od_syslog_open(&instance->syslog,
		               instance->scheme.syslog_ident,
		               instance->scheme.syslog_facility);
	}
	od_log(&instance->log, "odissey (git: %s %s)",
	       OD_VERSION_GIT,
	       OD_VERSION_BUILD);
	od_log(&instance->log, "");
	/* validate configuration scheme */
	rc = od_scheme_validate(&instance->scheme, &instance->log);
	if (rc == -1)
		return 1;
	/* print configuration */
	od_log(&instance->log, "using configuration file '%s'",
	       instance->scheme.config_file);
	od_log(&instance->log, "");
	if (instance->scheme.log_config)
		od_scheme_print(&instance->scheme, &instance->log);
	/* create pid file */
	if (instance->scheme.pid_file)
		od_pid_create(&instance->pid, instance->scheme.pid_file);
	/* seed id manager */
	rc = od_idmgr_seed(&instance->id_mgr);
	if (rc == -1)
		od_error(&instance->log, NULL, "failed to open random source device");

	/* run system services */
	od_router_t router;
	od_console_t console;
	od_periodic_t periodic;
	od_pooler_t pooler;
	od_relaypool_t relay_pool;
	od_system_t system = {
		.pooler     = &pooler,
		.router     = &router,
		.console    = &console,
		.periodic   = &periodic,
		.relay_pool = &relay_pool,
		.instance   = instance
	};
	od_router_init(&router, &system);
	od_console_init(&console, &system);
	od_periodic_init(&periodic, &system);
	od_pooler_init(&pooler, &system);
	rc = od_relaypool_init(&relay_pool, &system, instance->scheme.workers);
	if (rc == -1)
		return 1;
	/* start pooler machine thread */
	rc = od_pooler_start(&pooler);
	if (rc == -1)
		return 1;
	/* start worker threads */
	rc = od_relaypool_start(&relay_pool);
	if (rc == -1)
		return 1;

	machine_wait(pooler.machine);
	return 0;
}