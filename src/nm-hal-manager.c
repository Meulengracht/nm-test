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
 * Copyright (C) 2007 - 2008 Novell, Inc.
 * Copyright (C) 2007 - 2009 Red Hat, Inc.
 */

#include "config.h"

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <libhal.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#if HAVE_LIBUDEV
#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE
#include <libudev.h>
#endif /* HAVE_LIBUDEV */

#include "nm-glib-compat.h"
#include "nm-hal-manager.h"
#include "nm-marshal.h"
#include "nm-dbus-manager.h"
#include "nm-utils.h"
#include "nm-device-wifi.h"
#include "nm-device-ethernet.h"
#include "nm-gsm-device.h"
#include "nm-hso-gsm-device.h"
#include "nm-cdma-device.h"

/* Killswitch poll frequency in seconds */
#define RFKILL_POLL_FREQUENCY 6

#define HAL_DBUS_SERVICE "org.freedesktop.Hal"

typedef struct {
	GType device_type;
	char *capability_str;
	char *category;
	gboolean (*is_device_fn) (NMHalManager *self, const char *udi);
	NMDeviceCreatorFn creator_fn;
} DeviceCreator;

static void emit_udi_added (NMHalManager *self, const char *udi, DeviceCreator *creator);

typedef struct {
	LibHalContext *hal_ctx;
	NMDBusManager *dbus_mgr;
	GSList *device_creators;
	gboolean rfkilled;  /* Authoritative rfkill state */

	GSList *deferred_modems;
	guint deferred_modem_id;
	DeviceCreator *modem_creator;

	/* Killswitch handling */
	GSList *killswitch_list;
	guint32 killswitch_poll_id;
	char *kswitch_err;
	gboolean poll_rfkilled;
	guint32 pending_polls;
	GSList *poll_proxies;

	gboolean disposed;
} NMHalManagerPrivate;

#define NM_HAL_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_HAL_MANAGER, NMHalManagerPrivate))

G_DEFINE_TYPE (NMHalManager, nm_hal_manager, G_TYPE_OBJECT)

enum {
	UDI_ADDED,
	UDI_REMOVED,
	RFKILL_CHANGED,
	HAL_REAPPEARED,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


static gboolean poll_killswitches (gpointer user_data);

/* Device creators */

static DeviceCreator *
get_creator (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DeviceCreator *creator;
	GSList *iter;

	for (iter = priv->device_creators; iter; iter = g_slist_next (iter)) {
		creator = (DeviceCreator *) iter->data;

		if (libhal_device_query_capability (priv->hal_ctx, udi, creator->capability_str, NULL) && 
		    creator->is_device_fn (self, udi))
			return creator;
	}

	return NULL;
}

/* end of device creators */

/* Common helpers for built-in device creators */

static char *
nm_get_device_driver_name (LibHalContext *ctx, const char *origdev_udi)
{
	char *driver_name = NULL;

	if (origdev_udi && libhal_device_property_exists (ctx, origdev_udi, "info.linux.driver", NULL)) {
		char *drv;

		drv = libhal_device_get_property_string (ctx, origdev_udi, "info.linux.driver", NULL);
		if (drv) {
			driver_name = g_strdup (drv);
			libhal_free_string (drv);
		}
	}
	return driver_name;
}

/* Returns the parent if the device is a Sony Ericsson 'mbm'-style device */
static char *
is_mbm (LibHalContext *ctx, const char *udi)
{
	guint32 vendor_id = 0, product_id = 0;
	char *parent;

	parent = libhal_device_get_property_string (ctx, udi, "info.parent", NULL);
	if (!parent)
		return NULL;

	vendor_id = libhal_device_get_property_int (ctx, parent, "usb.vendor_id", NULL);
	product_id = libhal_device_get_property_int (ctx, parent, "usb.product_id", NULL);

	if (   (vendor_id == 0x0bdb && product_id == 0x1900)  /* SE F3507g */
	    || (vendor_id == 0x0bdb && product_id == 0x1902)  /* SE F3507g */
	    || (vendor_id == 0x0fce && product_id == 0xd0cf)  /* SE MD300 */
	    || (vendor_id == 0x413c && product_id == 0x8147)) /* Dell 5530 HSDPA */
		return parent;

	libhal_free_string (parent);
	return NULL;
}

/* Wired device creator */

static gboolean
is_wired_device (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char *category;
	gboolean is_wired = FALSE;

	if (libhal_device_property_exists (priv->hal_ctx, udi, "net.linux.ifindex", NULL) &&
		libhal_device_property_exists (priv->hal_ctx, udi, "info.category", NULL)) {

		category = libhal_device_get_property_string (priv->hal_ctx, udi, "info.category", NULL);
		if (category) {
			is_wired = strcmp (category, "net.80203") == 0;
			libhal_free_string (category);
		}
	}

	return is_wired;
}

static GObject *
wired_device_creator (NMHalManager *self,
                      const char *udi,
                      const char *origdev_udi,
                      gboolean managed)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GObject *device = NULL;
	char *iface, *driver, *parent;
	gboolean mbm = FALSE;

	iface = libhal_device_get_property_string (priv->hal_ctx, udi, "net.interface", NULL);
	if (!iface) {
		nm_warning ("Couldn't get interface for %s, ignoring.", udi);
		return NULL;
	}

	driver = nm_get_device_driver_name (priv->hal_ctx, origdev_udi);

	/* Special handling of Ericsson F3507g 'mbm' devices; ignore the
	 * cdc-ether device that it provides since we don't use it yet.
	 */
	if (driver && !strcmp (driver, "cdc_ether")) {
		parent = is_mbm (priv->hal_ctx, udi);
		mbm = !!parent;
		libhal_free_string (parent);
	}

	if (!mbm)
		device = (GObject *) nm_device_ethernet_new (udi, iface, driver, managed);

	libhal_free_string (iface);
	g_free (driver);
	return device;
}

/* Wireless device creator */

static gboolean
is_wireless_device (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char *category;
	gboolean is_wireless = FALSE;

	if (libhal_device_property_exists (priv->hal_ctx, udi, "net.linux.ifindex", NULL) &&
		libhal_device_property_exists (priv->hal_ctx, udi, "info.category", NULL)) {

		category = libhal_device_get_property_string (priv->hal_ctx, udi, "info.category", NULL);
		if (category) {
			is_wireless = strcmp (category, "net.80211") == 0;
			libhal_free_string (category);
		}
	}

	return is_wireless;
}

