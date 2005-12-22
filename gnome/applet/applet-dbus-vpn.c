/* -*- Mode: C; tab-width: 5; indent-tabs-mode: t; c-basic-offset: 5 -*- */
/* NetworkManager Wireless Applet -- Display wireless access points and allow user control
 *
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2004-2005 Red Hat, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <dbus/dbus.h>
#include "applet-dbus-vpn.h"
#include "applet-dbus.h"
#include "applet.h"
#include "vpn-connection.h"
#include "nm-utils.h"

static void nmwa_free_vpn_connections (NMWirelessApplet *applet);


/*
 * nmwa_dbus_vpn_update_vpn_connection_state
 *
 * Sets the state for a dbus vpn connection and schedules a copy to the applet gui.
 */
void nmwa_dbus_vpn_update_vpn_connection_state (NMWirelessApplet *applet, const char *vpn_name, NMVPNActStage vpn_state)
{
	VPNConnection	*vpn;

	g_return_if_fail (applet != NULL);

	if ((vpn = nmwa_vpn_connection_find_by_name (applet->vpn_connections, vpn_name)))
		nmwa_vpn_connection_set_state (vpn, vpn_state);
}

typedef struct VpnPropsCBData
{
	NMWirelessApplet *	applet;	
	char *			name;
} VpnPropsCBData;

static void free_vpn_props_cb_data (VpnPropsCBData *data)
{
	if (data)
	{
		g_free (data->name);
		memset (data, 0, sizeof (VpnPropsCBData));
		g_free (data);
	}
}

/*
 * nmwa_dbus_vpn_properties_cb
 *
 * Callback for each VPN connection we called "getVPNConnectionProperties" on.
 *
 */
static void nmwa_dbus_vpn_properties_cb (DBusPendingCall *pcall, void *user_data)
{
	DBusMessage *		reply;
	VpnPropsCBData *	cb_data = user_data;
	NMWirelessApplet *	applet;
	const char *		name;
	const char *        user_name;
	const char *        service;
	NMVPNActStage		state;
	dbus_uint32_t		state_int;
	
	g_return_if_fail (pcall != NULL);
	g_return_if_fail (cb_data != NULL);
	g_return_if_fail (cb_data->applet != NULL);
	g_return_if_fail (cb_data->name != NULL);

	applet = cb_data->applet;

	if (!(reply = dbus_pending_call_steal_reply (pcall)))
		goto out;

	if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR)
	{
		dbus_message_unref (reply);
		goto out;
	}

	if (dbus_message_get_args (reply, NULL,	DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &user_name,
				DBUS_TYPE_STRING, &service, DBUS_TYPE_UINT32, &state_int, DBUS_TYPE_INVALID))
	{
		VPNConnection *	vpn;

		state = (NMVPNActStage) state_int;

		/* If its already there, update the service, otherwise add it to the list */
		if ((vpn = nmwa_vpn_connection_find_by_name (applet->vpn_connections, name)))
		{
			nmwa_vpn_connection_set_service (vpn, service);
			nmwa_vpn_connection_set_state (vpn, state);
		}
		else
		{
			vpn = nmwa_vpn_connection_new (name);
			nmwa_vpn_connection_set_service (vpn, service);
			nmwa_vpn_connection_set_state (vpn, state);
			applet->vpn_connections = g_slist_append (applet->vpn_connections, vpn);
		}
	}
	dbus_message_unref (reply);

out:
	dbus_pending_call_unref (pcall);
}


/*
 * nmwa_dbus_vpn_update_one_vpn_connection
 *
 * Get properties on one VPN connection
 *
 */
void nmwa_dbus_vpn_update_one_vpn_connection (NMWirelessApplet *applet, const char *vpn_name)
{
	DBusMessage *		message;
	DBusPendingCall *	pcall = NULL;

	g_return_if_fail (applet != NULL);
	g_return_if_fail (vpn_name != NULL);

	nmwa_get_first_active_vpn_connection (applet);

	if ((message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH_VPN, NM_DBUS_INTERFACE_VPN, "getVPNConnectionProperties")))
	{
		dbus_message_append_args (message, DBUS_TYPE_STRING, &vpn_name, DBUS_TYPE_INVALID);
		dbus_connection_send_with_reply (applet->connection, message, &pcall, -1);
		dbus_message_unref (message);
		if (pcall)
		{
			VpnPropsCBData *	cb_data = g_malloc0 (sizeof (VpnPropsCBData));

			cb_data->applet = applet;
			cb_data->name = g_strdup (vpn_name);
			dbus_pending_call_set_notify (pcall, nmwa_dbus_vpn_properties_cb, cb_data, (DBusFreeFunction) free_vpn_props_cb_data);
		}
	}
}


/*
 * nmwa_dbus_vpn_update_vpn_connections_cb
 *
 * nmwa_dbus_vpn_update_vpn_connections callback.
 *
 */
