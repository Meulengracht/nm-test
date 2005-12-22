/* nm-tool - information tool for NetworkManager
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
 * (C) Copyright 2005 Red Hat, Inc.
 */

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "NetworkManager.h"
#include "nm-utils.h"


static gboolean get_nm_state (DBusConnection *connection)
{
	dbus_uint32_t	uint32_state;
	char *		state_string = NULL;
	gboolean		success = TRUE;
	DBusMessage *	message = NULL;
	DBusMessage *	reply = NULL;

	if (!(message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH, NM_DBUS_INTERFACE, "state")))
	{
		fprintf (stderr, "get_nm_state(): couldn't create new dbus message.\n");
		return FALSE;
	}

	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, NULL);
	dbus_message_unref (message);
	if (!reply)
	{
		fprintf (stderr, "get_nm_state(): didn't get a reply from NetworkManager.\n");
		return FALSE;
	}

	if (!dbus_message_get_args (reply, NULL, DBUS_TYPE_UINT32, &uint32_state, DBUS_TYPE_INVALID))
	{
		fprintf (stderr, "get_nm_state(): unexpected reply from NetworkManager.\n");
		return FALSE;
	}

	switch ((NMState) uint32_state)
	{
		case NM_STATE_ASLEEP:
			state_string = "asleep";
			break;

		case NM_STATE_CONNECTING:
			state_string = "connecting";
			break;

		case NM_STATE_CONNECTED:
			state_string = "connected";
			break;

		case NM_STATE_DISCONNECTED:
			state_string = "disconnected";
			break;

		case NM_STATE_UNKNOWN:
		default:
			state_string = "unknown";
			success = FALSE;
			break;
	}
	fprintf (stderr, "State: %s\n\n", state_string);

	return success;
}

static void print_string (const char *label, const char *data)
{
#define SPACING 18
	int label_len = 0;
	char spaces[50];
	int i;

	g_return_if_fail (label != NULL);
	g_return_if_fail (data != NULL);

	label_len = strlen (label);
	if (label_len > SPACING)
		label_len = SPACING - 1;
	for (i = 0; i < (SPACING - label_len); i++)
		spaces[i] = 0x20;
	spaces[i] = 0x00;

	fprintf (stdout, "  %s:%s%s\n", label, &spaces[0], data);
}


static void detail_network (DBusConnection *connection, const char *path, const char *active_path)
{
	DBusMessage *		message = NULL;
	DBusMessage *		reply = NULL;
	const char *		op = NULL;
	const char *		essid = NULL;
	const char *		hw_addr = NULL;
	dbus_int32_t		strength = -1;
	double 			freq = 0;
	dbus_int32_t		rate = 0;
	dbus_bool_t		enc = FALSE;
	dbus_uint32_t		mode = 0;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (path != NULL);

	if (!(message = dbus_message_new_method_call (NM_DBUS_SERVICE, path, NM_DBUS_INTERFACE_DEVICES, "getProperties")))
	{
		fprintf (stderr, "detail_network(): couldn't create new dbus message.\n");
		return;
	}

	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, NULL);
	dbus_message_unref (message);
	if (!reply)
	{
		fprintf (stderr, "detail_network(): didn't get a reply from NetworkManager for device %s.\n", path);
		return;
	}

	if (dbus_message_get_args (reply, NULL,	DBUS_TYPE_OBJECT_PATH, &op,
									DBUS_TYPE_STRING, &essid,
									DBUS_TYPE_STRING, &hw_addr,
									DBUS_TYPE_INT32,  &strength,
									DBUS_TYPE_DOUBLE, &freq,
									DBUS_TYPE_INT32,  &rate,
									DBUS_TYPE_BOOLEAN,&enc,
									DBUS_TYPE_UINT32, &mode,
									DBUS_TYPE_INVALID))
	{
		char *temp = NULL;
		char *temp_essid = NULL;
		float flt_freq = freq / 1000000000;
		gboolean active = (active_path && !strcmp (active_path, path)) ? TRUE : FALSE;

		temp = g_strdup_printf ("%s Mode, Freq %.3f MHz, Strength %d%%%s", (mode == NETWORK_MODE_INFRA) ? "Infrastructure" : "Ad-Hoc", 
					flt_freq, strength, enc ? ", Encrypted" : "");
		temp_essid = g_strdup_printf ("  %s%s", active ? "*" : "", essid);
		print_string (temp_essid, temp);
		g_free (temp_essid);
		g_free (temp);
	}
	else
		fprintf (stderr, "detail_network(): unexpected reply from NetworkManager for device %s.\n", path);

	dbus_message_unref (reply);
}