static GObject *
wireless_device_creator (NMHalManager *self,
                         const char *udi,
                         const char *origdev_udi,
                         gboolean managed)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GObject *device;
	char *iface;
	char *driver;

	iface = libhal_device_get_property_string (priv->hal_ctx, udi, "net.interface", NULL);
	if (!iface) {
		nm_warning ("Couldn't get interface for %s, ignoring.", udi);
		return NULL;
	}

	driver = nm_get_device_driver_name (priv->hal_ctx, origdev_udi);
	device = (GObject *) nm_device_wifi_new (udi, iface, driver, managed);

	libhal_free_string (iface);
	g_free (driver);

	return device;
}

/* Modem device creator */

static gboolean
is_modem_device (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	gboolean is_modem = FALSE;

	if (libhal_device_property_exists (priv->hal_ctx, udi, "info.category", NULL)) {
		char *category;

		category = libhal_device_get_property_string (priv->hal_ctx, udi, "info.category", NULL);
		if (category) {
			is_modem = strcmp (category, "serial") == 0;
			libhal_free_string (category);
		}
	}

	return is_modem;
}

static char *
get_hso_netdev (LibHalContext *ctx, const char *udi)
{
	char *serial_od = NULL, *serial_od_parent = NULL, *netdev = NULL, *bus;
	char **netdevs = NULL;
	int num, i;

	/* Get the serial interface's originating device UDI, used to find the
	 * originating device's netdev.
	 */
	serial_od = libhal_device_get_property_string (ctx, udi, "serial.originating_device", NULL);
	if (!serial_od)
		serial_od = libhal_device_get_property_string (ctx, udi, "serial.physical_device", NULL);
	if (!serial_od)
		goto out;

	serial_od_parent = libhal_device_get_property_string (ctx, serial_od, "info.parent", NULL);
	if (!serial_od_parent)
		goto out;

	/* Check to ensure we've got the actual "USB Device" */
	bus = libhal_device_get_property_string (ctx, serial_od_parent, "info.bus", NULL);
	if (!bus || strcmp (bus, "usb_device")) {
		libhal_free_string (bus);
		goto out;
	}
	libhal_free_string (bus);

	/* Look for the originating device's netdev */
	netdevs = libhal_find_device_by_capability (ctx, "net", &num, NULL);
	for (i = 0; netdevs && !netdev && (i < num); i++) {
		char *net_od = NULL, *net_od_parent = NULL, *tmp;

		net_od = libhal_device_get_property_string (ctx, netdevs[i], "net.originating_device", NULL);
		if (!net_od)
			net_od = libhal_device_get_property_string (ctx, netdevs[i], "net.physical_device", NULL);
		if (!net_od)
			goto next;

		net_od_parent = libhal_device_get_property_string (ctx, net_od, "info.parent", NULL);
		if (!net_od_parent)
			goto next;

		/* Check to ensure we've got the actual "USB Device" */
		bus = libhal_device_get_property_string (ctx, net_od_parent, "info.bus", NULL);
		if (!bus || strcmp (bus, "usb_device")) {
			libhal_free_string (bus);
			goto next;
		}
		libhal_free_string (bus);

		if (!strcmp (net_od_parent, serial_od_parent)) {
			/* We found it */
			tmp = libhal_device_get_property_string (ctx, netdevs[i], "net.interface", NULL);
			if (tmp)
				netdev = g_strdup (tmp);
			libhal_free_string (tmp);
		}

	next:
		libhal_free_string (net_od_parent);
		libhal_free_string (net_od);
	}

out:
	libhal_free_string_array (netdevs);
	libhal_free_string (serial_od);
	libhal_free_string (serial_od_parent);

	return netdev;
}

#define PROP_GSM   "ID_NM_MODEM_GSM"
#define PROP_CDMA  "ID_NM_MODEM_IS707_A"
#define PROP_EVDO1 "ID_NM_MODEM_IS856"
#define PROP_EVDOA "ID_NM_MODEM_IS856_A"

#if HAVE_LIBUDEV

typedef struct {
	gboolean gsm;
	gboolean cdma;
} UdevIterData;

#if UDEV_VERSION >= 129
static const char *
get_udev_property (struct udev_device *device, const char *name)
{
	struct udev_list_entry *entry;

	udev_list_entry_foreach (entry, udev_device_get_properties_list_entry (device)) {
		if (strcmp (udev_list_entry_get_name (entry), name) == 0)
			return udev_list_entry_get_value (entry);
	}

	return NULL;
}
#else
static int udev_device_prop_iter(struct udev_device *udev_device,
                                 const char *key,
                                 const char *value,
                                 void *data)
{
	UdevIterData *types = data;

	if (!strcmp (key, PROP_GSM) && !strcmp (value, "1"))
		types->gsm = TRUE;
	if (!strcmp (key, PROP_CDMA) && !strcmp (value, "1"))
		types->cdma = TRUE;
	if (!strcmp (key, PROP_EVDO1) && !strcmp (value, "1"))
		types->cdma = TRUE;
	if (!strcmp (key, PROP_EVDOA) && !strcmp (value, "1"))
		types->cdma = TRUE;

	/* Return 0 to continue looking */
	return types->gsm && types->cdma;
}
#endif

