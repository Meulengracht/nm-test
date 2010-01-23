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
 * Copyright (C) 2009 Red Hat, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <net/ethernet.h>

#include "nm-glib-compat.h"
#include "nm-bluez-common.h"
#include "nm-dbus-manager.h"
#include "nm-device-bt.h"
#include "nm-device-interface.h"
#include "nm-device-private.h"
#include "nm-utils.h"
#include "nm-marshal.h"
#include "ppp-manager/nm-ppp-manager.h"
#include "nm-properties-changed-signal.h"
#include "nm-setting-connection.h"
#include "nm-setting-bluetooth.h"
#include "nm-setting-cdma.h"
#include "nm-setting-gsm.h"
#include "nm-device-bt-glue.h"
#include "NetworkManagerUtils.h"

#define BLUETOOTH_DUN_UUID "dun"
#define BLUETOOTH_NAP_UUID "nap"

G_DEFINE_TYPE (NMDeviceBt, nm_device_bt, NM_TYPE_DEVICE)

#define NM_DEVICE_BT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE_BT, NMDeviceBtPrivate))

typedef struct {
	char *bdaddr;
	char *name;
	guint32 capabilities;

	DBusGProxy *type_proxy;

	NMPPPManager *ppp_manager;
	char *rfcomm_iface;
	guint32 in_bytes;
	guint32 out_bytes;

	NMIP4Config *pending_ip4_config;
	guint32 bt_type;  /* BT type of the current connection */
} NMDeviceBtPrivate;

enum {
	PROP_0,
	PROP_HW_ADDRESS,
	PROP_BT_NAME,
	PROP_BT_CAPABILITIES,

	LAST_PROP
};

enum {
	PPP_STATS,
	PROPERTIES_CHANGED,

	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };


typedef enum {
	NM_BT_ERROR_CONNECTION_NOT_BT = 0,
	NM_BT_ERROR_CONNECTION_INVALID,
	NM_BT_ERROR_CONNECTION_INCOMPATIBLE,
} NMBtError;

#define NM_BT_ERROR (nm_bt_error_quark ())
#define NM_TYPE_BT_ERROR (nm_bt_error_get_type ())

static GQuark
nm_bt_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("nm-bt-error");
	return quark;
}

/* This should really be standard. */
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

static GType
nm_bt_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			/* Connection was not a BT connection. */
			ENUM_ENTRY (NM_BT_ERROR_CONNECTION_NOT_BT, "ConnectionNotBt"),
			/* Connection was not a valid BT connection. */
			ENUM_ENTRY (NM_BT_ERROR_CONNECTION_INVALID, "ConnectionInvalid"),
			/* Connection does not apply to this device. */
			ENUM_ENTRY (NM_BT_ERROR_CONNECTION_INCOMPATIBLE, "ConnectionIncompatible"),
			{ 0, 0, 0 }
		};
		etype = g_enum_register_static ("NMBtError", values);
	}
	return etype;
}


NMDevice *
nm_device_bt_new (const char *udi,
                  const char *bdaddr,
                  const char *name,
                  guint32 capabilities,
                  gboolean managed)
{
	g_return_val_if_fail (udi != NULL, NULL);
	g_return_val_if_fail (bdaddr != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);
	g_return_val_if_fail (capabilities != NM_BT_CAPABILITY_NONE, NULL);

	return (NMDevice *) g_object_new (NM_TYPE_DEVICE_BT,
	                                  NM_DEVICE_INTERFACE_UDI, udi,
	                                  NM_DEVICE_INTERFACE_IFACE, bdaddr,
	                                  NM_DEVICE_INTERFACE_DRIVER, "bluez",
	                                  NM_DEVICE_BT_HW_ADDRESS, bdaddr,
	                                  NM_DEVICE_BT_NAME, name,
	                                  NM_DEVICE_BT_CAPABILITIES, capabilities,
	                                  NM_DEVICE_INTERFACE_MANAGED, managed,
	                                  NM_DEVICE_INTERFACE_TYPE_DESC, "Bluetooth",
	                                  NM_DEVICE_INTERFACE_DEVICE_TYPE, NM_DEVICE_TYPE_BT,
	                                  NULL);
}

