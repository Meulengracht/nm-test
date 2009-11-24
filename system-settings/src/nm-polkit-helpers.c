/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
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
 * (C) Copyright 2008 Novell, Inc.
 * (C) Copyright 2008 Red Hat, Inc.
 */

#include <nm-dbus-settings.h>
#include "nm-polkit-helpers.h"
#include "nm-system-config-error.h"

static gboolean
pk_io_watch_have_data (GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
	int fd;
	PolKitContext *pk_context = (PolKitContext *) user_data;

	fd = g_io_channel_unix_get_fd (channel);
	polkit_context_io_func (pk_context, fd);
	
	return TRUE;
}

static int
pk_io_add_watch (PolKitContext *pk_context, int fd)
{
	guint id = 0;
	GIOChannel *channel;
	
	channel = g_io_channel_unix_new (fd);
	if (channel == NULL)
		goto out;
	id = g_io_add_watch (channel, G_IO_IN, pk_io_watch_have_data, pk_context);
	if (id == 0) {
		g_io_channel_unref (channel);
		goto out;
	}
	g_io_channel_unref (channel);

 out:
	return id;
}

static void
pk_io_remove_watch (PolKitContext *pk_context, int watch_id)
{
	g_source_remove (watch_id);
}

PolKitContext *
create_polkit_context (GError **error)
{
	static PolKitContext *global_context = NULL;
	PolKitError *pk_err = NULL;

	if (G_LIKELY (global_context))
		return polkit_context_ref (global_context);

	global_context = polkit_context_new ();
	polkit_context_set_io_watch_functions (global_context, pk_io_add_watch, pk_io_remove_watch);
	if (!polkit_context_init (global_context, &pk_err)) {
		g_set_error (error, NM_SYSCONFIG_SETTINGS_ERROR,
		             NM_SYSCONFIG_SETTINGS_ERROR_GENERAL,
		             "%s (%d): %s",
		             pk_err ? polkit_error_get_error_name (pk_err) : "(unknown)",
		             pk_err ? polkit_error_get_error_code (pk_err) : -1,
		             pk_err ? polkit_error_get_error_message (pk_err) : "(unknown)");
		if (pk_err)
			polkit_error_free (pk_err);

		/* PK 0.6's polkit_context_init() unrefs the global_context on failure */
#if (POLKIT_VERSION_MAJOR == 0) && (POLKIT_VERSION_MINOR >= 7)
		polkit_context_unref (global_context);
#endif
		global_context = NULL;
	}

	return global_context;
}

gboolean
check_polkit_privileges (DBusGConnection *dbus_connection,
					PolKitContext *pol_ctx,
					DBusGMethodInvocation *context,
					GError **err)
{
	DBusConnection *tmp;
	DBusError dbus_error;
	char *sender;
	gulong sender_uid = G_MAXULONG;
	PolKitCaller *pk_caller;
	PolKitAction *pk_action;
	PolKitResult pk_result;

	/* Always allow uid 0 */
	tmp = dbus_g_connection_get_connection (dbus_connection);
	if (!tmp) {
		g_set_error (err,
		             NM_SYSCONFIG_SETTINGS_ERROR,
		             NM_SYSCONFIG_SETTINGS_ERROR_GENERAL,
		             "Could not get D-Bus connection.");
		return FALSE;
	}

	sender = dbus_g_method_get_sender (context);

	dbus_error_init (&dbus_error);
	/* FIXME: do this async */
	sender_uid = dbus_bus_get_unix_user (tmp, sender, &dbus_error);
	if (dbus_error_is_set (&dbus_error)) {
		g_set_error (err,
		             NM_SYSCONFIG_SETTINGS_ERROR,
		             NM_SYSCONFIG_SETTINGS_ERROR_GENERAL,
		             "Could not determine the Unix user ID of the requestor: %s: %s",
		             dbus_error.name, dbus_error.message);
		dbus_error_free (&dbus_error);
		return FALSE;
	}

	/* PolicyKit < 1.0 is not compatible with root processes spawned outside
	 * the session manager, and when asking ConsoleKit for the session of the
	 * process, ConsoleKit won't be able to get XDG_SESSION_COOKIE because it
	 * doesn't exist in the caller's environment for non-session-managed
	 * processes.  So, for PK < 1.0, ignore PolicyKit for uid 0.
	 */
	if (0 == sender_uid)
		return TRUE;

	/* Non-root users need to auth via PolicyKit */
	dbus_error_init (&dbus_error);
	pk_caller = polkit_caller_new_from_dbus_name (dbus_g_connection_get_connection (dbus_connection),
										 sender,
										 &dbus_error);
	g_free (sender);

	if (dbus_error_is_set (&dbus_error)) {
		*err = g_error_new (NM_SYSCONFIG_SETTINGS_ERROR,
						NM_SYSCONFIG_SETTINGS_ERROR_NOT_PRIVILEGED,
						"Error getting information about caller: %s: %s",
						dbus_error.name, dbus_error.message);
		dbus_error_free (&dbus_error);

		if (pk_caller)
			polkit_caller_unref (pk_caller);

		return FALSE;
	}

	pk_action = polkit_action_new ();
	polkit_action_set_action_id (pk_action, NM_SYSCONFIG_POLICY_ACTION);

#if (POLKIT_VERSION_MAJOR == 0) && (POLKIT_VERSION_MINOR < 7)
	pk_result = polkit_context_can_caller_do_action (pol_ctx, pk_action, pk_caller);
#else
	pk_result = polkit_context_is_caller_authorized (pol_ctx, pk_action, pk_caller, TRUE, NULL);
#endif
	polkit_caller_unref (pk_caller);
	polkit_action_unref (pk_action);

	if (pk_result != POLKIT_RESULT_YES) {
		*err = g_error_new (NM_SYSCONFIG_SETTINGS_ERROR,
						NM_SYSCONFIG_SETTINGS_ERROR_NOT_PRIVILEGED,
						"%s %s",
						NM_SYSCONFIG_POLICY_ACTION,
						polkit_result_to_string_representation (pk_result));
		return FALSE;
	}

	return TRUE;
}