static gboolean
libudev_get_modem_capabilities (const char *sysfs_path,
                                gboolean *gsm,
                                gboolean *cdma)
{
	struct udev *udev;
	struct udev_device *device;

	g_return_val_if_fail (sysfs_path != NULL, FALSE);
	g_return_val_if_fail (gsm != NULL, FALSE);
	g_return_val_if_fail (*gsm == FALSE, FALSE);
	g_return_val_if_fail (cdma != NULL, FALSE);
	g_return_val_if_fail (*cdma == FALSE, FALSE);

	udev = udev_new ();
	if (!udev)
		return FALSE;

#if UDEV_VERSION >= 129
	device = udev_device_new_from_syspath (udev, sysfs_path);
#else
	/* udev_device_new_from_devpath() requires the sysfs mount point to be
	 * stripped off the path.
	 */
	if (!strncmp (sysfs_path, "/sys", 4))
		sysfs_path += 4;
	device = udev_device_new_from_devpath (udev, sysfs_path);
#endif
	if (!device) {
		udev_unref (udev);
		nm_warning ("couldn't inspect device '%s' with libudev", sysfs_path);
		return FALSE;
	}

#if UDEV_VERSION >= 129
	{
		const char *gsm_val = get_udev_property (device, PROP_GSM);
		const char *cdma_val = get_udev_property (device, PROP_CDMA);
		const char *evdo1_val = get_udev_property (device, PROP_EVDO1);
		const char *evdoa_val = get_udev_property (device, PROP_EVDOA);

		if (gsm_val && !strcmp (gsm_val, "1"))
			*gsm = TRUE;
		if (cdma_val && !strcmp (cdma_val, "1"))
			*cdma = TRUE;
		if (evdo1_val && !strcmp (evdo1_val, "1"))
			*cdma = TRUE;
		if (evdoa_val && !strcmp (evdoa_val, "1"))
			*cdma = TRUE;
	}
#else
	{
		UdevIterData iterdata = { FALSE, FALSE };

		udev_device_get_properties (device, udev_device_prop_iter, &iterdata);
		*gsm = iterdata.gsm;
		*cdma = iterdata.cdma;
	}
#endif

	udev_device_unref (device);
	udev_unref (udev);
	return TRUE;
}
#else
static gboolean
udevadm_get_modem_capabilities (const char *sysfs_path,
                                gboolean *gsm,
                                gboolean *cdma)
{
	char *udevadm_argv[] = { "/sbin/udevadm", "info", "--query=env", NULL, NULL };
	char *syspath_arg = NULL;
	char *udevadm_stdout = NULL;
	int exitcode;
	GError *error = NULL;
	char **lines = NULL, **iter;
	gboolean success = FALSE;

	g_return_val_if_fail (sysfs_path != NULL, FALSE);
	g_return_val_if_fail (gsm != NULL, FALSE);
	g_return_val_if_fail (*gsm == FALSE, FALSE);
	g_return_val_if_fail (cdma != NULL, FALSE);
	g_return_val_if_fail (*cdma == FALSE, FALSE);

	udevadm_argv[3] = syspath_arg = g_strdup_printf ("--path=%s", sysfs_path);
	if (g_spawn_sync ("/", udevadm_argv, NULL, 0, NULL, NULL,
			  &udevadm_stdout,
			  NULL,
			  &exitcode,
			  &error) != TRUE) {
		nm_warning ("could not run udevadm to get modem capabilities for '%s': %s",
		            sysfs_path,
		            (error && error->message) ? error->message : "(unknown)");
		g_clear_error (&error);
		goto error;
	}

	if (exitcode != 0) {
		nm_warning ("udevadm error while getting modem capabilities for '%s': %d",
		            sysfs_path, WEXITSTATUS (exitcode));
		goto error;
	}

	lines = g_strsplit_set (udevadm_stdout, "\n\r", -1);
	for (iter = lines; *iter; iter++) {
		if (!strcmp (*iter, PROP_GSM "=1")) {
			*gsm = TRUE;
			break;
		} else if (   !strcmp (*iter, PROP_CDMA "=1")
		           || !strcmp (*iter, PROP_EVDO1 "=1")
		           || !strcmp (*iter, PROP_EVDOA "=1")) {
			*cdma = TRUE;
			break;
		}
	}
	success = TRUE;

error:
	if (lines)
		g_strfreev (lines);
	g_free (udevadm_stdout);
	g_free (syspath_arg);
	return success;
}
#endif

static void
hal_get_modem_capabilities (LibHalContext *ctx,
                            const char *udi,
                            gboolean *gsm,
                            gboolean *cdma)
{
	char **capabilities, **iter;

	g_return_if_fail (ctx != NULL);
	g_return_if_fail (udi != NULL);
	g_return_if_fail (gsm != NULL);
	g_return_if_fail (*gsm == FALSE);
	g_return_if_fail (cdma != NULL);
	g_return_if_fail (*cdma == FALSE);

	/* Make sure it has the 'modem' capability first */
	if (!libhal_device_query_capability (ctx, udi, "modem", NULL))
		return;

	capabilities = libhal_device_get_property_strlist (ctx, udi, "modem.command_sets", NULL);
	/* 'capabilites' may be NULL */
	for (iter = capabilities; iter && *iter; iter++) {
		if (!strcmp (*iter, "GSM-07.07")) {
			*gsm = TRUE;
			break;
		}
		if (!strcmp (*iter, "IS-707-A")) {
			*cdma = TRUE;
			break;
		}
	}
	if (capabilities)
		g_strfreev (capabilities);
}

typedef struct {
	guint32 refcount;
	char *udi;
	char *origdev_udi;
	gboolean gsm;
	gboolean cdma;
	guint32 usb_interface_number;
	guint32 serial_port;
} DeferredModem;

#if 0
static void
deferred_modem_ref (DeferredModem *modem)
{
	g_return_if_fail (modem != NULL);
	g_return_if_fail (modem->refcount > 0);
	modem->refcount++;
}
#endif

static void
deferred_modem_unref (DeferredModem *modem)
{
	g_return_if_fail (modem != NULL);
	g_return_if_fail (modem->refcount > 0);

	modem->refcount--;
	if (modem->refcount == 0) {
		g_free (modem->udi);
		g_free (modem->origdev_udi);
		memset (modem, 0, sizeof (DeferredModem));
		g_free (modem);
	}
}

static DeferredModem *
deferred_modem_new (const char *udi,
                    const char *origdev_udi,
                    gboolean gsm,
                    gboolean cdma,
                    LibHalContext *ctx)
{
	DeferredModem *modem;
	char *parent;
	dbus_int32_t serial_port = -1, usb_interface = -1;
	DBusError error;

	parent = libhal_device_get_property_string (ctx, udi, "info.parent", NULL);
	if (!parent) {
		g_warning ("couldn't get parent for '%s'", udi);
		return NULL;
	}

	modem = g_new0 (DeferredModem, 1);
	modem->refcount = 1;
	modem->udi = g_strdup (udi);
	modem->origdev_udi = g_strdup (origdev_udi);
	modem->gsm = gsm;
	modem->cdma = cdma;

	dbus_error_init (&error);
	usb_interface = libhal_device_get_property_int (ctx, parent, "usb.interface.number", &error);
	if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
		usb_interface = -1;
	}

	dbus_error_init (&error);
	serial_port = libhal_device_get_property_int (ctx, udi, "serial.port", &error);
	if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
		serial_port = -1;
	}

	if (usb_interface == -1 && serial_port == -1) {
		g_warning ("couldn't get usb.interface.number and serial.port for '%s'", udi);
		deferred_modem_unref (modem);
		modem = NULL;
	} else {
		modem->usb_interface_number = usb_interface;
		modem->serial_port = serial_port;
	}

	libhal_free_string (parent);
	return modem;
}

