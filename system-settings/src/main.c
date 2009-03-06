/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * Søren Sandmann <sandmann@daimi.au.dk>
 * Dan Williams <dcbw@redhat.com>
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
 * (C) Copyright 2007 - 2008 Red Hat, Inc.
 */

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <signal.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gmodule.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <nm-connection.h>
#include <nm-setting-connection.h>
#include <nm-setting-wired.h>
#include <nm-setting-pppoe.h>
#include <nm-settings.h>
#include <nm-utils.h>
#include <NetworkManager.h>
#include "nm-glib-compat.h"

#include "dbus-settings.h"
#include "nm-system-config-hal-manager.h"
#include "nm-system-config-interface.h"
#include "nm-default-wired-connection.h"

#define CONFIG_KEY_NO_AUTO_DEFAULT "no-auto-default"

static GMainLoop *loop = NULL;
static gboolean debug = FALSE;

typedef struct {
	DBusConnection *connection;
	DBusGConnection *g_connection;

	DBusGProxy *bus_proxy;
	NMSystemConfigHalManager *hal_mgr;

	NMSysconfigSettings *settings;

	GHashTable *wired_devices;

	const char *config;
} Application;


NMSystemConfigHalManager *nm_system_config_hal_manager_get (DBusGConnection *g_connection);
void nm_system_config_hal_manager_shutdown (NMSystemConfigHalManager *self);

static gboolean dbus_init (Application *app);
static gboolean start_dbus_service (Application *app);
static void destroy_cb (DBusGProxy *proxy, gpointer user_data);
static void device_added_cb (DBusGProxy *proxy, const char *udi, NMDeviceType devtype, gpointer user_data);


static GQuark
plugins_error_quark (void)
{
	static GQuark error_quark = 0;

	if (G_UNLIKELY (error_quark == 0))
		error_quark = g_quark_from_static_string ("plugins-error-quark");

	return error_quark;
}

static GObject *
find_plugin (GSList *list, const char *pname)
{
	GSList *iter;
	GObject *obj = NULL;

	g_return_val_if_fail (pname != NULL, FALSE);

	for (iter = list; iter && !obj; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		char *list_pname = NULL;

		g_object_get (G_OBJECT (plugin),
		              NM_SYSTEM_CONFIG_INTERFACE_NAME,
		              &list_pname,
		              NULL);
		if (list_pname && !strcmp (pname, list_pname))
			obj = G_OBJECT (plugin);

		g_free (list_pname);
	}

	return obj;
}

static gboolean
load_plugins (Application *app, const char *plugins, GError **error)
{
	GSList *list = NULL;
	char **plist;
	char **iter;

	plist = g_strsplit (plugins, ",", 0);
	if (!plist)
		return FALSE;

	for (iter = plist; *iter; iter++) {
		GModule *plugin;
		char *full_name, *path;
		const char *pname = *iter;
		GObject *obj;
		GObject * (*factory_func) (void);

		/* ifcfg-fedora was renamed ifcfg-rh; handle old configs here */
		if (!strcmp (pname, "ifcfg-fedora"))
			pname = "ifcfg-rh";

		obj = find_plugin (list, pname);
		if (obj)
			continue;

		full_name = g_strdup_printf ("nm-settings-plugin-%s", pname);
		path = g_module_build_path (PLUGINDIR, full_name);

		plugin = g_module_open (path, G_MODULE_BIND_LOCAL);
		if (!plugin) {
			g_set_error (error, plugins_error_quark (), 0,
			             "Could not load plugin '%s': %s",
			             pname, g_module_error ());
			g_free (full_name);
			g_free (path);
			break;
		}

		g_free (full_name);
		g_free (path);

		if (!g_module_symbol (plugin, "nm_system_config_factory", (gpointer) (&factory_func))) {
			g_set_error (error, plugins_error_quark (), 0,
			             "Could not find plugin '%s' factory function.",
			             pname);
			break;
		}

		obj = (*factory_func) ();
		if (!obj || !NM_IS_SYSTEM_CONFIG_INTERFACE (obj)) {
			g_set_error (error, plugins_error_quark (), 0,
			             "Plugin '%s' returned invalid system config object.",
			             pname);
			break;
		}

		g_module_make_resident (plugin);
		g_object_weak_ref (obj, (GWeakNotify) g_module_close, plugin);
		nm_sysconfig_settings_add_plugin (app->settings, NM_SYSTEM_CONFIG_INTERFACE (obj));
		list = g_slist_append (list, obj);
	}

	g_strfreev (plist);

	g_slist_foreach (list, (GFunc) g_object_unref, NULL);
	g_slist_free (list);

	return TRUE;
}