static void detail_device (DBusConnection *connection, const char *path)
{
	DBusMessage *		message = NULL;
	DBusMessage *		reply = NULL;
	char *			op = NULL;
	const char *		iface = NULL;
	dbus_uint32_t		type = 0;
	const char *		udi = NULL;
	dbus_bool_t		active = FALSE;
	const char *		ip4_address = NULL;
	const char *		broadcast = NULL;
	const char *		subnetmask = NULL;
	const char *		hw_addr = NULL;
	const char *		route = NULL;
	const char *		primary_dns = NULL;
	const char *		secondary_dns = NULL;
	dbus_uint32_t		mode = 0;
	dbus_int32_t		strength = -1;
	char *			active_network_path = NULL;
	dbus_bool_t		link_active = FALSE;
	dbus_uint32_t		caps = NM_DEVICE_CAP_NONE;
	char **			networks = NULL;
	int				num_networks = 0;
	NMActStage		act_stage = NM_ACT_STAGE_UNKNOWN;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (path != NULL);

	if (!(message = dbus_message_new_method_call (NM_DBUS_SERVICE, path, NM_DBUS_INTERFACE_DEVICES, "getProperties")))
	{
		fprintf (stderr, "detail_device(): couldn't create new dbus message.\n");
		return;
	}

	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, NULL);
	dbus_message_unref (message);
	if (!reply)
	{
		fprintf (stderr, "detail_device(): didn't get a reply from NetworkManager for device %s.\n", path);
		return;
	}

	if (dbus_message_get_args (reply, NULL,	DBUS_TYPE_OBJECT_PATH, &op,
									DBUS_TYPE_STRING, &iface,
									DBUS_TYPE_UINT32, &type,
									DBUS_TYPE_STRING, &udi,
									DBUS_TYPE_BOOLEAN,&active,
									DBUS_TYPE_UINT32, &act_stage,
									DBUS_TYPE_STRING, &ip4_address,
									DBUS_TYPE_STRING, &subnetmask,
									DBUS_TYPE_STRING, &broadcast,
									DBUS_TYPE_STRING, &hw_addr,
									DBUS_TYPE_STRING, &route,
									DBUS_TYPE_STRING, &primary_dns,
									DBUS_TYPE_STRING, &secondary_dns,
									DBUS_TYPE_UINT32, &mode,
									DBUS_TYPE_INT32,  &strength,
									DBUS_TYPE_BOOLEAN,&link_active,
									DBUS_TYPE_UINT32, &caps,
									DBUS_TYPE_STRING, &active_network_path,
									DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &networks, &num_networks,
									DBUS_TYPE_INVALID))
	{
		fprintf (stdout, "- Device: %s ----------------------------------------------------------------\n", iface);

		/* General information */
		print_string ("NM Path", op);
		if (type == DEVICE_TYPE_WIRELESS_ETHERNET)
			print_string ("Type", "802.11 Wireless");
		else if (type == DEVICE_TYPE_WIRED_ETHERNET)
			print_string ("Type", "Wired");
		if (active)
			print_string ("Active", "yes");
		else
			print_string ("Active", "no");

		print_string ("HW Address", hw_addr);

		/* Capabilities */
		fprintf (stderr, "\n  Capabilities:\n");
		if (caps & NM_DEVICE_CAP_NM_SUPPORTED)
			print_string ("  Supported", "yes");
		else
			print_string ("  Supported", "no");
		if (caps & NM_DEVICE_CAP_CARRIER_DETECT)
			print_string ("  Carrier Detect", "yes");
		if (caps & NM_DEVICE_CAP_WIRELESS_SCAN)
			print_string ("  Scanning", "yes");

		/* Wireless specific information */
		if (type == DEVICE_TYPE_WIRELESS_ETHERNET)
		{
			char *str_strength;
			int	 i;

			/*
			fprintf (stdout, "\n  Wireless Settings\n");
			if (mode == NETWORK_MODE_INFRA)
				print_string ("  Mode", "Infrastructure");
			else if (mode == NETWORK_MODE_ADHOC)
				print_string ("  Mode", "Ad-Hoc");
			str_strength = g_strdup_printf ("%d%%", strength);
			print_string ("  Strength", str_strength);
			g_free (str_strength);
			*/

			fprintf (stdout, "\n  Wireless Networks (* = Current Network)\n");
			for (i = 0; i < num_networks; i++)
				detail_network (connection, networks[i], active_network_path);
		}
		else if (type == DEVICE_TYPE_WIRED_ETHERNET)
		{
			fprintf (stdout, "\n  Wired Settings\n");
			if (link_active)
				print_string ("  Hardware Link", "yes");
			else
				print_string ("  Hardware Link", "no");
		}

		/* IP Setup info */
		if (active)
		{
			fprintf (stdout, "\n  IP Settings:\n");
			print_string ("  IP Address", ip4_address);
			print_string ("  Subnet Mask", subnetmask);
			print_string ("  Broadcast", broadcast);
			print_string ("  Gateway", route);
			print_string ("  Primary DNS", primary_dns);
			print_string ("  Secondary DNS", secondary_dns);
		}
		

		fprintf (stdout, "\n\n");
		dbus_free_string_array (networks);
	}
	else
		fprintf (stderr, "detail_device(): unexpected reply from NetworkManager for device %s.\n", path);

	dbus_message_unref (reply);
}