static gint
deferred_modem_sort_func (const DeferredModem *a, const DeferredModem *b)
{
	gint ret;

	/* Sort is: origdev_udi, usb_interface, serial_port */

	ret = strcmp (a->origdev_udi, b->origdev_udi);
	if (ret)
		return ret;

	if (a->usb_interface_number < b->usb_interface_number)
		return -1;
	else if (a->usb_interface_number > b->usb_interface_number)
		return 1;

	if (a->serial_port < b->serial_port)
		return -1;
	else if (a->serial_port > b->serial_port)
		return 1;

	return 0;
}

static DeferredModem *
deferred_modem_find (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GSList *iter;

	for (iter = priv->deferred_modems; iter; iter = g_slist_next (iter)) {
		DeferredModem *candidate = iter->data;

		if (!strcmp (candidate->udi, udi))
			return candidate;
	}
	return NULL;
}

static DeferredModem *
deferred_modem_find_by_origdev (NMHalManager *self, const char *origdev_udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GSList *iter;

	for (iter = priv->deferred_modems; iter; iter = g_slist_next (iter)) {
		DeferredModem *candidate = iter->data;

		if (!strcmp (candidate->origdev_udi, origdev_udi))
			return candidate;
	}
	return NULL;
}

static void
deferred_modem_remove (NMHalManager *self, DeferredModem *modem)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GSList *iter, *new = NULL;

	g_return_if_fail (modem != NULL);

	for (iter = priv->deferred_modems; iter; iter = g_slist_next (iter)) {
		if (iter->data != modem)
			new = g_slist_insert_sorted (new, iter->data, (GCompareFunc) deferred_modem_sort_func);
	}
	g_slist_free (priv->deferred_modems);
	priv->deferred_modems = new;
}

static gboolean
deferred_modem_timeout (gpointer data)
{
	NMHalManager *self = data;
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	GSList *iter;

	nm_info ("Re-checking deferred serial ports");
	for (iter = priv->deferred_modems; iter; iter = g_slist_next (iter)) {
		DeferredModem *modem = iter->data;

		/* re-emit udi-added for each UDI that was deferred */
		emit_udi_added (self, modem->udi, priv->modem_creator);
	}

	g_slist_foreach (priv->deferred_modems, (GFunc) deferred_modem_unref, NULL);
	g_slist_free (priv->deferred_modems);
	priv->deferred_modems = NULL;
	priv->deferred_modem_id = 0;
	return FALSE;
}

static GObject *
new_modem_device (const char *udi,
                  gboolean gsm,
                  gboolean cdma,
                  const char *ttyname,
                  const char *driver,
                  gboolean managed,
                  LibHalContext *ctx)
{
	GObject *device = NULL;
	char *netdev = NULL;

	g_return_val_if_fail (udi != NULL, NULL);
	g_return_val_if_fail (gsm || cdma, NULL);
	g_return_val_if_fail (ttyname != NULL, NULL);
	g_return_val_if_fail (driver != NULL, NULL);
	g_return_val_if_fail (ctx != NULL, NULL);

	/* Special handling of 'hso' cards (until punted to ModemManager) */
	if (gsm && !strcmp (driver, "hso")) {
		char *hsotype_path;
		char *contents = NULL;
		gboolean success;

		/* We only want the "Control" interface, since that's the only
		 * one that gives us the unsolicited OWANCALL responses.  Ignore
		 * errors since we didn't care about them before.
		 */
		hsotype_path = g_strdup_printf ("/sys/class/tty/%s/hsotype", ttyname);
		success = g_file_get_contents (hsotype_path, &contents, NULL, NULL);
		g_free (hsotype_path);
		if (success && contents) {
			if (   !strstr (contents, "Control")
			    && !strstr (contents, "control")) {
				g_free (contents);
				return NULL;
			}
			g_free (contents);
		}

		netdev = get_hso_netdev (ctx, udi);
		if (!netdev) {
			nm_warning ("couldn't find HSO modem network device.");
			return NULL;
		}
	}

	/* Special handling of Option cards (until punted to ModemManager).  Only
	 * the first USB interface can be used for control and PPP.
	 */
	if (gsm && !strcmp (driver, "option")) {
		char *parent;
		guint32 vendor_id = 0, usb_interface = 0;

		parent = libhal_device_get_property_string (ctx, udi, "info.parent", NULL);
		if (!parent)
			return NULL;

		vendor_id = libhal_device_get_property_int (ctx, parent, "usb.vendor_id", NULL);
		if (vendor_id == 0x0AF0) {
			usb_interface = libhal_device_get_property_int (ctx, parent, "usb.interface.number", NULL);
			if (usb_interface > 0) {
				libhal_free_string (parent);
				return NULL;
			}
		}
		libhal_free_string (parent);
	}

	/* Special handling of Ericsson F3507g 'mbm' devices; already handled by
	 * ModemManager more correctly in HEAD.
	 */
	if (gsm && !strcmp (driver, "cdc_acm")) {
		char *parent;
		guint32 usb_interface;

		parent = is_mbm (ctx, udi);
		if (parent) {
			usb_interface = libhal_device_get_property_int (ctx, parent, "usb.interface.number", NULL);
			libhal_free_string (parent);
			if (usb_interface != 1)
				return NULL;
		}
	}

	if (gsm && netdev) {
		device = (GObject *) nm_hso_gsm_device_new (udi, ttyname, NULL, netdev, driver, managed);
		g_free (netdev);
	} else if (gsm)
		device = (GObject *) nm_gsm_device_new (udi, ttyname, NULL, driver, managed);
	else if (cdma)
		device = (GObject *) nm_cdma_device_new (udi, ttyname, NULL, driver, managed);
	else
		g_assert_not_reached ();

	return device;
}