static gboolean
load_stuff (gpointer user_data)
{
	Application *app = (Application *) user_data;
	GSList *devs, *iter;

	/* Grab wired devices to make default DHCP connections for them if needed */
	devs = nm_system_config_hal_manager_get_devices_of_type (app->hal_mgr, NM_DEVICE_TYPE_ETHERNET);
	for (iter = devs; iter; iter = g_slist_next (iter)) {
		device_added_cb (NULL, (const char *) iter->data, NM_DEVICE_TYPE_ETHERNET, app);
		g_free (iter->data);
	}

	g_slist_free (devs);

	if (!start_dbus_service (app)) {
		g_main_loop_quit (loop);
		return FALSE;
	}

	return FALSE;
}

typedef struct {
	Application *app;
	NMDefaultWiredConnection *connection;
	guint add_id;
	guint updated_id;
	guint deleted_id;
	char *udi;
} WiredDeviceInfo;

static void
wired_device_info_destroy (gpointer user_data)
{
	WiredDeviceInfo *info = (WiredDeviceInfo *) user_data;

	if (info->add_id)
		g_source_remove (info->add_id);
	if (info->updated_id)
		g_source_remove (info->updated_id);
	if (info->deleted_id)
		g_source_remove (info->deleted_id);
	if (info->connection) {
		nm_sysconfig_settings_remove_connection (info->app->settings,
		                                         NM_EXPORTED_CONNECTION (info->connection),
		                                         TRUE);
		g_object_unref (info->connection);
	}
	g_free (info);
}

static char *
get_details_for_udi (Application *app, const char *udi, struct ether_addr *mac)
{
	DBusGProxy *dev_proxy = NULL;
	char *address = NULL;
	char *iface = NULL;
	struct ether_addr *temp;
	GError *error = NULL;

	g_return_val_if_fail (app != NULL, FALSE);
	g_return_val_if_fail (udi != NULL, FALSE);
	g_return_val_if_fail (mac != NULL, FALSE);

	dev_proxy = dbus_g_proxy_new_for_name (app->g_connection,
	                                       "org.freedesktop.Hal",
	                                       udi,
	                                       "org.freedesktop.Hal.Device");
	if (!dev_proxy)
		goto out;

	if (!dbus_g_proxy_call_with_timeout (dev_proxy,
	                                     "GetPropertyString", 5000, &error,
	                                     G_TYPE_STRING, "net.address", G_TYPE_INVALID,
	                                     G_TYPE_STRING, &address, G_TYPE_INVALID)) {
		g_message ("Error getting hardware address for %s: (%d) %s",
		           udi, error->code, error->message);
		g_error_free (error);
		goto out;
	}

	if (!address && !strlen (address))
		goto out;

	temp = ether_aton (address);
	if (!temp)
		goto out;
	memcpy (mac, temp, sizeof (struct ether_addr));

	if (!dbus_g_proxy_call_with_timeout (dev_proxy,
	                                     "GetPropertyString", 5000, &error,
	                                     G_TYPE_STRING, "net.interface", G_TYPE_INVALID,
	                                     G_TYPE_STRING, &iface, G_TYPE_INVALID)) {
		g_message ("Error getting interface name for %s: (%d) %s",
		           udi, error->code, error->message);
		g_error_free (error);
	}

out:
	g_free (address);
	if (dev_proxy)
		g_object_unref (dev_proxy);
	return iface;
}