guint32 nm_device_bt_get_capabilities (NMDeviceBt *self)
{
	g_return_val_if_fail (self != NULL, NM_BT_CAPABILITY_NONE);
	g_return_val_if_fail (NM_IS_DEVICE_BT (self), NM_BT_CAPABILITY_NONE);

	return NM_DEVICE_BT_GET_PRIVATE (self)->capabilities;
}

const char *nm_device_bt_get_hw_address (NMDeviceBt *self)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE_BT (self), NULL);

	return NM_DEVICE_BT_GET_PRIVATE (self)->bdaddr;
}

static guint32
get_connection_bt_type (NMConnection *connection)
{
	NMSettingBluetooth *s_bt;
	const char *bt_type;

	s_bt = (NMSettingBluetooth *) nm_connection_get_setting (connection, NM_TYPE_SETTING_BLUETOOTH);
	if (!s_bt)
		return NM_BT_CAPABILITY_NONE;

	bt_type = nm_setting_bluetooth_get_connection_type (s_bt);
	g_assert (bt_type);

	if (!strcmp (bt_type, NM_SETTING_BLUETOOTH_TYPE_DUN))
		return NM_BT_CAPABILITY_DUN;
	else if (!strcmp (bt_type, NM_SETTING_BLUETOOTH_TYPE_PANU))
		return NM_BT_CAPABILITY_NAP;

	return NM_BT_CAPABILITY_NONE;
}

static NMConnection *
real_get_best_auto_connection (NMDevice *device,
                               GSList *connections,
                               char **specific_object)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	GSList *iter;

	for (iter = connections; iter; iter = g_slist_next (iter)) {
		NMConnection *connection = NM_CONNECTION (iter->data);
		NMSettingConnection *s_con;
		guint32 bt_type;

		s_con = (NMSettingConnection *) nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION);
		g_assert (s_con);

		if (!nm_setting_connection_get_autoconnect (s_con))
			continue;

		if (strcmp (nm_setting_connection_get_connection_type (s_con), NM_SETTING_BLUETOOTH_SETTING_NAME))
			continue;

		bt_type = get_connection_bt_type (connection);
		if (!(bt_type & priv->capabilities))
			continue;

		return connection;
	}
	return NULL;
}

static gboolean
real_check_connection_compatible (NMDevice *device,
                                  NMConnection *connection,
                                  GError **error)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	NMSettingConnection *s_con;
	NMSettingBluetooth *s_bt;
	const GByteArray *array;
	char *str;
	int addr_match = FALSE;
	guint32 bt_type;

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);

	if (strcmp (nm_setting_connection_get_connection_type (s_con), NM_SETTING_BLUETOOTH_SETTING_NAME)) {
		g_set_error (error,
		             NM_BT_ERROR, NM_BT_ERROR_CONNECTION_NOT_BT,
		             "The connection was not a Bluetooth connection.");
		return FALSE;
	}

	s_bt = NM_SETTING_BLUETOOTH (nm_connection_get_setting (connection, NM_TYPE_SETTING_BLUETOOTH));
	if (!s_bt) {
		g_set_error (error,
		             NM_BT_ERROR, NM_BT_ERROR_CONNECTION_INVALID,
		             "The connection was not a valid Bluetooth connection.");
		return FALSE;
	}

	array = nm_setting_bluetooth_get_bdaddr (s_bt);
	if (!array || (array->len != ETH_ALEN)) {
		g_set_error (error,
		             NM_BT_ERROR, NM_BT_ERROR_CONNECTION_INVALID,
		             "The connection did not contain a valid Bluetooth address.");
		return FALSE;
	}

	bt_type = get_connection_bt_type (connection);
	if (!(bt_type & priv->capabilities)) {
		g_set_error (error,
		             NM_BT_ERROR, NM_BT_ERROR_CONNECTION_INCOMPATIBLE,
		             "The connection was not compatible with the device's capabilities.");
		return FALSE;
	}

	str = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
	                       array->data[0], array->data[1], array->data[2],
	                       array->data[3], array->data[4], array->data[5]);
	addr_match = !strcmp (priv->bdaddr, str);
	g_free (str);

	return addr_match;
}

static guint32
real_get_generic_capabilities (NMDevice *dev)
{
	return NM_DEVICE_CAP_NM_SUPPORTED;
}

/*****************************************************************************/
/* IP method PPP */

static void
ppp_state_changed (NMPPPManager *ppp_manager, NMPPPStatus status, gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	switch (status) {
	case NM_PPP_STATUS_DISCONNECT:
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_PPP_DISCONNECT);
		break;
	case NM_PPP_STATUS_DEAD:
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_PPP_FAILED);
		break;
	default:
		break;
	}
}