static char *
nm_get_modem_device_driver_name (LibHalContext *ctx, const char *udi)
{
	char *driver_name = NULL;
	char *origdev_udi;

	origdev_udi = libhal_device_get_property_string (ctx, udi, "serial.originating_device", NULL);
	/* Older HAL uses "physical_device" */
	if (!origdev_udi)
		origdev_udi = libhal_device_get_property_string (ctx, udi, "serial.physical_device", NULL);

	if (origdev_udi && libhal_device_property_exists (ctx, origdev_udi, "info.linux.driver", NULL)) {
		char *drv;

		drv = libhal_device_get_property_string (ctx, origdev_udi, "info.linux.driver", NULL);
		if (drv) {
			driver_name = g_strdup (drv);
			libhal_free_string (drv);
		}
	}
	libhal_free_string (origdev_udi);
	return driver_name;
}

static GObject *
modem_device_creator (NMHalManager *self,
                      const char *udi,
                      const char *origdev_udi,
                      gboolean managed)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char *serial_device;
	const char *ttyname;
	char *sysfs_path;
	char *driver = NULL;
	GObject *device = NULL;
	gboolean udev_gsm = FALSE;
	gboolean udev_cdma = FALSE;
	gboolean hal_gsm = FALSE;
	gboolean hal_cdma = FALSE;
	gboolean later = FALSE;
	gboolean udev_success = FALSE;
	DeferredModem *deferred = NULL;

	serial_device = libhal_device_get_property_string (priv->hal_ctx, udi, "serial.device", NULL);
	/* For serial devices, 'origdev_udi' will be the actual USB or platform device,
	 * not HAL's 'serial.originating_device'.
	 */
	driver = nm_get_modem_device_driver_name (priv->hal_ctx, udi);
	if (!serial_device || !driver)
		goto out;

	ttyname = serial_device + strlen ("/dev/");

	/* If the UDI was deferred from earlier, just make the device with the
	 * cached capabilities.
	 */
	deferred = deferred_modem_find (self, udi);
	if (deferred) {
		udev_gsm = deferred->gsm;
		udev_cdma = deferred->cdma;
		device = new_modem_device (udi, deferred->gsm, deferred->cdma, ttyname, driver, managed, priv->hal_ctx);
		goto out;
	}

	/* Get udev probed capabilities */
	sysfs_path = libhal_device_get_property_string (priv->hal_ctx, udi, "linux.sysfs_path", NULL);
	if (!sysfs_path) {
		nm_warning ("(%s): could not determine sysfs path", ttyname);
		goto out;
	}

#if HAVE_LIBUDEV
	udev_success = libudev_get_modem_capabilities (sysfs_path, &udev_gsm, &udev_cdma);
#else
	udev_success = udevadm_get_modem_capabilities (sysfs_path, &udev_gsm, &udev_cdma);
#endif
	libhal_free_string (sysfs_path);

	/* Get HAL capabilities too */
	hal_get_modem_capabilities (priv->hal_ctx, udi, &hal_gsm, &hal_cdma);

	/* If for some reason running the udev prober failed fall back to HAL */
	if (!udev_success) {
		nm_warning ("(%s): udev probing failed; using only HAL modem capabilities.", ttyname);
		udev_gsm = hal_gsm;
		udev_cdma = hal_cdma;
	}

	/* If it's not known to either udev or HAL as a modem, nothing to do */
	if (!udev_gsm && !udev_cdma && !hal_gsm && !hal_cdma) {
		nm_info ("(%s): ignoring due to lack of mobile broadband capabilties", ttyname);
		goto out;
	}

	nm_info ("(%s): found serial port (udev:%s%s%s  hal:%s%s%s)",
	         ttyname,
	         udev_gsm ? "GSM" : "", udev_gsm && udev_cdma ? "/" : "", udev_cdma ? "CDMA" : "",
	         hal_gsm ? "GSM" : "", hal_gsm && hal_cdma ? "/" : "", hal_cdma ? "CDMA" : "");

	/* If other ports owned by this port's originating device are already
	 * deferred, defer this port as well.
	 */
	later = !!deferred_modem_find_by_origdev (self, origdev_udi);

	/* The logic goes like this:
	 *
	 * a) if both udev and HAL agree on the port command sets, use it
	 * b) if HAL thinks its a modem, but the command set disagrees with udev,
	 *      assume udev probing is correct
	 * c) if HAL thinks it's a modem but udev does not, don't use it
	 * d) if HAL doesn't think it's a modem but udev does, schedule a timer
	 *      to check back later after HAL is done sending ttys.
	 */
	if (udev_gsm == hal_gsm && udev_cdma == hal_cdma) {
		/* Case (a): HAL and udev agree. */
		if (!later)
			device = new_modem_device (udi, udev_gsm, udev_cdma, ttyname, driver, managed, priv->hal_ctx);
	} else if ((hal_gsm || hal_cdma) && (udev_gsm || udev_cdma)) {
		/* Case (b): HAL and udev think it's a modem, but they don't
		 * agree on the command set the modem supports.  Trust udev.
		 */
		if (!later)
			device = new_modem_device (udi, udev_gsm, udev_cdma, ttyname, driver, managed, priv->hal_ctx);
	} else if ((hal_gsm || hal_cdma) && !udev_gsm && !udev_cdma) {
		/* Case (c): HAL thinks it's a modem, but udev doesn't */
		nm_info ("(%s): ignoring due to lack of probed mobile broadband capabilties", ttyname);
		later = FALSE;
	} else {
		/* Case (d): HAL doesn't think its a modem, but udev does */
		later = TRUE;
	}

	if (!device && later) {
		deferred = deferred_modem_new (udi, origdev_udi, udev_gsm, udev_cdma, priv->hal_ctx);
		priv->deferred_modems = g_slist_insert_sorted (priv->deferred_modems,
		                                               deferred,
		                                               (GCompareFunc) deferred_modem_sort_func);

		if (priv->deferred_modem_id)
			g_source_remove (priv->deferred_modem_id);
		priv->deferred_modem_id = g_timeout_add_seconds (4, deferred_modem_timeout, self);

		nm_info ("(%s): deferring until all ports found", ttyname);
	}