static gboolean
have_connection_for_device (Application *app, GByteArray *mac)
{
	GSList *list, *iter;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	const GByteArray *setting_mac;
	gboolean ret = FALSE;

	g_return_val_if_fail (app != NULL, FALSE);
	g_return_val_if_fail (mac != NULL, FALSE);

	/* Find a wired connection locked to the given MAC address, if any */
	list = nm_settings_list_connections (NM_SETTINGS (app->settings));
	for (iter = list; iter; iter = g_slist_next (iter)) {
		NMExportedConnection *exported = NM_EXPORTED_CONNECTION (iter->data);
		NMConnection *connection;
		const char *connection_type;

		connection = nm_exported_connection_get_connection (exported);
		if (!connection)
			continue;

		s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
		connection_type = nm_setting_connection_get_connection_type (s_con);

		if (   strcmp (connection_type, NM_SETTING_WIRED_SETTING_NAME)
		    && strcmp (connection_type, NM_SETTING_PPPOE_SETTING_NAME))
			continue;

		s_wired = (NMSettingWired *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRED);

		/* No wired setting; therefore the PPPoE connection applies to any device */
		if (!s_wired && !strcmp (connection_type, NM_SETTING_PPPOE_SETTING_NAME)) {
			ret = TRUE;
			break;
		}

		setting_mac = nm_setting_wired_get_mac_address (s_wired);
		if (setting_mac) {
			/* A connection mac-locked to this device */
			if (!memcmp (setting_mac->data, mac->data, ETH_ALEN)) {
				ret = TRUE;
				break;
			}

		} else {
			/* A connection that applies to any wired device */
			ret = TRUE;
			break;
		}
	}

	g_slist_free (list);

	return ret;
}

/* Search through the list of blacklisted MAC addresses in the config file. */
static gboolean
is_mac_auto_wired_blacklisted (const GByteArray *mac, const char *filename)
{
	GKeyFile *config;
	char **list, **iter;
	gboolean found = FALSE;

	g_return_val_if_fail (mac != NULL, FALSE);
	g_return_val_if_fail (filename != NULL, FALSE);

	config = g_key_file_new ();
	if (!config) {
		g_warning ("%s: not enough memory to load config file.", __func__);
		return FALSE;
	}

	g_key_file_set_list_separator (config, ',');
	if (!g_key_file_load_from_file (config, filename, G_KEY_FILE_NONE, NULL))
		goto out;

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, NULL, NULL);
	for (iter = list; iter && *iter; iter++) {
		struct ether_addr *candidate;

		candidate = ether_aton (*iter);
		if (candidate && !memcmp (mac->data, candidate->ether_addr_octet, ETH_ALEN)) {
			found = TRUE;
			break;
		}
	}

	if (list)
		g_strfreev (list);

out:
	g_key_file_free (config);
	return found;
}

static void
default_wired_deleted (NMDefaultWiredConnection *wired,
                       const GByteArray *mac,
                       WiredDeviceInfo *info)
{
	NMConnection *wrapped;
	NMSettingConnection *s_con;
	char *tmp;
	GKeyFile *config;
	char **list, **iter, **updated;
	gboolean found = FALSE;
	gsize len = 0;
	char *data;

	/* If there was no config file specified, there's nothing to do */
	if (!info->app->config)
		goto cleanup;

	/* When the default wired connection is removed (either deleted or saved
	 * to a new persistent connection by a plugin), write the MAC address of
	 * the wired device to the config file and don't create a new default wired
	 * connection for that device again.
	 */

	wrapped = nm_exported_connection_get_connection (NM_EXPORTED_CONNECTION (wired));
	g_assert (wrapped);
	s_con = (NMSettingConnection *) nm_connection_get_setting (wrapped, NM_TYPE_SETTING_CONNECTION);
	g_assert (s_con);

	/* Ignore removals of read-only connections, since they couldn't have
	 * been removed by the user.
	 */
	if (nm_setting_connection_get_read_only (s_con))
		goto cleanup;

	config = g_key_file_new ();
	if (!config)
		goto cleanup;

	g_key_file_set_list_separator (config, ',');
	g_key_file_load_from_file (config, info->app->config, G_KEY_FILE_KEEP_COMMENTS, NULL);

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, &len, NULL);
	/* Traverse entire list to get count of # items */
	for (iter = list; iter && *iter; iter++) {
		struct ether_addr *candidate;

		candidate = ether_aton (*iter);
		if (candidate && !memcmp (mac->data, candidate->ether_addr_octet, ETH_ALEN))
			found = TRUE;
	}

	/* Add this device's MAC to the list */
	if (!found) {
		tmp = g_strdup_printf ("%02x:%02x:%02x:%02x:%02x:%02x",
		                       mac->data[0], mac->data[1], mac->data[2],
		                       mac->data[3], mac->data[4], mac->data[5]);

		updated = g_malloc0 (sizeof (char*) * (len + 2));
		if (list && len)
			memcpy (updated, list, len);
		updated[len] = tmp;

		g_key_file_set_string_list (config,
		                            "main", CONFIG_KEY_NO_AUTO_DEFAULT,
		                            (const char **) updated,
		                            len + 1);
		/* g_free() not g_strfreev() since 'updated' isn't a deep-copy */
		g_free (updated);
		g_free (tmp);

		data = g_key_file_to_data (config, &len, NULL);
		if (data) {
			g_file_set_contents (info->app->config, data, len, NULL);
			g_free (data);
		}
	}

	if (list)
		g_strfreev (list);
	g_key_file_free (config);