static void
ppp_ip4_config (NMPPPManager *ppp_manager,
                const char *iface,
                NMIP4Config *config,
                gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	/* Ignore PPP IP4 events that come in after initial configuration */
	if (nm_device_get_state (device) != NM_DEVICE_STATE_IP_CONFIG)
		return;

	nm_device_set_ip_iface (device, iface);
	NM_DEVICE_BT_GET_PRIVATE (device)->pending_ip4_config = g_object_ref (config);
	nm_device_activate_schedule_stage4_ip4_config_get (device);
}

static void
ppp_stats (NMPPPManager *ppp_manager,
           guint32 in_bytes,
           guint32 out_bytes,
           gpointer user_data)
{
	NMDeviceBt *self = NM_DEVICE_BT (user_data);
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (self);

	if (priv->in_bytes != in_bytes || priv->out_bytes != out_bytes) {
		priv->in_bytes = in_bytes;
		priv->out_bytes = out_bytes;

		g_signal_emit (self, signals[PPP_STATS], 0, in_bytes, out_bytes);
	}
}

static gboolean
get_ppp_credentials (NMConnection *connection,
                     const char **username,
                     const char **password)
{
	NMSettingGsm *s_gsm;
	NMSettingCdma *s_cdma = NULL;

	s_gsm = (NMSettingGsm *) nm_connection_get_setting (connection, NM_TYPE_SETTING_GSM);
	if (s_gsm) {
		if (username)
			*username = nm_setting_gsm_get_username (s_gsm);
		if (password)
			*password = nm_setting_gsm_get_password (s_gsm);
	} else {
		/* Try CDMA then */
		s_cdma = (NMSettingCdma *) nm_connection_get_setting (connection, NM_TYPE_SETTING_CDMA);
		if (s_cdma) {
			if (username)
				*username = nm_setting_cdma_get_username (s_cdma);
			if (password)
				*password = nm_setting_cdma_get_password (s_cdma);
		}
	}

	return (s_cdma || s_gsm) ? TRUE : FALSE;
}


static void
real_connection_secrets_updated (NMDevice *device,
                                 NMConnection *connection,
                                 GSList *updated_settings,
                                 RequestSecretsCaller caller)
{
	NMDeviceBt *self = NM_DEVICE_BT (device);
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (self);
	NMActRequest *req;
	const char *username = NULL, *password = NULL;
	gboolean success = FALSE;

	if (caller != SECRETS_CALLER_PPP)
		return;

	g_return_if_fail (priv->ppp_manager);

	req = nm_device_get_act_request (device);
	g_assert (req);

	success = get_ppp_credentials (nm_act_request_get_connection (req),
	                               &username,
	                               &password);
	if (success) {
		nm_ppp_manager_update_secrets (priv->ppp_manager,
		                               nm_device_get_ip_iface (device),
		                               username ? username : "",
		                               password ? password : "",
		                               NULL);
		return;
	}

	/* Shouldn't ever happen */
	nm_ppp_manager_update_secrets (priv->ppp_manager,
	                               nm_device_get_ip_iface (device),
	                               NULL,
	                               NULL,
	                               "missing GSM/CDMA setting; no secrets could be found.");
}

