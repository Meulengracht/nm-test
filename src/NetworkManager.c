/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2004 - 2008 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <getopt.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <glib/gi18n.h>
#include <string.h>

#include "NetworkManager.h"
#include "nm-utils.h"
#include "NetworkManagerUtils.h"
#include "nm-manager.h"
#include "NetworkManagerPolicy.h"
#include "NetworkManagerSystem.h"
#include "nm-named-manager.h"
#include "nm-dbus-manager.h"
#include "nm-supplicant-manager.h"
#include "nm-dhcp-manager.h"
#include "nm-netlink-monitor.h"
#include "nm-vpn-manager.h"
#include "nm-logging.h"

#define NM_DEFAULT_PID_FILE	LOCALSTATEDIR"/run/NetworkManager.pid"
#define NM_DEFAULT_SYSTEM_STATE_FILE LOCALSTATEDIR"/lib/NetworkManager/NetworkManager.state"

/*
 * Globals
 */
static NMManager *manager = NULL;
static GMainLoop *main_loop = NULL;

typedef struct {
	time_t time;
	GQuark domain;
	guint32 code;
	guint32 count;
} MonitorInfo;

static gboolean
detach_monitor (gpointer data)
{
	nm_info ("Detaching netlink event monitor");
	nm_netlink_monitor_detach (NM_NETLINK_MONITOR (data));
	return FALSE;
}

static void
nm_error_monitoring_device_link_state (NMNetlinkMonitor *monitor,
									   GError *error,
									   gpointer user_data)
{
	MonitorInfo *info = (MonitorInfo *) user_data;
	time_t now;

	now = time (NULL);

	if (info->domain != error->domain || info->code != error->code || (info->time && now > info->time + 10)) {
		/* FIXME: Try to handle the error instead of just printing it. */
		nm_warning ("error monitoring device for netlink events: %s\n",
					error->message);

		info->time = now;
		info->domain = error->domain;
		info->code = error->code;
		info->count = 0;
	}

	info->count++;
	if (info->count > 100) {
		/* Broken drivers will sometimes cause a flood of netlink errors.
		 * rh #459205, novell #443429, lp #284507
		 */
		nm_warning ("Excessive netlink errors ocurred, disabling netlink monitor.");
		nm_warning ("Link change events will not be processed.");
		g_idle_add_full (G_PRIORITY_HIGH, detach_monitor, monitor, NULL);
	}
}

static gboolean
nm_monitor_setup (void)
{
	GError *error = NULL;
	NMNetlinkMonitor *monitor;
	MonitorInfo *info;

	monitor = nm_netlink_monitor_get ();
	nm_netlink_monitor_open_connection (monitor, &error);
	if (error != NULL)
	{
		nm_warning ("could not monitor wired ethernet devices: %s",
					error->message);
		g_error_free (error);
		g_object_unref (monitor);
		return FALSE;
	}

	info = g_new0 (MonitorInfo, 1);
	g_signal_connect_data (G_OBJECT (monitor), "error",
						   G_CALLBACK (nm_error_monitoring_device_link_state),
						   info,
						   (GClosureNotify) g_free,
						   0);

	nm_netlink_monitor_attach (monitor);

	/* Request initial status of cards */
	nm_netlink_monitor_request_status (monitor, NULL);

	return TRUE;
}

static gboolean quit_early = FALSE;

static void
nm_signal_handler (int signo)
{
	static int in_fatal = 0;

	/* avoid loops */
	if (in_fatal > 0)
		return;
	++in_fatal;

	switch (signo)
	{
		case SIGSEGV:
		case SIGBUS:
		case SIGILL:
		case SIGABRT:
			nm_warning ("Caught signal %d.  Generating backtrace...", signo);
			nm_logging_backtrace ();
			exit (1);
			break;

		case SIGFPE:
		case SIGPIPE:
			/* let the fatal signals interrupt us */
			--in_fatal;

			nm_warning ("Caught signal %d, shutting down abnormally.  Generating backtrace...", signo);
			nm_logging_backtrace ();
			g_main_loop_quit (main_loop);
			break;

		case SIGINT:
		case SIGTERM:
			/* let the fatal signals interrupt us */
			--in_fatal;

			nm_warning ("Caught signal %d, shutting down normally.", signo);
			quit_early = TRUE;
			g_main_loop_quit (main_loop);
			break;

		case SIGHUP:
			--in_fatal;
			/* FIXME:
			 * Reread config stuff like system config files, VPN service files, etc
			 */
			break;

		case SIGUSR1:
			--in_fatal;
			/* FIXME:
			 * Play with log levels or something
			 */
			break;

		default:
			signal (signo, nm_signal_handler);
			break;
	}
}