cleanup:
	/* Clear the connection first so that a 'removed' signal doesn't get emitted
	 * during wired_device_info_destroy(), becuase this connection removal
	 * is expected and already handled.
	 */
	g_object_unref (wired);
	info->connection = NULL;

	g_hash_table_remove (info->app->wired_devices, info->udi);
}

static GError *
default_wired_try_update (NMDefaultWiredConnection *wired,
                          GHashTable *new_settings,
                          WiredDeviceInfo *info)
{
	GError *error = NULL;
	NMConnection *wrapped;
	NMSettingConnection *s_con;
	const char *id;

	/* Try to move this default wired conneciton to a plugin so that it has
	 * persistent storage.
	 */

	wrapped = nm_exported_connection_get_connection (NM_EXPORTED_CONNECTION (wired));
	g_assert (wrapped);
	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (wrapped, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);
	id = nm_setting_connection_get_id (s_con);
	g_assert (id);

	nm_sysconfig_settings_remove_connection (info->app->settings, NM_EXPORTED_CONNECTION (wired), FALSE);
	if (nm_sysconfig_settings_add_new_connection (info->app->settings, new_settings, &error)) {
		g_message ("Saved default wired connection '%s' to persistent storage", id);
		return NULL;
	}

	g_warning ("%s: couldn't save default wired connection '%s': %d / %s",
	           __func__, id, error ? error->code : -1,
	           (error && error->message) ? error->message : "(unknown)");

	/* If there was an error, don't destroy the default wired connection,
	 * but add it back to the system settings service. Connection is already
	 * exported on the bus, don't export it again, thus do_export == FALSE.
	 */
	nm_sysconfig_settings_add_connection (info->app->settings, NM_EXPORTED_CONNECTION (wired), FALSE);

	return error;
}

static gboolean
add_default_wired_connection (gpointer user_data)
{
	WiredDeviceInfo *info = (WiredDeviceInfo *) user_data;
	GByteArray *mac = NULL;
	struct ether_addr tmp;
	char *iface = NULL;
	NMSettingConnection *s_con;
	NMConnection *wrapped;
	gboolean read_only = TRUE;
	const char *id;

	info->add_id = 0;
	g_assert (info->connection == NULL);

	/* If the device isn't managed, ignore it */
	if (!nm_sysconfig_settings_is_device_managed (info->app->settings, info->udi))
		goto ignore;

	iface = get_details_for_udi (info->app, info->udi, &tmp);
	if (!iface)
		goto ignore;

	mac = g_byte_array_sized_new (ETH_ALEN);
	g_byte_array_append (mac, tmp.ether_addr_octet, ETH_ALEN);

	if (have_connection_for_device (info->app, mac))
		goto ignore;

	if (info->app->config && is_mac_auto_wired_blacklisted (mac, info->app->config))
		goto ignore;

	if (nm_sysconfig_settings_get_plugin (info->app->settings, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS))
		read_only = FALSE;

	info->connection = nm_default_wired_connection_new (mac, iface, read_only);
	if (!info->connection)
		goto ignore;

	wrapped = nm_exported_connection_get_connection (NM_EXPORTED_CONNECTION (info->connection));
	g_assert (wrapped);
	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (wrapped, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);
	id = nm_setting_connection_get_id (s_con);
	g_assert (id);

	g_message ("Added default wired connection '%s' for %s", id, info->udi);

	info->updated_id = g_signal_connect (info->connection, "try-update",
	                                     (GCallback) default_wired_try_update, info);
	info->deleted_id = g_signal_connect (info->connection, "deleted",
	                                     (GCallback) default_wired_deleted, info);
	nm_sysconfig_settings_add_connection (info->app->settings,
	                                      NM_EXPORTED_CONNECTION (info->connection),
	                                      TRUE);
	return FALSE;

ignore:
	if (mac)
		g_byte_array_free (mac, TRUE);
	g_free (iface);
	g_hash_table_remove (info->app->wired_devices, info->udi);
	return FALSE;
}