static void print_devices (DBusConnection *connection)
{
	DBusMessage *	message = NULL;
	DBusMessage *	reply = NULL;
	DBusError		error;
	char **		paths = NULL;
	int			num = -1;
	int			i;

	if (!(message = dbus_message_new_method_call (NM_DBUS_SERVICE, NM_DBUS_PATH, NM_DBUS_INTERFACE, "getDevices")))
	{
		fprintf (stderr, "print_devices(): couldn't create new dbus message.\n");
		return;
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	dbus_message_unref (message);
	if (!reply)
	{
		fprintf (stderr, "print_devices(): didn't get a reply from NetworkManager.\n");
		if (dbus_error_is_set (&error))
		{
			if (dbus_error_has_name (&error, NM_DBUS_NO_DEVICES_ERROR))
				fprintf (stderr, "There are no available network devices.\n");
		}
		else
			fprintf (stderr, "print_devices(): NetworkManager returned an error: '%s'\n", error.message);
		return;
	}

	if (!dbus_message_get_args (reply, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_OBJECT_PATH, &paths, &num, DBUS_TYPE_INVALID))
	{
		fprintf (stderr, "print_devices(): unexpected reply from NetworkManager.\n");
		dbus_message_unref (reply);
		return;
	}

	for (i = 0; i < num; i++)
		detail_device (connection, paths[i]);

	dbus_free_string_array (paths);
	dbus_message_unref (reply);
}


int main( int argc, char *argv[] )
{
	DBusConnection *connection;
	DBusError		error;

	g_type_init ();

	dbus_error_init (&error);
	connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (connection == NULL)
	{
		fprintf (stderr, "Error connecting to system bus: %s\n", error.message);
		dbus_error_free (&error);
		return 1;
	}

	fprintf (stdout, "\nNetworkManager Tool\n\n");

	if (!get_nm_state (connection))
	{
		fprintf (stdout, "\n\nNetworkManager appears not to be running (could not get its state).\n");
		exit (1);
	}

	print_devices (connection);

	return 0;
}