static void
setup_signals (void)
{
	struct sigaction action;
	sigset_t mask;

	sigemptyset (&mask);
	action.sa_handler = nm_signal_handler;
	action.sa_mask = mask;
	action.sa_flags = 0;
	sigaction (SIGTERM,  &action, NULL);
	sigaction (SIGINT,  &action, NULL);
	sigaction (SIGILL,  &action, NULL);
	sigaction (SIGBUS,  &action, NULL);
	sigaction (SIGFPE,  &action, NULL);
	sigaction (SIGHUP,  &action, NULL);
	sigaction (SIGSEGV, &action, NULL);
	sigaction (SIGABRT, &action, NULL);
	sigaction (SIGUSR1,  &action, NULL);
}

static gboolean
write_pidfile (const char *pidfile)
{
 	char pid[16];
	int fd;
	gboolean success = FALSE;
 
	if ((fd = open (pidfile, O_CREAT|O_WRONLY|O_TRUNC, 00644)) < 0) {
		nm_warning ("Opening %s failed: %s", pidfile, strerror (errno));
		return FALSE;
	}

 	snprintf (pid, sizeof (pid), "%d", getpid ());
	if (write (fd, pid, strlen (pid)) < 0)
		nm_warning ("Writing to %s failed: %s", pidfile, strerror (errno));
	else
		success = TRUE;

	if (close (fd))
		nm_warning ("Closing %s failed: %s", pidfile, strerror (errno));

	return success;
}

/* Check whether the pidfile already exists and contains PID of a running NetworkManager
 *  Returns:  FALSE - specified pidfile doesn't exist or doesn't contain PID of a running NM process
 *            TRUE  - specified pidfile already exists and contains PID of a running NM process
 */
static gboolean
check_pidfile (const char *pidfile)
{
	char *contents = NULL;
	gsize len = 0;
	glong pid;
	char *proc_cmdline = NULL;
	gboolean nm_running = FALSE;
	const char *process_name;

	if (!g_file_get_contents (pidfile, &contents, &len, NULL))
		return FALSE;

	if (len <= 0)
		goto done;

	errno = 0;
	pid = strtol (contents, NULL, 10);
	if (pid <= 0 || pid > 65536 || errno)
		goto done;

	g_free (contents);
	proc_cmdline = g_strdup_printf ("/proc/%ld/cmdline", pid);
	if (!g_file_get_contents (proc_cmdline, &contents, &len, NULL))
		goto done;

	process_name = strrchr (contents, '/');
	if (process_name)
		process_name++;
	else
		process_name = contents;
	if (strcmp (process_name, "NetworkManager") == 0) {
		/* Check that the process exists */
		if (kill (pid, 0) == 0) {
			g_warning ("NetworkManager is already running (pid %ld)", pid);
			nm_running = TRUE;
		}
	}

done:
	g_free (proc_cmdline);
	g_free (contents);
	return nm_running;
}