static void
device_added_cb (DBusGProxy *proxy, const char *udi, NMDeviceType devtype, gpointer user_data)
{
	Application *app = (Application *) user_data;
	WiredDeviceInfo *info;

	if (devtype != NM_DEVICE_TYPE_ETHERNET)
		return;

	/* Wait for a plugin to figure out if the device should be managed or not */
	info = g_malloc0 (sizeof (WiredDeviceInfo));
	info->app = app;
	info->add_id = g_timeout_add_seconds (4, add_default_wired_connection, info);
	info->udi = g_strdup (udi);
	g_hash_table_insert (app->wired_devices, info->udi, info);
}

static void
device_removed_cb (DBusGProxy *proxy, const char *udi, NMDeviceType devtype, gpointer user_data)
{
	Application *app = (Application *) user_data;

	g_hash_table_remove (app->wired_devices, udi);
}

/******************************************************************/

static void
dbus_cleanup (Application *app)
{
	if (app->g_connection) {
		dbus_g_connection_unref (app->g_connection);
		app->g_connection = NULL;
		app->connection = NULL;
	}

	if (app->bus_proxy) {
		g_signal_handlers_disconnect_by_func (app->bus_proxy, destroy_cb, app);
		g_object_unref (app->bus_proxy);
		app->bus_proxy = NULL;
	}
}

static void
destroy_cb (DBusGProxy *proxy, gpointer user_data)
{
	/* Clean up existing connection */
	g_warning ("disconnected from the system bus, exiting.");
	g_main_loop_quit (loop);
}

static gboolean
start_dbus_service (Application *app)
{
	int request_name_result;
	GError *err = NULL;

	if (!dbus_g_proxy_call (app->bus_proxy, "RequestName", &err,
							G_TYPE_STRING, NM_DBUS_SERVICE_SYSTEM_SETTINGS,
							G_TYPE_UINT, DBUS_NAME_FLAG_DO_NOT_QUEUE,
							G_TYPE_INVALID,
							G_TYPE_UINT, &request_name_result,
							G_TYPE_INVALID)) {
		g_warning ("Could not acquire the NetworkManagerSystemSettings service.\n"
		           "  Message: '%s'", err->message);
		g_error_free (err);
		return FALSE;
	}

	if (request_name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		g_warning ("Could not acquire the NetworkManagerSystemSettings service "
		           "as it is already taken.  Return: %d",
		           request_name_result);
		return FALSE;
	}

	return TRUE;
}

static gboolean
dbus_init (Application *app)
{
	GError *err = NULL;
	
	dbus_connection_set_change_sigpipe (TRUE);

	app->g_connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &err);
	if (!app->g_connection) {
		g_warning ("Could not get the system bus.  Make sure "
		           "the message bus daemon is running!  Message: %s",
		           err->message);
		g_error_free (err);
		return FALSE;
	}

	app->connection = dbus_g_connection_get_connection (app->g_connection);
	dbus_connection_set_exit_on_disconnect (app->connection, FALSE);

	app->bus_proxy = dbus_g_proxy_new_for_name (app->g_connection,
	                                            "org.freedesktop.DBus",
	                                            "/org/freedesktop/DBus",
	                                            "org.freedesktop.DBus");
	if (!app->bus_proxy) {
		g_warning ("Could not get the DBus object!");
		return FALSE;
	}

	g_signal_connect (app->bus_proxy, "destroy", G_CALLBACK (destroy_cb), app);

	return TRUE;
}

static gboolean
parse_config_file (const char *filename, char **plugins, GError **error)
{
	GKeyFile *config;

	config = g_key_file_new ();
	if (!config) {
		g_set_error (error, plugins_error_quark (), 0,
		             "Not enough memory to load config file.");
		return FALSE;
	}

	g_key_file_set_list_separator (config, ',');
	if (!g_key_file_load_from_file (config, filename, G_KEY_FILE_NONE, error))
		return FALSE;

	*plugins = g_key_file_get_value (config, "main", "plugins", error);
	if (*error)
		return FALSE;

	g_key_file_free (config);
	return TRUE;
}