static void nmwa_dbus_vpn_update_vpn_connections_cb (DBusPendingCall *pcall, void *user_data)
{
	DBusMessage *		reply;
	NMWirelessApplet *	applet = (NMWirelessApplet *) user_data;
	char **			vpn_names;
	int				num_vpn_names;

	g_return_if_fail (pcall != NULL);
	g_return_if_fail (applet != NULL);

	if (!(reply = dbus_pending_call_steal_reply (pcall)))
		goto out;

	if (dbus_message_is_error (reply, NM_DBUS_NO_VPN_CONNECTIONS))
	{
		dbus_message_unref (reply);
		goto out;
	}

	if (dbus_message_get_args (reply, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &vpn_names, &num_vpn_names, DBUS_TYPE_INVALID))
	{
		char ** item;

		/* For each connection, fire off a "getVPNConnectionProperties" call */
		for (item = vpn_names; *item; item++)
			nmwa_dbus_vpn_update_one_vpn_connection (applet, *item);

		dbus_free_string_array (vpn_names);
	}
	dbus_message_unref (reply);

out:
	dbus_pending_call_unref (pcall);
}


/*
 * nmwa_dbus_vpn_update_vpn_connections
 *
 * Do a full update of vpn connections from NetworkManager
 *
 */
void nmwa_dbus_vpn_update_vpn_connections (NMWirelessApplet *applet)
{
	DBusMessage *		message;
	DBusPendingCall *	pcall;

	nmwa_free_vpn_connections (applet);

	nmwa_get_first_active_vpn_connection (applet);

	if ((message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH_VPN, NM_DBUS_INTERFACE_VPN, "getVPNConnections")))
	{
		dbus_connection_send_with_reply (applet->connection, message, &pcall, -1);
		dbus_message_unref (message);
		if (pcall)
			dbus_pending_call_set_notify (pcall, nmwa_dbus_vpn_update_vpn_connections_cb, applet, NULL);
	}
}


/*
 * nmwa_dbus_vpn_remove_one_vpn_connection
 *
 * Remove one vpn connection from the list
 *
 */
void nmwa_dbus_vpn_remove_one_vpn_connection (NMWirelessApplet *applet, const char *vpn_name)
{
	VPNConnection *	vpn;

	g_return_if_fail (applet != NULL);
	g_return_if_fail (vpn_name != NULL);

	if ((vpn = nmwa_vpn_connection_find_by_name (applet->vpn_connections, vpn_name)))
	{
		applet->vpn_connections = g_slist_remove (applet->vpn_connections, vpn);
		nmwa_vpn_connection_unref (vpn);
	}
}

static void nmwa_free_vpn_connections (NMWirelessApplet *applet)
{
	g_return_if_fail (applet != NULL);

	if (applet->vpn_connections)
	{
		g_slist_foreach (applet->vpn_connections, (GFunc) nmwa_vpn_connection_unref, NULL);
		g_slist_free (applet->vpn_connections);
		applet->vpn_connections = NULL;
	}
}


/*
 * nmwa_dbus_vpn_activate_connection
 *
 * Tell NetworkManager to activate a particular VPN connection.
 *
 */
void nmwa_dbus_vpn_activate_connection (DBusConnection *connection, const char *name, GSList *passwords)
{
	DBusMessage	*message;
	DBusMessageIter	 iter;
	DBusMessageIter	 iter_array;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (name != NULL);
	g_return_if_fail (passwords != NULL);

	if ((message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH_VPN, NM_DBUS_INTERFACE_VPN, "activateVPNConnection")))
	{
		GSList *i;

		nm_info ("Activating VPN connection '%s'.\n", name);
		dbus_message_iter_init_append (message, &iter);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &iter_array);

		for (i = passwords; i != NULL; i = g_slist_next (i)) {
			dbus_message_iter_append_basic (&iter_array, DBUS_TYPE_STRING, &(i->data));
		}
		dbus_message_iter_close_container (&iter, &iter_array);
		dbus_connection_send (connection, message, NULL);
	}
	else
		nm_warning ("nmwa_dbus_activate_vpn_connection(): Couldn't allocate the dbus message\n");
}


/*
 * nmwa_dbus_deactivate_vpn_connection
 *
 * Tell NetworkManager to deactivate the currently active VPN connection.
 *
 */
void nmwa_dbus_vpn_deactivate_connection (DBusConnection *connection)
{
	DBusMessage	*message;

	g_return_if_fail (connection != NULL);

	if ((message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH_VPN, NM_DBUS_INTERFACE_VPN, "deactivateVPNConnection")))
	{
		nm_info ("Deactivating the current VPN connection.\n");
		dbus_connection_send (connection, message, NULL);
	}
	else
		nm_warning ("nmwa_dbus_activate_vpn_connection(): Couldn't allocate the dbus message\n");
}