out:
	libhal_free_string (serial_device);
	g_free (driver);
	return device;
}

static void
register_built_in_creators (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DeviceCreator *creator;

	/* Wired device */
	creator = g_slice_new0 (DeviceCreator);
	creator->device_type = NM_TYPE_DEVICE_ETHERNET;
	creator->capability_str = g_strdup ("net.80203");
	creator->category = g_strdup ("net");
	creator->is_device_fn = is_wired_device;
	creator->creator_fn = wired_device_creator;
	priv->device_creators = g_slist_append (priv->device_creators, creator);

	/* Wireless device */
	creator = g_slice_new0 (DeviceCreator);
	creator->device_type = NM_TYPE_DEVICE_WIFI;
	creator->capability_str = g_strdup ("net.80211");
	creator->category = g_strdup ("net");
	creator->is_device_fn = is_wireless_device;
	creator->creator_fn = wireless_device_creator;
	priv->device_creators = g_slist_append (priv->device_creators, creator);

	/* Modem */
	creator = g_slice_new0 (DeviceCreator);
	creator->device_type = NM_TYPE_SERIAL_DEVICE;
	creator->capability_str = g_strdup ("serial");
	creator->category = g_strdup ("serial");
	creator->is_device_fn = is_modem_device;
	creator->creator_fn = modem_device_creator;
	priv->device_creators = g_slist_append (priv->device_creators, creator);
	priv->modem_creator = creator;
}

static void
emit_udi_added (NMHalManager *self, const char *udi, DeviceCreator *creator)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char *od = NULL, *tmp, *parent, *bus = NULL;

	g_return_if_fail (self != NULL);
	g_return_if_fail (udi != NULL);
	g_return_if_fail (creator != NULL);

	/* For USB serial devices, originating device should really be the "USB Device"
	 * object, which is the grandparent of the tty device.  Only this grandparent
	 * is really common among all ttys of the device; HAL's "serial.originating_device"
	 * points to the "USB Interface" parent of the tty, but this interface only
	 * sometimes has multiple child ttys.  More commonly, the "USB Device"
	 * object provides several "USB Interface" objects, which each provide one tty.
	 * Thus, to ensure that NM only uses the correct tty for the deivce,
	 * we need to filter on the "USB Device" instead of the "USB Interface".
	 */
	parent = libhal_device_get_property_string (priv->hal_ctx, udi, "info.parent", NULL);
	if (parent)
		bus = libhal_device_get_property_string (priv->hal_ctx, parent, "info.bus", NULL);
	if (bus && !strcmp (bus, "usb")) {
		char *usb_intf_udi;

		usb_intf_udi = libhal_device_get_property_string (priv->hal_ctx, udi, "info.parent", NULL);
		if (usb_intf_udi) {
			od = libhal_device_get_property_string (priv->hal_ctx, usb_intf_udi, "info.parent", NULL);

			/* Ensure the grandparent really is the "USB Device" */
			if (od) {
				tmp = libhal_device_get_property_string (priv->hal_ctx, od, "info.bus", NULL);
				if (!tmp || strcmp (tmp, "usb_device")) {
					libhal_free_string (od);
					od = NULL;
				}
				if (tmp)
					libhal_free_string (tmp);
			}
			libhal_free_string (usb_intf_udi);
		}
	}
	libhal_free_string (bus);
	libhal_free_string (parent);

	/* For non-USB devices, and ss a fallback, just use the originating device
	 * of the tty; though this might result in more than one modem being detected by NM.
	 */
	if (!od) {
		tmp = g_strdup_printf ("%s.originating_device", creator->category);
		od = libhal_device_get_property_string (priv->hal_ctx, udi, tmp, NULL);
		g_free (tmp);
	}

	if (!od) {
		/* Older HAL uses 'physical_device' */
		tmp = g_strdup_printf ("%s.physical_device", creator->category);
		od = libhal_device_get_property_string (priv->hal_ctx, udi, tmp, NULL);
		g_free (tmp);
	}

	g_signal_emit (self, signals[UDI_ADDED], 0,
	               udi,
	               od,
	               GSIZE_TO_POINTER (creator->device_type),
	               creator->creator_fn);

	libhal_free_string (od);
}

static void
device_added (LibHalContext *ctx, const char *udi)
{
	NMHalManager *self = NM_HAL_MANAGER (libhal_ctx_get_user_data (ctx));
	DeviceCreator *creator;

	/* If not all the device's properties are set up yet (like net.interface),
	 * the device will actually get added later when HAL signals new device
	 * capabilties.
	 */
	creator = get_creator (self, udi);
	if (creator)
		emit_udi_added (self, udi, creator);
}

static void
device_removed (LibHalContext *ctx, const char *udi)
{
	NMHalManager *self = NM_HAL_MANAGER (libhal_ctx_get_user_data (ctx));
	DeferredModem *modem;

	modem = deferred_modem_find (self, udi);
	if (modem) {
		deferred_modem_remove (self, modem);
		deferred_modem_unref (modem);
	}

	g_signal_emit (self, signals[UDI_REMOVED], 0, udi);
}

static void
device_new_capability (LibHalContext *ctx, const char *udi, const char *capability)
{
	NMHalManager *self = NM_HAL_MANAGER (libhal_ctx_get_user_data (ctx));
	DeviceCreator *creator;

	creator = get_creator (self, udi);
	if (creator)
		emit_udi_added (self, udi, creator);
}

static void
add_initial_devices (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DeviceCreator *creator;
	GSList *iter;
	char **devices;
	int num_devices;
	int i;
	DBusError err;

	for (iter = priv->device_creators; iter; iter = g_slist_next (iter)) {
		creator = (DeviceCreator *) iter->data;

		dbus_error_init (&err);
		devices = libhal_find_device_by_capability (priv->hal_ctx,
		                                            creator->capability_str,
		                                            &num_devices,
		                                            &err);
		if (dbus_error_is_set (&err)) {
			nm_warning ("could not find existing devices: %s", err.message);
			dbus_error_free (&err);
			continue;
		}
		if (!devices)
			continue;

		for (i = 0; i < num_devices; i++) {
			if (creator->is_device_fn (self, devices[i]))
				emit_udi_added (self, devices[i], creator);
		}

		libhal_free_string_array (devices);
	}
}