static gboolean
parse_state_file (const char *filename,
                  gboolean *net_enabled,
                  gboolean *wifi_enabled,
                  GError **error)
{
	GKeyFile *state_file;
	GError *tmp_error = NULL;
	gboolean wifi, net;

	g_return_val_if_fail (net_enabled != NULL, FALSE);
	g_return_val_if_fail (wifi_enabled != NULL, FALSE);

	state_file = g_key_file_new ();
	if (!state_file) {
		g_set_error (error, 0, 0,
		             "Not enough memory to load state file.");
		return FALSE;
	}

	g_key_file_set_list_separator (state_file, ',');
	if (!g_key_file_load_from_file (state_file, filename, G_KEY_FILE_KEEP_COMMENTS, &tmp_error)) {
		/* This is kinda ugly; create the file and directory if it doesn't
		 * exist yet.  We can't rely on distros necessarily creating the
		 * /var/lib/NetworkManager for us since we have to ensure that
		 * users upgrading NM get this working too.
		 */
		if (   tmp_error->domain == G_FILE_ERROR
		    && tmp_error->code == G_FILE_ERROR_NOENT) {
			char *data, *dirname;
			gsize len = 0;
			gboolean ret = FALSE;

			/* try to create the directory if it doesn't exist */
			dirname = g_path_get_dirname (filename);
			errno = 0;
			if (mkdir (dirname, 0755) != 0) {
				if (errno != EEXIST) {
					g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES,
					             "Error creating state directory %s: %d", dirname, errno);
					g_free (dirname);
					return FALSE;
				}
			}
			g_free (dirname);

			/* Write out the initial state to the state file */
			g_key_file_set_boolean (state_file, "main", "NetworkingEnabled", *net_enabled);
			g_key_file_set_boolean (state_file, "main", "WirelessEnabled", *wifi_enabled);

			data = g_key_file_to_data (state_file, &len, NULL);
			if (data)
				ret = g_file_set_contents (filename, data, len, error);
			g_free (data);

			return ret;
		} else {
			g_set_error_literal (error, tmp_error->domain, tmp_error->code, tmp_error->message);
			g_clear_error (&tmp_error);
		}

		/* Otherwise, file probably corrupt or inaccessible */
		return FALSE;
	}

	/* Reading state bits of NetworkManager; an error leaves the passed-in state
	 * value unchanged.
	 */
	net = g_key_file_get_boolean (state_file, "main", "NetworkingEnabled", &tmp_error);
	if (tmp_error)
		g_set_error_literal (error, tmp_error->domain, tmp_error->code, tmp_error->message);
	else
		*net_enabled = net;
	g_clear_error (&tmp_error);

	wifi = g_key_file_get_boolean (state_file, "main", "WirelessEnabled", error);
	if (tmp_error) {
		g_clear_error (error);
		g_set_error_literal (error, tmp_error->domain, tmp_error->code, tmp_error->message);
	} else
		*wifi_enabled = wifi;
	g_clear_error (&tmp_error);

	g_key_file_free (state_file);

	return TRUE;
}

/*
 * main
 *
 */