static NMActStageReturn
ppp_stage3_start (NMDevice *device, NMDeviceStateReason *reason)
{
	NMDeviceBt *self = NM_DEVICE_BT (device);
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (self);
	NMActRequest *req;
	const char *ppp_name = NULL;
	GError *err = NULL;
	NMActStageReturn ret;
	gboolean success;

	req = nm_device_get_act_request (device);
	g_assert (req);

	success = get_ppp_credentials (nm_act_request_get_connection (req),
	                               &ppp_name,
	                               NULL);
	if (!success) {
		// FIXME: set reason to something plausible
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	priv->ppp_manager = nm_ppp_manager_new (priv->rfcomm_iface);
	if (nm_ppp_manager_start (priv->ppp_manager, req, ppp_name, 20, &err)) {
		g_signal_connect (priv->ppp_manager, "state-changed",
						  G_CALLBACK (ppp_state_changed),
						  device);
		g_signal_connect (priv->ppp_manager, "ip4-config",
						  G_CALLBACK (ppp_ip4_config),
						  device);
		g_signal_connect (priv->ppp_manager, "stats",
						  G_CALLBACK (ppp_stats),
						  device);

		ret = NM_ACT_STAGE_RETURN_POSTPONE;
	} else {
		nm_warning ("%s", err->message);
		g_error_free (err);

		g_object_unref (priv->ppp_manager);
		priv->ppp_manager = NULL;

		*reason = NM_DEVICE_STATE_REASON_PPP_START_FAILED;
		ret = NM_ACT_STAGE_RETURN_FAILURE;
	}

	return ret;
}

static NMActStageReturn
ppp_stage4 (NMDevice *device, NMIP4Config **config, NMDeviceStateReason *reason)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	NMConnection *connection;
	NMSettingIP4Config *s_ip4;

	*config = priv->pending_ip4_config;
	priv->pending_ip4_config = NULL;

	/* Merge user-defined overrides into the IP4Config to be applied */
	connection = nm_act_request_get_connection (nm_device_get_act_request (device));
	g_assert (connection);
	s_ip4 = (NMSettingIP4Config *) nm_connection_get_setting (connection, NM_TYPE_SETTING_IP4_CONFIG);
	nm_utils_merge_ip4_config (*config, s_ip4);

	return NM_ACT_STAGE_RETURN_SUCCESS;
}

/*****************************************************************************/

static void
nm_device_bt_connect_cb (DBusGProxy       *proxy,
                         DBusGProxyCall   *call_id,
                         void             *user_data)
{
	NMDeviceBt *self = NM_DEVICE_BT (user_data);
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (self);
	GError *error = NULL;
	char *device;

	if (dbus_g_proxy_end_call (proxy, call_id, &error,
				   G_TYPE_STRING, &device,
				   G_TYPE_INVALID) == FALSE) {
		nm_warning ("Error connecting with bluez: %s",
		            error && error->message ? error->message : "(unknown)");
		g_clear_error (&error);

		// FIXME: get a better reason code
		nm_device_state_changed (NM_DEVICE (self), NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_NONE);
		return;
	}

	if (!device || !strlen (device)) {
		nm_warning ("Invalid network device returned by bluez");

		// FIXME: get a better reason code
		nm_device_state_changed (NM_DEVICE (self), NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_NONE);
	}

	if (priv->bt_type == NM_BT_CAPABILITY_DUN) {
		g_free (priv->rfcomm_iface);
		priv->rfcomm_iface = device;
	} else if (priv->bt_type == NM_BT_CAPABILITY_NAP) {
		nm_device_set_ip_iface (NM_DEVICE (self), device);
		g_free (device);
	}

	/* Stage 3 gets scheduled when Bluez says we're connected */
}

static void
bluez_property_changed (DBusGProxy *proxy,
                        const char *property,
                        GValue *value,
                        gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMDeviceBt *self = NM_DEVICE_BT (user_data);
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (self);
	gboolean connected;
	NMDeviceState state;

	if (strcmp (property, "Connected"))
		return;

	state = nm_device_get_state (device);
	connected = g_value_get_boolean (value);
	if (connected) {
		/* Bluez says we're connected now.  Start IP config. */

		if (state == NM_DEVICE_STATE_CONFIG) {
			gboolean pan = (priv->bt_type == NM_BT_CAPABILITY_NAP);
			gboolean dun = (priv->bt_type == NM_BT_CAPABILITY_DUN);

			nm_info ("Activation (%s/bluetooth) Stage 2 of 5 (Device Configure) "
			         "successful.  Connected via %s.",
			         nm_device_get_iface (device),
			         dun ? "DUN" : (pan ? "PAN" : "unknown"));

			nm_device_activate_schedule_stage3_ip_config_start (device);
		}
	} else {
		gboolean fail = FALSE;

		/* Bluez says we're disconnected from the device.  Suck. */

		if (nm_device_is_activating (device)) {
			nm_info ("Activation (%s/bluetooth): bluetooth link disconnected.",
			         nm_device_get_iface (device));
			fail = TRUE;
		} else if (state == NM_DEVICE_STATE_ACTIVATED) {
			nm_info ("%s: bluetooth link disconnected.", nm_device_get_iface (device));
			fail = TRUE;
		}

		if (fail)
			nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_CARRIER);
	}
}