static void
killswitch_poll_cleanup (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	if (priv->poll_proxies) {
		g_slist_foreach (priv->poll_proxies, (GFunc) g_object_unref, NULL);
		g_slist_free (priv->poll_proxies);
		priv->poll_proxies = NULL;
	}

	priv->pending_polls = 0;
	priv->poll_rfkilled = FALSE;
}

static void
killswitch_getpower_done (gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	priv->pending_polls--;

	if (priv->pending_polls > 0)
		return;

	if (priv->poll_rfkilled != priv->rfkilled) {
		priv->rfkilled = priv->poll_rfkilled;
		g_signal_emit (self, signals[RFKILL_CHANGED], 0, priv->rfkilled);
	}

	killswitch_poll_cleanup (self);

	/* Schedule next poll */
	priv->killswitch_poll_id = g_timeout_add_seconds (RFKILL_POLL_FREQUENCY,
	                                          poll_killswitches,
	                                          self);
}

static void 
killswitch_getpower_reply (DBusGProxy *proxy,
					  DBusGProxyCall *call_id,
					  gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	int power = 1;
	GError *err = NULL;

	if (dbus_g_proxy_end_call (proxy, call_id, &err,
	                           G_TYPE_INT, &power,
	                           G_TYPE_INVALID)) {
		if (power == 0)
			priv->poll_rfkilled = TRUE;
	} else {
		if (err->message) {
			/* Only print the error if we haven't seen it before */
		    if (!priv->kswitch_err || strcmp (priv->kswitch_err, err->message) != 0) {
				nm_warning ("Error getting killswitch power: %s.", err->message);
				g_free (priv->kswitch_err);
				priv->kswitch_err = g_strdup (err->message);

				/* If there was an error talking to HAL, treat that as rfkilled.
				 * See rh #448889.  On some Dell laptops, dellWirelessCtl
				 * may not be present, but HAL still advertises a killswitch,
				 * and calls to GetPower() will fail.  Thus we cannot assume
				 * that a failure of GetPower() automatically means the wireless
				 * is rfkilled, because in this situation NM would never bring
				 * the radio up.  Only assume failures between NM and HAL should
				 * block the radio, not failures of the HAL killswitch callout
				 * itself.
				 */
				if (strstr (err->message, "Did not receive a reply")) {
					nm_warning ("HAL did not reply to killswitch power request;"
					            " assuming radio is blocked.");
					priv->poll_rfkilled = TRUE;
				}
			}
		}
		g_error_free (err);
	}
}

static void
poll_one_killswitch (gpointer data, gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DBusGProxy *proxy;

	proxy = dbus_g_proxy_new_for_name (nm_dbus_manager_get_connection (priv->dbus_mgr),
								"org.freedesktop.Hal",
								(char *) data,
								"org.freedesktop.Hal.Device.KillSwitch");

	dbus_g_proxy_begin_call (proxy, "GetPower",
						killswitch_getpower_reply,
						self,
						killswitch_getpower_done,
						G_TYPE_INVALID);
	priv->pending_polls++;
	priv->poll_proxies = g_slist_prepend (priv->poll_proxies, proxy);
}

static gboolean
poll_killswitches (gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	killswitch_poll_cleanup (self);

	g_slist_foreach (priv->killswitch_list, poll_one_killswitch, self);
	return FALSE;
}

static void
add_killswitch_device (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char *type;
	GSList *iter;

	type = libhal_device_get_property_string (priv->hal_ctx, udi, "killswitch.type", NULL);
	if (!type)
		return;

	if (strcmp (type, "wlan"))
		goto out;

	/* see if it's already in the list */
	for (iter = priv->killswitch_list; iter; iter = iter->next) {
		const char *list_udi = (const char *) iter->data;
		if (!strcmp (list_udi, udi))
			goto out;
	}

	/* Poll switches if this is the first switch we've found */
	if (!priv->killswitch_list)
		priv->killswitch_poll_id = g_idle_add (poll_killswitches, self);

	priv->killswitch_list = g_slist_append (priv->killswitch_list, g_strdup (udi));
	nm_info ("Found radio killswitch %s", udi);

out:
	libhal_free_string (type);
}

static void
add_killswitch_devices (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	char **udis;
	int num_udis;
	int i;
	DBusError	err;

	dbus_error_init (&err);
	udis = libhal_find_device_by_capability (priv->hal_ctx, "killswitch", &num_udis, &err);
	if (!udis)
		return;

	if (dbus_error_is_set (&err)) {
		nm_warning ("Could not find killswitch devices: %s", err.message);
		dbus_error_free (&err);
		return;
	}

	for (i = 0; i < num_udis; i++)
		add_killswitch_device (self, udis[i]);

	libhal_free_string_array (udis);
}

static gboolean
hal_init (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DBusError error;
	DBusGConnection *connection; 

	priv->hal_ctx = libhal_ctx_new ();
	if (!priv->hal_ctx) {
		nm_warning ("Could not get connection to the HAL service.");
		return FALSE;
	}

	connection = nm_dbus_manager_get_connection (priv->dbus_mgr);
	libhal_ctx_set_dbus_connection (priv->hal_ctx,
									dbus_g_connection_get_connection (connection));

	dbus_error_init (&error);
	if (!libhal_ctx_init (priv->hal_ctx, &error)) {
		nm_warning ("libhal_ctx_init() failed: %s\n"
				    "Make sure the hal daemon is running?", 
				    error.message);
		goto error;
	}

	libhal_ctx_set_user_data (priv->hal_ctx, self);
	libhal_ctx_set_device_added (priv->hal_ctx, device_added);
	libhal_ctx_set_device_removed (priv->hal_ctx, device_removed);
	libhal_ctx_set_device_new_capability (priv->hal_ctx, device_new_capability);

	libhal_device_property_watch_all (priv->hal_ctx, &error);
	if (dbus_error_is_set (&error)) {
		nm_error ("libhal_device_property_watch_all(): %s", error.message);
		libhal_ctx_shutdown (priv->hal_ctx, NULL);
		goto error;
	}

	return TRUE;

error:
	if (dbus_error_is_set (&error))
		dbus_error_free (&error);
	if (priv->hal_ctx) {
		libhal_ctx_free (priv->hal_ctx);
		priv->hal_ctx = NULL;
	}
	return FALSE;
}