static void
log_handler (const gchar *log_domain,
             GLogLevelFlags log_level,
             const gchar *message,
             gpointer ignored)
{
	int syslog_priority;	

	switch (log_level) {
		case G_LOG_LEVEL_ERROR:
			syslog_priority = LOG_CRIT;
			break;

		case G_LOG_LEVEL_CRITICAL:
			syslog_priority = LOG_ERR;
			break;

		case G_LOG_LEVEL_WARNING:
			syslog_priority = LOG_WARNING;
			break;

		case G_LOG_LEVEL_MESSAGE:
			syslog_priority = LOG_NOTICE;
			break;

		case G_LOG_LEVEL_DEBUG:
			syslog_priority = LOG_DEBUG;
			break;

		case G_LOG_LEVEL_INFO:
		default:
			syslog_priority = LOG_INFO;
			break;
	}

	syslog (syslog_priority, "%s", message);
}


static void
logging_setup (void)
{
	openlog (G_LOG_DOMAIN, LOG_CONS, LOG_DAEMON);
	g_log_set_handler (G_LOG_DOMAIN, 
	                   G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
	                   log_handler,
	                   NULL);
}

static void
logging_shutdown (void)
{
	closelog ();
}

static void
signal_handler (int signo)
{
	if (signo == SIGINT || signo == SIGTERM) {
		if (debug)
			g_message ("Caught signal %d, shutting down...", signo);
		g_main_loop_quit (loop);
	}
}

static void
setup_signals (void)
{
	struct sigaction action;
	sigset_t mask;

	sigemptyset (&mask);
	action.sa_handler = signal_handler;
	action.sa_mask = mask;
	action.sa_flags = 0;
	sigaction (SIGTERM,  &action, NULL);
	sigaction (SIGINT,  &action, NULL);
}

int
main (int argc, char **argv)
{
	Application *app = g_new0 (Application, 1);
	GOptionContext *opt_ctx;
	GError *error = NULL;
	char *plugins = NULL;
	char *config = NULL;

	GOptionEntry entries[] = {
		{ "config", 0, 0, G_OPTION_ARG_FILENAME, &config, "Config file location", "/path/to/config.file" },
		{ "plugins", 0, 0, G_OPTION_ARG_STRING, &plugins, "List of plugins separated by ,", "plugin1,plugin2" },
		{ "debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Output to console rather than syslog", NULL },
		{ NULL }
	};

	opt_ctx = g_option_context_new (NULL);
	g_option_context_set_summary (opt_ctx, "Provides system network settings to NetworkManager.");
	g_option_context_add_main_entries (opt_ctx, entries, NULL);

	if (!g_option_context_parse (opt_ctx, &argc, &argv, &error)) {
		g_warning ("%s\n", error->message);
		g_error_free (error);
		return 1;
	}

	g_option_context_free (opt_ctx);

	if (config) {
		app->config = config;
		if (!parse_config_file (app->config, &plugins, &error)) {
			g_warning ("Invalid config file: %s.", error->message);
			return 1;
		}
	}

	if (!plugins) {
		g_warning ("No plugins were specified.");
		return 1;
	}

	g_type_init ();

	if (!g_module_supported ()) {
		g_warning ("GModules are not supported on your platform!");
		return 1;
	}

	loop = g_main_loop_new (NULL, FALSE);

	if (!debug)
		logging_setup ();

	if (!dbus_init (app))
		return -1;

	app->hal_mgr = nm_system_config_hal_manager_get (app->g_connection);
	app->settings = nm_sysconfig_settings_new (app->g_connection, app->hal_mgr);

	app->wired_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                            g_free, wired_device_info_destroy);
	g_signal_connect (G_OBJECT (app->hal_mgr), "device-added",
	                  G_CALLBACK (device_added_cb), app);
	g_signal_connect (G_OBJECT (app->hal_mgr), "device-removed",
	                  G_CALLBACK (device_removed_cb), app);

	/* Load the plugins; fail if a plugin is not found. */
	load_plugins (app, plugins, &error);
	if (error) {
		g_warning ("Error: %d - %s", error->code, error->message);
		return -1;
	}
	g_free (plugins);

	setup_signals ();

	g_idle_add (load_stuff, app);

	g_main_loop_run (loop);

	nm_system_config_hal_manager_shutdown (app->hal_mgr);
	g_object_unref (app->hal_mgr);

	g_hash_table_destroy (app->wired_devices);

	g_object_unref (app->settings);
	dbus_cleanup (app);

	if (!debug)
		logging_shutdown ();

	return 0;
}