static NMActStageReturn
real_act_stage2_config (NMDevice *device, NMDeviceStateReason *reason)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	NMActRequest *req;
	NMDBusManager *dbus_mgr;
	DBusGConnection *g_connection;

	req = nm_device_get_act_request (device);
	g_assert (req);

	priv->bt_type = get_connection_bt_type (nm_act_request_get_connection (req));
	if (priv->bt_type == NM_BT_CAPABILITY_NONE) {
		// FIXME: set a reason code
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	dbus_mgr = nm_dbus_manager_get ();
	g_connection = nm_dbus_manager_get_connection (dbus_mgr);
	g_object_unref (dbus_mgr);

	if (priv->bt_type == NM_BT_CAPABILITY_DUN) {
		priv->type_proxy = dbus_g_proxy_new_for_name (g_connection,
							      BLUEZ_SERVICE,
							      nm_device_get_udi (device),
							      BLUEZ_SERIAL_INTERFACE);
		if (!priv->type_proxy) {
			// FIXME: set a reason code
			return NM_ACT_STAGE_RETURN_FAILURE;
		}

		dbus_g_proxy_begin_call_with_timeout (priv->type_proxy, "Connect",
		                                      nm_device_bt_connect_cb,
		                                      device,
		                                      NULL,
		                                      20000,
		                                      G_TYPE_STRING, BLUETOOTH_DUN_UUID,
		                                      G_TYPE_INVALID);
	} else if (priv->bt_type == NM_BT_CAPABILITY_NAP) {
		priv->type_proxy = dbus_g_proxy_new_for_name (g_connection,
							      BLUEZ_SERVICE,
							      nm_device_get_udi (device),
							      BLUEZ_NETWORK_INTERFACE);
		if (!priv->type_proxy) {
			// FIXME: set a reason code
			return NM_ACT_STAGE_RETURN_FAILURE;
		}

		dbus_g_proxy_begin_call_with_timeout (priv->type_proxy, "Connect",
		                                      nm_device_bt_connect_cb,
		                                      device,
		                                      NULL,
		                                      20000,
		                                      G_TYPE_STRING, BLUETOOTH_NAP_UUID,
		                                      G_TYPE_INVALID);
	} else
		g_assert_not_reached ();

	/* Watch for BT device property changes */
	dbus_g_object_register_marshaller (_nm_marshal_VOID__STRING_BOXED,
	                                   G_TYPE_NONE,
	                                   G_TYPE_STRING, G_TYPE_VALUE,
	                                   G_TYPE_INVALID);
	dbus_g_proxy_add_signal (priv->type_proxy, "PropertyChanged",
	                         G_TYPE_STRING, G_TYPE_VALUE, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (priv->type_proxy, "PropertyChanged",
	                             G_CALLBACK (bluez_property_changed), device, NULL);

	return NM_ACT_STAGE_RETURN_POSTPONE;
}

static NMActStageReturn
real_act_stage3_ip4_config_start (NMDevice *device, NMDeviceStateReason *reason)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	NMActStageReturn ret;

	if (priv->bt_type == NM_BT_CAPABILITY_DUN)
		ret = ppp_stage3_start (device, reason);
	else
		ret = NM_DEVICE_CLASS (nm_device_bt_parent_class)->act_stage3_ip4_config_start (device, reason);

	return ret;
}

static NMActStageReturn
real_act_stage4_get_ip4_config (NMDevice *device,
                                NMIP4Config **config,
                                NMDeviceStateReason *reason)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);
	NMActStageReturn ret;

	if (priv->bt_type == NM_BT_CAPABILITY_DUN)
		ret = ppp_stage4 (device, config, reason);
	else
		ret = NM_DEVICE_CLASS (nm_device_bt_parent_class)->act_stage4_get_ip4_config (device, config, reason);

	return ret;
}