static void
hal_deinit (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);
	DBusError error;

	if (priv->killswitch_poll_id) {
		g_source_remove (priv->killswitch_poll_id);
		priv->killswitch_poll_id = 0;
	}
	killswitch_poll_cleanup (self);

	if (priv->killswitch_list) {
		g_slist_foreach (priv->killswitch_list, (GFunc) g_free, NULL);
		g_slist_free (priv->killswitch_list);
		priv->killswitch_list = NULL;
	}

	if (!priv->hal_ctx)
		return;

	dbus_error_init (&error);
	libhal_ctx_shutdown (priv->hal_ctx, &error);
	if (dbus_error_is_set (&error)) {
		nm_warning ("libhal shutdown failed - %s", error.message);
		dbus_error_free (&error);
	}

	libhal_ctx_free (priv->hal_ctx);
	priv->hal_ctx = NULL;
}

static void
name_owner_changed (NMDBusManager *dbus_mgr,
					const char *name,
					const char *old,
					const char *new,
					gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);
	gboolean old_owner_good = (old && (strlen (old) > 0));
	gboolean new_owner_good = (new && (strlen (new) > 0));

	/* Only care about signals from HAL */
	if (strcmp (name, HAL_DBUS_SERVICE))
		return;

	if (!old_owner_good && new_owner_good) {
		nm_info ("HAL re-appeared");
		/* HAL just appeared */
		if (!hal_init (self))
			nm_warning ("Could not re-connect to HAL!!");
		else
			g_signal_emit (self, signals[HAL_REAPPEARED], 0);
	} else if (old_owner_good && !new_owner_good) {
		/* HAL went away. Bad HAL. */
		nm_info ("HAL disappeared");
		hal_deinit (self);
	}
}

static void
connection_changed (NMDBusManager *dbus_mgr,
					DBusGConnection *connection,
					gpointer user_data)
{
	NMHalManager *self = NM_HAL_MANAGER (user_data);

	if (!connection) {
		hal_deinit (self);
		return;
	}

	if (nm_dbus_manager_name_has_owner (dbus_mgr, HAL_DBUS_SERVICE)) {
		if (!hal_init (self))
			nm_warning ("Could not re-connect to HAL!!");
	}
}

void
nm_hal_manager_query_devices (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	/* Find hardware we care about */
	if (priv->hal_ctx) {
		add_killswitch_devices (self);
		add_initial_devices (self);
	}
}

gboolean
nm_hal_manager_udi_exists (NMHalManager *self, const char *udi)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	return libhal_device_property_exists (priv->hal_ctx, udi, "info.udi", NULL);
}

NMHalManager *
nm_hal_manager_new (void)
{
	NMHalManager *self;
	NMHalManagerPrivate *priv;

	self = NM_HAL_MANAGER (g_object_new (NM_TYPE_HAL_MANAGER, NULL));

	priv = NM_HAL_MANAGER_GET_PRIVATE (self);
 	if (!nm_dbus_manager_name_has_owner (priv->dbus_mgr, HAL_DBUS_SERVICE)) {
		nm_info ("Waiting for HAL to start...");
		return self;
	}

	if (!hal_init (self)) {
		g_object_unref (self);
		self = NULL;
	}

	return self;
}

static void
nm_hal_manager_init (NMHalManager *self)
{
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	priv->rfkilled = FALSE;

	priv->dbus_mgr = nm_dbus_manager_get ();

	register_built_in_creators (self);

	g_signal_connect (priv->dbus_mgr,
	                  "name-owner-changed",
	                  G_CALLBACK (name_owner_changed),
	                  self);
	g_signal_connect (priv->dbus_mgr,
	                  "dbus-connection-changed",
	                  G_CALLBACK (connection_changed),
	                  self);
}

static void
destroy_creator (gpointer data, gpointer user_data)
{
	DeviceCreator *creator = (DeviceCreator *) data;

	g_free (creator->capability_str);
	g_free (creator->category);
	g_slice_free (DeviceCreator, data);
}

static void
dispose (GObject *object)
{
	NMHalManager *self = NM_HAL_MANAGER (object);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	if (priv->disposed) {
		G_OBJECT_CLASS (nm_hal_manager_parent_class)->dispose (object);
		return;
	}
	priv->disposed = TRUE;

	g_object_unref (priv->dbus_mgr);

	g_slist_foreach (priv->device_creators, destroy_creator, NULL);
	g_slist_free (priv->device_creators);

	g_slist_foreach (priv->deferred_modems, (GFunc) deferred_modem_unref, NULL);
	g_slist_free (priv->deferred_modems);

	if (priv->deferred_modem_id)
		g_source_remove (priv->deferred_modem_id);

	hal_deinit (self);

	G_OBJECT_CLASS (nm_hal_manager_parent_class)->dispose (object);	
}

static void
finalize (GObject *object)
{
	NMHalManager *self = NM_HAL_MANAGER (object);
	NMHalManagerPrivate *priv = NM_HAL_MANAGER_GET_PRIVATE (self);

	g_free (priv->kswitch_err);

	G_OBJECT_CLASS (nm_hal_manager_parent_class)->finalize (object);
}

static void
nm_hal_manager_class_init (NMHalManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMHalManagerPrivate));

	/* virtual methods */
	object_class->dispose = dispose;
	object_class->finalize = finalize;

	/* Signals */
	signals[UDI_ADDED] =
		g_signal_new ("udi-added",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMHalManagerClass, udi_added),
					  NULL, NULL,
					  _nm_marshal_VOID__STRING_STRING_POINTER_POINTER,
					  G_TYPE_NONE, 4,
					  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER);

	signals[UDI_REMOVED] =
		g_signal_new ("udi-removed",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMHalManagerClass, udi_removed),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__STRING,
					  G_TYPE_NONE, 1,
					  G_TYPE_STRING);

	signals[RFKILL_CHANGED] =
		g_signal_new ("rfkill-changed",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMHalManagerClass, rfkill_changed),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__BOOLEAN,
					  G_TYPE_NONE, 1,
					  G_TYPE_BOOLEAN);

	signals[HAL_REAPPEARED] =
		g_signal_new ("hal-reappeared",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMHalManagerClass, hal_reappeared),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__VOID,
					  G_TYPE_NONE, 0);
}