int
main (int argc, char *argv[])
{
	GOptionContext *opt_ctx = NULL;
	gboolean become_daemon = FALSE;
	char *pidfile = NULL;
	char *user_pidfile = NULL;
	char *state_file = NM_DEFAULT_SYSTEM_STATE_FILE;
	gboolean wifi_enabled = TRUE, net_enabled = TRUE;
	gboolean success;
	NMPolicy *policy = NULL;
	NMVPNManager *vpn_manager = NULL;
	NMNamedManager *named_mgr = NULL;
	NMDBusManager *dbus_mgr = NULL;
	NMSupplicantManager *sup_mgr = NULL;
	NMDHCPManager *dhcp_mgr = NULL;
	GError *error = NULL;
	gboolean wrote_pidfile = FALSE;

	GOptionEntry options[] = {
		{"no-daemon", 0, 0, G_OPTION_ARG_NONE, &become_daemon, "Don't become a daemon", NULL},
		{"pid-file", 0, 0, G_OPTION_ARG_FILENAME, &user_pidfile, "Specify the location of a PID file", "filename"},
		{"state-file", 0, 0, G_OPTION_ARG_FILENAME, &state_file, "State file location", "/path/to/state.file"},
		{NULL}
	};

	if (getuid () != 0) {
		g_printerr ("You must be root to run NetworkManager!\n");
		exit (1);
	}

	bindtextdomain (GETTEXT_PACKAGE, NMLOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Parse options */
	opt_ctx = g_option_context_new ("");
	g_option_context_set_translation_domain (opt_ctx, "UTF-8");
	g_option_context_set_ignore_unknown_options (opt_ctx, FALSE);
	g_option_context_set_help_enabled (opt_ctx, TRUE);
	g_option_context_add_main_entries (opt_ctx, options, NULL);

	g_option_context_set_summary (opt_ctx,
		"NetworkManager monitors all network connections and automatically\nchooses the best connection to use.  It also allows the user to\nspecify wireless access points which wireless cards in the computer\nshould associate with.");

	success = g_option_context_parse (opt_ctx, &argc, &argv, NULL);
	g_option_context_free (opt_ctx);

	if (!success) {
		fprintf (stderr, _("Invalid option.  Please use --help to see a list of valid options.\n"));
		exit (1);
	}

	pidfile = g_strdup (user_pidfile ? user_pidfile : NM_DEFAULT_PID_FILE);

	/* check pid file */
	if (check_pidfile (pidfile))
		exit (1);

	/* Parse the state file */
	if (!parse_state_file (state_file, &net_enabled, &wifi_enabled, &error)) {
		g_warning ("State file %s parsing failed: (%d) %s.",
		           state_file,
		           error ? error->code : -1,
		           (error && error->message) ? error->message : "unknown");
		/* Not a hard failure */
	}

	/* Tricky: become_daemon is FALSE by default, so unless it's TRUE because
	 * of a CLI option, it'll become TRUE after this
	 */
	become_daemon = !become_daemon;
	if (become_daemon) {
		if (daemon (0, 0) < 0) {
			int saved_errno;

			saved_errno = errno;
			nm_error ("Could not daemonize: %s [error %u]",
			          g_strerror (saved_errno),
			          saved_errno);
			exit (1);
		}
		if (write_pidfile (pidfile))
			wrote_pidfile = TRUE;
	}

	/*
	 * Set the umask to 0022, which results in 0666 & ~0022 = 0644.
	 * Otherwise, if root (or an su'ing user) has a wacky umask, we could
	 * write out an unreadable resolv.conf.
	 */
	umask (022);

	g_type_init ();
	if (!g_thread_supported ())
		g_thread_init (NULL);
	dbus_g_thread_init ();

	setup_signals ();

	nm_logging_setup (become_daemon);

	nm_info ("starting...");
	success = FALSE;

	main_loop = g_main_loop_new (NULL, FALSE);

	/* Create watch functions that monitor cards for link status. */
	if (!nm_monitor_setup ())
		goto done;

	/* Initialize our DBus service & connection */
	dbus_mgr = nm_dbus_manager_get ();

	vpn_manager = nm_vpn_manager_get ();
	if (!vpn_manager) {
		nm_warning ("Failed to start the VPN manager.");
		goto done;
	}

	manager = nm_manager_get (state_file, net_enabled, wifi_enabled);
	if (manager == NULL) {
		nm_error ("Failed to initialize the network manager.");
		goto done;
	}

	policy = nm_policy_new (manager, vpn_manager);
	if (policy == NULL) {
		nm_error ("Failed to initialize the policy.");
		goto done;
	}

	/* Initialize the supplicant manager */
	sup_mgr = nm_supplicant_manager_get ();
	if (!sup_mgr) {
		nm_error ("Failed to initialize the supplicant manager.");
		goto done;
	}

	named_mgr = nm_named_manager_get ();
	if (!named_mgr) {
		nm_warning ("Failed to start the named manager.");
		goto done;
	}

	dhcp_mgr = nm_dhcp_manager_get ();
	if (!dhcp_mgr) {
		nm_warning ("Failed to start the DHCP manager.");
		goto done;
	}

	/* Start our DBus service */
	if (!nm_dbus_manager_start_service (dbus_mgr)) {
		nm_warning ("Failed to start the dbus service.");
		goto done;
	}

	/* Bring up the loopback interface. */
	nm_system_enable_loopback ();

	success = TRUE;

	/* Told to quit before getting to the mainloop by the signal handler */
	if (quit_early == TRUE)
		goto done;

	g_main_loop_run (main_loop);

done:
	if (policy)
		nm_policy_destroy (policy);

	if (manager)
		g_object_unref (manager);

	if (vpn_manager)
		g_object_unref (vpn_manager);

	if (named_mgr)
		g_object_unref (named_mgr);

	if (dhcp_mgr)
		g_object_unref (dhcp_mgr);

	if (sup_mgr)
		g_object_unref (sup_mgr);

	if (dbus_mgr)
		g_object_unref (dbus_mgr);

	nm_logging_shutdown ();

	if (pidfile && wrote_pidfile)
		unlink (pidfile);
	g_free (pidfile);

	nm_info ("exiting (%s)", success ? "success" : "error");
	exit (success ? 0 : 1);
}