static void
real_deactivate_quickly (NMDevice *device)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (device);

	if (priv->pending_ip4_config) {
		g_object_unref (priv->pending_ip4_config);
		priv->pending_ip4_config = NULL;
	}

	priv->in_bytes = priv->out_bytes = 0;

	if (priv->bt_type == NM_BT_CAPABILITY_DUN) {
		if (priv->ppp_manager) {
			g_object_unref (priv->ppp_manager);
			priv->ppp_manager = NULL;
		}

		if (priv->type_proxy) {
			/* Don't ever pass NULL through dbus; rfcomm_iface
			 * might happen to be NULL for some reason.
			 */
			if (priv->rfcomm_iface) {
				dbus_g_proxy_call_no_reply (priv->type_proxy, "Disconnect",
				                            G_TYPE_STRING, priv->rfcomm_iface,
				                            G_TYPE_INVALID);
			}
			g_object_unref (priv->type_proxy);
			priv->type_proxy = NULL;
		}
	} else if (priv->bt_type == NM_BT_CAPABILITY_NAP) {
		if (priv->type_proxy) {
			dbus_g_proxy_call_no_reply (priv->type_proxy, "Disconnect",
			                            G_TYPE_INVALID);
			g_object_unref (priv->type_proxy);
			priv->type_proxy = NULL;
		}
	}

	priv->bt_type = NM_BT_CAPABILITY_NONE;

	g_free (priv->rfcomm_iface);
	priv->rfcomm_iface = NULL;

	if (NM_DEVICE_CLASS (nm_device_bt_parent_class)->deactivate_quickly)
		NM_DEVICE_CLASS (nm_device_bt_parent_class)->deactivate_quickly (device);
}

static void
nm_device_bt_init (NMDeviceBt *self)
{
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_HW_ADDRESS:
		/* Construct only */
		priv->bdaddr = g_ascii_strup (g_value_get_string (value), -1);
		break;
	case PROP_BT_NAME:
		/* Construct only */
		priv->name = g_value_dup_string (value);
		break;
	case PROP_BT_CAPABILITIES:
		/* Construct only */
		priv->capabilities = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_HW_ADDRESS:
		g_value_set_string (value, priv->bdaddr);
		break;
	case PROP_BT_NAME:
		g_value_set_string (value, priv->name);
		break;
	case PROP_BT_CAPABILITIES:
		g_value_set_uint (value, priv->capabilities);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
finalize (GObject *object)
{
	NMDeviceBtPrivate *priv = NM_DEVICE_BT_GET_PRIVATE (object);

	if (priv->type_proxy)
		g_object_unref (priv->type_proxy);

	g_free (priv->bdaddr);
	g_free (priv->name);

	G_OBJECT_CLASS (nm_device_bt_parent_class)->finalize (object);
}

static void
nm_device_bt_class_init (NMDeviceBtClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMDeviceClass *device_class = NM_DEVICE_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDeviceBtPrivate));

	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;

	device_class->get_best_auto_connection = real_get_best_auto_connection;
	device_class->get_generic_capabilities = real_get_generic_capabilities;
	device_class->connection_secrets_updated = real_connection_secrets_updated;
	device_class->deactivate_quickly = real_deactivate_quickly;
	device_class->act_stage2_config = real_act_stage2_config;
	device_class->act_stage3_ip4_config_start = real_act_stage3_ip4_config_start;
	device_class->act_stage4_get_ip4_config = real_act_stage4_get_ip4_config;
	device_class->check_connection_compatible = real_check_connection_compatible;

	/* Properties */
	g_object_class_install_property
		(object_class, PROP_HW_ADDRESS,
		 g_param_spec_string (NM_DEVICE_BT_HW_ADDRESS,
		                      "Bluetooth address",
		                      "Bluetooth address",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_BT_NAME,
		 g_param_spec_string (NM_DEVICE_BT_NAME,
		                      "Bluetooth device name",
		                      "Bluetooth device name",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_BT_CAPABILITIES,
		 g_param_spec_uint (NM_DEVICE_BT_CAPABILITIES,
		                    "Bluetooth device capabilities",
		                    "Bluetooth device capabilities",
		                    NM_BT_CAPABILITY_NONE, G_MAXUINT, NM_BT_CAPABILITY_NONE,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	/* Signals */
	signals[PPP_STATS] =
		g_signal_new ("ppp-stats",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (NMDeviceBtClass, ppp_stats),
		              NULL, NULL,
		              _nm_marshal_VOID__UINT_UINT,
		              G_TYPE_NONE, 2,
		              G_TYPE_UINT, G_TYPE_UINT);

	signals[PROPERTIES_CHANGED] = 
		nm_properties_changed_signal_new (object_class,
		                                  G_STRUCT_OFFSET (NMDeviceBtClass, properties_changed));

	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass),
	                                 &dbus_glib_nm_device_bt_object_info);

	dbus_g_error_domain_register (NM_BT_ERROR, NULL, NM_TYPE_BT_ERROR);
}
