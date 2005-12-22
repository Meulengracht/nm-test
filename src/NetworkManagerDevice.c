/* NetworkManager -- Network link manager
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

#include <errno.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <libhal.h>
#include <iwlib.h>
#include <signal.h>
#include <string.h>
#include <netinet/ether.h>

#include "autoip.h"
#include "NetworkManager.h"
#include "NetworkManagerMain.h"
#include "NetworkManagerDevice.h"
#include "NetworkManagerDevicePrivate.h"
#include "NetworkManagerUtils.h"
#include "NetworkManagerDbus.h"
#include "NetworkManagerWireless.h"
#include "NetworkManagerPolicy.h"
#include "NetworkManagerAPList.h"
#include "NetworkManagerSystem.h"
#include "nm-ip4-config.h"
#include "nm-vpn-manager.h"
#include "nm-dhcp-manager.h"
#include "nm-activation-request.h"
#include "nm-utils.h"

/* Local static prototypes */
static gpointer nm_device_worker (gpointer user_data);
static gboolean nm_device_wireless_scan (gpointer user_data);
static gboolean supports_mii_carrier_detect (NMDevice *dev);
static gboolean supports_ethtool_carrier_detect (NMDevice *dev);
static gboolean nm_device_bring_up_wait (NMDevice *dev, gboolean cancelable);
static gboolean link_to_specific_ap (NMDevice *dev, NMAccessPoint *ap, gboolean default_link);
static guint32 nm_device_discover_capabilities (NMDevice *dev);
static gboolean nm_is_driver_supported (NMDevice *dev);
static guint32 nm_device_wireless_discover_capabilities (NMDevice *dev);

static guint8 * get_scan_results(NMDevice *dev, NMSock *sk, guint32 *data_len);
static gboolean process_scan_results (NMDevice *dev, const guint8 *res_buf, guint32 res_buf_len);


static void nm_device_activate_schedule_stage1_device_prepare (NMActRequest *req);
static void nm_device_activate_schedule_stage2_device_config (NMActRequest *req);
static void nm_device_activate_schedule_stage3_ip_config_start (NMActRequest *req);
static void nm_device_activate_schedule_stage5_ip_config_commit (NMActRequest *req);

typedef struct
{
	NMDevice *	dev;
	guint8 *		results;
	guint32		results_len;
} NMWirelessScanResults;

typedef struct
{
	NMDevice *	dev;
	gboolean		force;
} NMWirelessScanCB;

/******************************************************/


/******************************************************/

/*
 * nm_device_test_wireless_extensions
 *
 * Test whether a given device is a wireless one or not.
 *
 */
static gboolean nm_device_test_wireless_extensions (NMDevice *dev)
{
	int		 err = -1;
	char		 ioctl_buf[64];
	NMSock	*sk;

	g_return_val_if_fail (dev != NULL, FALSE);

	/* We obviously cannot probe test devices (since they don't
	 * actually exist in hardware).
	 */
	if (dev->test_device)
		return (FALSE);

	ioctl_buf[63] = 0;
	strncpy (ioctl_buf, nm_device_get_iface(dev), 63);

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
#ifdef IOCTL_DEBUG
		nm_info ("%s: About to GET IWNAME\n", nm_device_get_iface (dev));
#endif
		err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIWNAME, ioctl_buf);
#ifdef IOCTL_DEBUG
		nm_info ("%s: Done with GET IWNAME\n", nm_device_get_iface (dev));
#endif
		nm_dev_sock_close (sk);
	}
	return (err == 0);
}


/*
 * nm_get_device_driver_name
 *
 * Get the device's driver name from HAL.
 *
 */
static char *nm_get_device_driver_name (NMDevice *dev)
{
	char	*		udi = NULL;
	char	*		driver_name = NULL;
	LibHalContext *ctx = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (dev->app_data != NULL, NULL);

	ctx = dev->app_data->hal_ctx;
	g_return_val_if_fail (ctx != NULL, NULL);

	if ((udi = nm_device_get_udi (dev)))
	{
		char *physdev_udi = libhal_device_get_property_string (ctx, udi, "net.physical_device", NULL);

		if (physdev_udi && libhal_device_property_exists (ctx, physdev_udi, "info.linux.driver", NULL))
		{
			char *drv = libhal_device_get_property_string (ctx, physdev_udi, "info.linux.driver", NULL);
			driver_name = g_strdup (drv);
			g_free (drv);
		}
		g_free (physdev_udi);
	}

	return driver_name;
}

/* Blacklist of unsupported drivers */
static char * driver_blacklist[] =
{
	NULL
};


/*
 * nm_is_driver_supported
 *
 * Check device's driver against a blacklist of unsupported drivers.
 *
 */
static gboolean nm_is_driver_supported (NMDevice *dev)
{
	char ** drv = NULL;
	gboolean supported = TRUE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->driver != NULL, FALSE);

	for (drv = &driver_blacklist[0]; *drv; drv++)
	{
		if (!strcmp (*drv, dev->driver))
		{
			supported = FALSE;
			break;
		}
	}

	/* Check for Wireless Extensions support >= 16 for wireless devices */
	if (supported && nm_device_is_wireless (dev))
	{
		NMSock *	sk = NULL;
		guint8	we_ver = 0;

		if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
		{
			iwrange	range;
			if (iw_get_range_info (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &range) >= 0)
				we_ver = range.we_version_compiled;
			nm_dev_sock_close (sk);
		}

		if (we_ver < 16)
		{
			nm_warning ("%s: driver's Wireless Extensions version (%d) is too old.  Can't use device.",
				nm_device_get_iface (dev), we_ver);
			supported = FALSE;
		}
	}

	return supported;
}


/*
 * nm_get_device_by_udi
 *
 * Search through the device list for a device with a given UDI.
 *
 * NOTE: the caller MUST hold the device list mutex already to make
 * this routine thread-safe.
 *
 */
NMDevice *nm_get_device_by_udi (NMData *data, const char *udi)
{
	NMDevice	*dev = NULL;
	GSList	*elt;
	
	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (udi  != NULL, NULL);

	for (elt = data->dev_list; elt; elt = g_slist_next (elt))
	{
		if ((dev = (NMDevice *)(elt->data)))
		{
			if (nm_null_safe_strcmp (nm_device_get_udi (dev), udi) == 0)
				return dev;
		}
	}

	return NULL;
}


/*
 * nm_get_device_by_iface
 *
 * Search through the device list for a device with a given iface.
 *
 * NOTE: the caller MUST hold the device list mutex already to make
 * this routine thread-safe.
 *
 */
NMDevice *nm_get_device_by_iface (NMData *data, const char *iface)
{
	NMDevice	*iter_dev = NULL;
	NMDevice	*found_dev = NULL;
	GSList	*elt;
	
	g_return_val_if_fail (data  != NULL, NULL);
	g_return_val_if_fail (iface != NULL, NULL);

	for (elt = data->dev_list; elt; elt = g_slist_next (elt))
	{
		iter_dev = (NMDevice *)(elt->data);
		if (iter_dev)
		{
			if (nm_null_safe_strcmp (nm_device_get_iface (iter_dev), iface) == 0)
			{
				found_dev = iter_dev;
				break;
			}
		}
	}

	return (found_dev);
}


/*****************************************************************************/
/* NMDevice object routines                                                  */
/*****************************************************************************/


/*
 * nm_device_copy_allowed_to_dev_list
 *
 * For devices that don't support wireless scanning, copy
 * the allowed AP list to the device's ap list.
 *
 */
void nm_device_copy_allowed_to_dev_list (NMDevice *dev, NMAccessPointList *allowed_list)
{
	NMAPListIter		*iter;
	NMAccessPoint		*src_ap;
	NMAccessPointList	*dev_list;

	g_return_if_fail (dev != NULL);

	if (allowed_list == NULL)
		return;

	nm_device_ap_list_clear (dev);
	dev->options.wireless.ap_list = nm_ap_list_new (NETWORK_TYPE_ALLOWED);

	if (!(iter = nm_ap_list_iter_new (allowed_list)))
		return;

	dev_list = nm_device_ap_list_get (dev);
	while ((src_ap = nm_ap_list_iter_next (iter)))
	{
		NMAccessPoint	*dst_ap = nm_ap_new_from_ap (src_ap);

		/* Assume that if the allowed list AP has a saved encryption
		 * key that the AP is encrypted.
		 */
		if (    (nm_ap_get_auth_method (src_ap) == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
			|| (nm_ap_get_auth_method (src_ap) == NM_DEVICE_AUTH_METHOD_SHARED_KEY))
			nm_ap_set_encrypted (dst_ap, TRUE);

		nm_ap_list_append_ap (dev_list, dst_ap);
		nm_ap_unref (dst_ap);
	}
	nm_ap_list_iter_free (iter);
}


/*
 * nm_device_wireless_init
 *
 * Initialize a new wireless device with wireless-specific settings.
 *
 */
static gboolean nm_device_wireless_init (NMDevice *dev)
{
	NMSock				*sk;
	NMDeviceWirelessOptions	*opts = &(dev->options.wireless);

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);

	opts->scan_mutex = g_mutex_new ();
	opts->ap_list = nm_ap_list_new (NETWORK_TYPE_DEVICE);
	if (!opts->scan_mutex || !opts->ap_list)
		return FALSE;

	nm_register_mutex_desc (opts->scan_mutex, "Scan Mutex");
	nm_wireless_set_scan_interval (dev->app_data, dev, NM_WIRELESS_SCAN_INTERVAL_ACTIVE);

	nm_device_set_mode (dev, NETWORK_MODE_INFRA);

	/* Non-scanning devices show the entire allowed AP list as their
	 * available networks.
	 */
	if (!(dev->capabilities & NM_DEVICE_CAP_WIRELESS_SCAN))
		nm_device_copy_allowed_to_dev_list (dev, dev->app_data->allowed_ap_list);

	opts->we_version = 0;
	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		iwrange	range;
		if (iw_get_range_info (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &range) >= 0)
		{
			int i;

			opts->max_qual.qual = range.max_qual.qual;
			opts->max_qual.level = range.max_qual.level;
			opts->max_qual.noise = range.max_qual.noise;
			opts->max_qual.updated = range.max_qual.updated;

			opts->avg_qual.qual = range.avg_qual.qual;
			opts->avg_qual.level = range.avg_qual.level;
			opts->avg_qual.noise = range.avg_qual.noise;
			opts->avg_qual.updated = range.avg_qual.updated;

			opts->num_freqs = MIN (range.num_frequency, IW_MAX_FREQUENCIES);
			for (i = 0; i < opts->num_freqs; i++)
				opts->freqs[i] = iw_freq2float (&(range.freq[i]));

			opts->we_version = range.we_version_compiled;
		}
		nm_dev_sock_close (sk);
	}

	return TRUE;
}


/*
 * nm_device_new
 *
 * Creates and initializes the structure representation of an NM device.  For test
 * devices, a device type other than DEVICE_TYPE_DONT_KNOW must be specified, this
 * argument is ignored for real hardware devices since they are auto-probed.
 *
 */
NMDevice *nm_device_new (const char *iface, const char *udi, gboolean test_dev, NMDeviceType test_dev_type, NMData *app_data)
{
	NMDevice	*dev;
	GError	*error = NULL;
	nm_completion_args args;

	g_return_val_if_fail (iface != NULL, NULL);
	g_return_val_if_fail (strlen (iface) > 0, NULL);
	g_return_val_if_fail (app_data != NULL, NULL);

	/* Test devices must have a valid type specified */
	if (test_dev && !(test_dev_type != DEVICE_TYPE_DONT_KNOW))
		return (NULL);

	/* Another check to make sure we don't create a test device unless
	 * test devices were enabled on the command line.
	 */
	if (!app_data->enable_test_devices && test_dev)
	{
		nm_warning ("attempt to create a test device, but test devices were not enabled "
			    "on the command line.  Will not create the device.");
		return (NULL);
	}

	dev = g_malloc0 (sizeof (NMDevice));

	dev->refcount = 2; /* 1 for starters, and another 1 for the worker thread */
	dev->app_data = app_data;
	dev->iface = g_strdup (iface);
	dev->test_device = test_dev;
	dev->udi = g_strdup (udi);
	dev->driver = nm_get_device_driver_name (dev);
	dev->use_dhcp = TRUE;

	/* Real hardware devices are probed for their type, test devices must have
	 * their type specified.
	 */
	if (test_dev)
		dev->type = test_dev_type;
	else
		dev->type = nm_device_test_wireless_extensions (dev) ?
						DEVICE_TYPE_WIRELESS_ETHERNET : DEVICE_TYPE_WIRED_ETHERNET;

	/* Device thread's main loop */
	dev->context = g_main_context_new ();
	dev->loop = g_main_loop_new (dev->context, FALSE);

	if (!dev->context || !dev->loop)
		goto err;

	/* Have to bring the device up before checking link status and other stuff */
	nm_device_bring_up_wait (dev, 0);

	/* First check for driver support */
	if (nm_is_driver_supported (dev))
		dev->capabilities |= NM_DEVICE_CAP_NM_SUPPORTED;

	/* Then discover devices-specific capabilities */
	if (dev->capabilities & NM_DEVICE_CAP_NM_SUPPORTED)
	{
		dev->capabilities |= nm_device_discover_capabilities (dev);

		/* Initialize wireless-specific options */
		if (nm_device_is_wireless (dev) && !nm_device_wireless_init (dev))
			goto err;

		nm_device_set_link_active (dev, nm_device_probe_link_state (dev));
		nm_device_update_ip4_address (dev);
		nm_device_update_hw_address (dev);

		/* Grab IP config data for this device from the system configuration files */
		dev->system_config_data = nm_system_device_get_system_config (dev);
		dev->use_dhcp = nm_system_device_get_use_dhcp (dev);
	}

	nm_print_device_capabilities (dev);

	dev->worker = g_thread_create (nm_device_worker, dev, TRUE, &error);
	if (!dev->worker)
	{
		nm_error ("could not create device worker thread. (glib said: '%s')", error->message);
		g_error_free (error);
		goto err;
	}

	/* Block until our device thread has actually had a chance to start. */
	args[0] = &dev->worker_started;
	args[1] = (gpointer) "nm_device_new(): waiting for device's worker thread to start";
	args[2] = GINT_TO_POINTER (LOG_INFO);
	args[3] = GINT_TO_POINTER (0);
	nm_wait_for_completion (NM_COMPLETION_TRIES_INFINITY,
			G_USEC_PER_SEC / 20, nm_completion_boolean_test, NULL, args);

	nm_info ("nm_device_new(): device's worker thread started, continuing.");

	return (dev);

err:
	/* Initial refcount is 2 */
	nm_device_unref (dev);
	nm_device_unref (dev);
	return NULL;
}


/*
 * Refcounting functions
 */
void nm_device_ref (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	dev->refcount++;
}

/*
 * nm_device_unref
 *
 * Decreases the refcount on a device by 1, and if the refcount reaches 0,
 * deallocates memory used by the device.
 *
 * Returns:	FALSE if device was not deallocated
 *			TRUE if device was deallocated
 */
gboolean nm_device_unref (NMDevice *dev)
{
	gboolean	deleted = FALSE;

	g_return_val_if_fail (dev != NULL, TRUE);

	if (dev->refcount == 1)
	{
		nm_device_worker_thread_stop (dev);
		nm_device_bring_down (dev);

		if (nm_device_is_wireless (dev))
		{
			nm_device_ap_list_clear (dev);

			g_mutex_free (dev->options.wireless.scan_mutex);
			if (dev->options.wireless.ap_list)
				nm_ap_list_unref (dev->options.wireless.ap_list);
		}

		nm_system_device_free_system_config (dev, dev->system_config_data);
		if (dev->ip4_config)
			nm_ip4_config_unref (dev->ip4_config);

		if (dev->act_request)
			nm_act_request_unref (dev->act_request);

		g_free (dev->udi);
		g_free (dev->iface);
		g_free (dev->driver);
		memset (dev, 0, sizeof (NMDevice));
		g_free (dev);
		deleted = TRUE;
	}
	else
		dev->refcount--;

	return deleted;
}


/*
 * nm_device_worker
 *
 * Main thread of the device.
 *
 */
static gpointer nm_device_worker (gpointer user_data)
{
	NMDevice *dev = (NMDevice *)user_data;

	if (!dev)
	{
		nm_error ("received NULL device object, NetworkManager cannot continue.");
		exit (1);
	}

	/* Start the scanning timeout for devices that can do scanning */
	if (nm_device_is_wireless (dev) && nm_device_get_supports_wireless_scan (dev))
	{
		GSource			*source = g_idle_source_new ();
		guint			 source_id = 0;
		NMWirelessScanCB	*scan_cb;

		scan_cb = g_malloc0 (sizeof (NMWirelessScanCB));
		scan_cb->dev = dev;
		scan_cb->force = TRUE;

		g_source_set_callback (source, nm_device_wireless_scan, scan_cb, NULL);
		source_id = g_source_attach (source, dev->context);
		g_source_unref (source);
	}

	dev->worker_started = TRUE;
	g_main_loop_run (dev->loop);

	g_main_loop_unref (dev->loop);
	g_main_context_unref (dev->context);

	dev->loop = NULL;
	dev->context = NULL;

	nm_device_unref (dev);

	return NULL;
}


void nm_device_worker_thread_stop (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	if (dev->loop)
		g_main_loop_quit (dev->loop);
	if (dev->worker)
	{
		g_thread_join (dev->worker);
		dev->worker = NULL;
	}
}


/*
 * nm_device_wireless_discover_capabilities
 *
 * Figure out wireless-specific capabilities
 *
 */
static guint32 nm_device_wireless_discover_capabilities (NMDevice *dev)
{
	NMSock *			sk;
	int				err;
	wireless_scan_head	scan_data;
	guint32			caps = NM_DEVICE_CAP_NONE;

	g_return_val_if_fail (dev != NULL, NM_DEVICE_CAP_NONE);
	g_return_val_if_fail (nm_device_is_wireless (dev), NM_DEVICE_CAP_NONE);

	/* A test wireless device can always scan (we generate fake scan data for it) */
	if (dev->test_device)
		caps |= NM_DEVICE_CAP_WIRELESS_SCAN;
	else
	{
		if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
		{
			struct iwreq wrq;
			memset (&wrq, 0, sizeof (struct iwreq));
			err = iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWSCAN, &wrq);
			if (!((err == -1) && (errno == EOPNOTSUPP)))
				caps |= NM_DEVICE_CAP_WIRELESS_SCAN;
			nm_dev_sock_close (sk);
		}
	}

	return caps;
}


/*
 * nm_device_wireless_discover_capabilities
 *
 * Figure out wireless-specific capabilities
 *
 */
static guint32 nm_device_wired_discover_capabilities (NMDevice *dev)
{
	guint32		caps = NM_DEVICE_CAP_NONE;
	const char *	udi = NULL;
	char *		usb_test = NULL;
	LibHalContext *ctx = NULL;

	g_return_val_if_fail (dev != NULL, NM_DEVICE_CAP_NONE);
	g_return_val_if_fail (nm_device_is_wired (dev), NM_DEVICE_CAP_NONE);
	g_return_val_if_fail (dev->app_data != NULL, NM_DEVICE_CAP_NONE);

	/* cipsec devices are also explicitly unsupported at this time */
	if (strstr (nm_device_get_iface (dev), "cipsec"))
		return NM_DEVICE_CAP_NONE;

	/* Ignore Ethernet-over-USB devices too for the moment (Red Hat #135722) */
	ctx = dev->app_data->hal_ctx;
	udi = nm_device_get_udi (dev);
	if (    libhal_device_property_exists (ctx, udi, "usb.interface.class", NULL)
		&& (usb_test = libhal_device_get_property_string (ctx, udi, "usb.interface.class", NULL)))
	{
		libhal_free_string (usb_test);
		return NM_DEVICE_CAP_NONE;
	}

	if (supports_ethtool_carrier_detect (dev) || supports_mii_carrier_detect (dev))
		caps |= NM_DEVICE_CAP_CARRIER_DETECT;

	return caps;
}


/*
 * nm_device_discover_capabilities
 *
 * Called only at device initialization time to discover device-specific
 * capabilities.
 *
 */
static guint32 nm_device_discover_capabilities (NMDevice *dev)
{
	guint32 caps = NM_DEVICE_CAP_NONE;

	g_return_val_if_fail (dev != NULL, NM_DEVICE_CAP_NONE);

	/* Don't touch devices that we already don't support */
	if (!(dev->capabilities & NM_DEVICE_CAP_NM_SUPPORTED))
		return NM_DEVICE_CAP_NONE;

	if (nm_device_is_wired (dev))
		caps |= nm_device_wired_discover_capabilities (dev);
	else if (nm_device_is_wireless (dev))
		caps |= nm_device_wireless_discover_capabilities (dev);

	return caps;
}


/*
 * nm_device_get_app_data
 *
 */
NMData *nm_device_get_app_data (const NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->app_data);
}


/*
 * Get/Set for "removed" flag
 */
gboolean nm_device_get_removed (const NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, TRUE);

	return (dev->removed);
}

void nm_device_set_removed (NMDevice *dev, const gboolean removed)
{
	g_return_if_fail (dev != NULL);

	dev->removed = removed;
}


/*
 * Return the amount of time we should wait for the device
 * to get a link, based on the # of frequencies it has to
 * scan.
 */
static gint nm_device_get_association_pause_value (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, -1);
	g_return_val_if_fail (nm_device_is_wireless (dev), -1);

	/* If the card supports more than 14 channels, we should probably wait
	 * around 10s so it can scan them all. After we set the ESSID on the card, the card
	 * has to scan all channels to find our requested AP (which can take a long time
	 * if it is an A/B/G chipset like the Atheros 5212, for example).
	 */
	if (dev->options.wireless.num_freqs > 14)
		return 8;
	else
		return 5;
}


/*
 * Get/set functions for UDI
 */
char * nm_device_get_udi (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (dev->udi);
}

void nm_device_set_udi (NMDevice *dev, const char *udi)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (udi != NULL);

	if (dev->udi)
		g_free (dev->udi);

	dev->udi = g_strdup (udi);
}


/*
 * Get/set functions for iface
 */
const char * nm_device_get_iface (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (dev->iface);
}


/*
 * Get/set functions for driver
 */
const char * nm_device_get_driver (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (dev->driver);
}


/*
 * Get/set functions for type
 */
guint nm_device_get_type (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, DEVICE_TYPE_DONT_KNOW);

	return (dev->type);
}

gboolean nm_device_is_wireless (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->type == DEVICE_TYPE_WIRELESS_ETHERNET);
}

gboolean nm_device_is_wired (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->type == DEVICE_TYPE_WIRED_ETHERNET);
}


/*
 * Accessor for device capabilities
 */
guint32 nm_device_get_capabilities (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NM_DEVICE_CAP_NONE);

	return dev->capabilities;
}


/*
 * Get/set functions for link_active
 */
gboolean nm_device_has_active_link (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->link_active);
}

void nm_device_set_link_active (NMDevice *dev, const gboolean link_active)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);

	if (dev->link_active != link_active)
	{
		dev->link_active = link_active;

		/* Deactivate a currently active device */
		if (!link_active && nm_device_get_act_request (dev))
		{
			nm_device_deactivate (dev);
			nm_policy_schedule_device_change_check (dev->app_data);
		}
		else if (link_active && !nm_device_get_act_request (dev))
		{
			NMDevice *	act_dev = nm_get_active_device (dev->app_data);
			NMActRequest *	act_dev_req = act_dev ? nm_device_get_act_request (act_dev) : NULL;

			/* Should we switch to this device now that it has a link?
			 *
			 * Only auto-switch for wired devices, AND...
			 *
			 * only switch to fully-supported devices, since ones that don't have carrier detection
			 * capability usually report the carrier as "always on" even if its not really on.  User
			 * must manually choose semi-supported devices.
			 *
			 */
			if (nm_device_is_wired (dev) && (nm_device_get_capabilities (dev) & NM_DEVICE_CAP_CARRIER_DETECT))
			{
				gboolean 		do_switch = act_dev ? FALSE : TRUE;	/* If no currently active device, switch to this one */
				NMActRequest *	act_req;

				/* If active device is wireless, switch to this one */
				if (act_dev && nm_device_is_wireless (act_dev) && act_dev_req && !nm_act_request_get_user_requested (act_dev_req))
					do_switch = TRUE;

				if (do_switch && (act_req = nm_act_request_new (dev->app_data, dev, NULL, TRUE)))
				{
					nm_info ("Will activate wired connection '%s' because it now has a link.", nm_device_get_iface (dev));
					nm_policy_schedule_device_activation (act_req);
				}
			}
		}
		nm_dbus_schedule_device_status_change_signal	(dev->app_data, dev, NULL, link_active ? DEVICE_CARRIER_ON : DEVICE_CARRIER_OFF);
	}
}


/*
 * Get function for supports_wireless_scan
 */
gboolean nm_device_get_supports_wireless_scan (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	if (!nm_device_is_wireless (dev))
		return (FALSE);

	return (dev->capabilities & NM_DEVICE_CAP_WIRELESS_SCAN);
}


/*
 * nm_device_get_supports_carrier_detect
 */
gboolean nm_device_get_supports_carrier_detect (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	if (!nm_device_is_wired (dev))
		return (FALSE);

	return (dev->capabilities & NM_DEVICE_CAP_CARRIER_DETECT);
}

/*
 * nm_device_wireless_is_associated
 *
 * Figure out whether or not we're associated to an access point
 */
static gboolean nm_device_wireless_is_associated (NMDevice *dev)
{
	struct iwreq	 wrq;
	NMSock		*sk;
	gboolean		 associated = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return nm_device_has_active_link (dev);

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)) == NULL)
		return FALSE;

	/* Some cards, for example ipw2x00 cards, can short-circuit the MAC
	 * address check using this check on IWNAME.  Its faster.
	 */
	memset (&wrq, 0, sizeof (struct iwreq));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWNAME.", nm_device_get_iface (dev));
#endif
	if (iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWNAME, &wrq) >= 0)
	{
		if (!strcmp(wrq.u.name, "unassociated"))
		{
			associated = FALSE;
			goto out;
		}
	}

	if (!associated)
	{
		/*
		 * For all other wireless cards, the best indicator of a "link" at this time
		 * seems to be whether the card has a valid access point MAC address.
		 * Is there a better way?  Some cards don't work too well with this check, ie
		 * Lucent WaveLAN.
		 */
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWAP.", nm_device_get_iface (dev));
#endif
		if (iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWAP, &wrq) >= 0)
			if (nm_ethernet_address_is_valid ((struct ether_addr *)(&(wrq.u.ap_addr.sa_data))))
				associated = TRUE;
	}

out:
	nm_dev_sock_close (sk);

	return associated;
}

/*
 * nm_device_probe_wireless_link_state
 *
 * Gets the link state of a wireless device.  WARNING: results are not
 * conclusive if the device is currently activating.
 *
 */
static gboolean nm_device_probe_wireless_link_state (NMDevice *dev)
{
	gboolean 		 link = FALSE;
	NMAccessPoint	*best_ap;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return nm_device_has_active_link (dev);

	if ((best_ap = nm_device_get_best_ap (dev)))
	{
		link = link_to_specific_ap (dev, best_ap, TRUE);
		nm_ap_unref (best_ap);
	}

	return link;
}


/*
 * nm_device_probe_wired_link_state
 *
 * 
 *
 */
static gboolean nm_device_probe_wired_link_state (NMDevice *dev)
{
	gboolean	link = FALSE;
	gchar *contents, *carrier_path;
	gsize length;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wired (dev) == TRUE, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return nm_device_has_active_link (dev);

	if (dev->removed)
		return FALSE;

	carrier_path = g_strdup_printf ("/sys/class/net/%s/carrier", dev->iface);
	if (g_file_get_contents (carrier_path, &contents, &length, NULL))
	{
		link = (gboolean) atoi (contents);
		g_free (contents);
	}
	g_free (carrier_path);

	/* We say that non-carrier-detect devices always have a link, because
	 * they never get auto-selected by NM.  User has to force them on us,
	 * so we just hope the user knows whether or not the cable's plugged in.
	 */
	if (!(dev->capabilities & NM_DEVICE_CAP_CARRIER_DETECT))
		link = TRUE;

	return link;
}


/*
 * nm_device_probe_link_state
 *
 * Return the current link state of the device.
 *
 */
gboolean nm_device_probe_link_state (NMDevice *dev)
{
	gboolean	link = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (!nm_device_is_up (dev))
		nm_device_bring_up (dev);

	if (nm_device_is_wireless (dev))
	{
		link = nm_device_probe_wireless_link_state (dev);
		nm_device_update_signal_strength (dev);
	}
	else if (nm_device_is_wired (dev))
		link = nm_device_probe_wired_link_state (dev);

	return link;
}


/*
 * nm_device_get_essid
 *
 * If a device is wireless, return the essid that it is attempting
 * to use.
 *
 * Returns:	allocated string containing essid.  Must be freed by caller.
 *
 */
char * nm_device_get_essid (NMDevice *dev)
{
	NMSock	*sk;
	int		 err;
	
	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);

	/* Test devices return the essid of their "best" access point
	 * or if there is none, the contents of the cur_essid field.
	 */
	if (dev->test_device)
	{
		NMAccessPoint	*best_ap = nm_device_get_best_ap (dev);
		char			*essid = dev->options.wireless.cur_essid;

		/* Or, if we've got a best ap, use that ESSID instead */
		if (best_ap)
		{
			essid = nm_ap_get_essid (best_ap);
			nm_ap_unref (best_ap);
		}
		return essid;
	}
	
	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		wireless_config	info;

#ifdef IOCTL_DEBUG
		nm_info ("%s: About to GET 'basic config' for ESSID.", nm_device_get_iface (dev));
#endif
		err = iw_get_basic_config (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &info);
		if (err >= 0)
		{
			if (dev->options.wireless.cur_essid)
				g_free (dev->options.wireless.cur_essid);
			dev->options.wireless.cur_essid = g_strdup (info.essid);
		}
		else
			nm_warning ("nm_device_get_essid(): error getting ESSID for device %s.  errno = %d", nm_device_get_iface (dev), errno);

		nm_dev_sock_close (sk);
	}

	return (dev->options.wireless.cur_essid);
}


/*
 * nm_device_set_essid
 *
 * If a device is wireless, set the essid that it should use.
 */
void nm_device_set_essid (NMDevice *dev, const char *essid)
{
	NMSock			*sk;
	int				 err;
	struct iwreq		 wreq;
	unsigned char		 safe_essid[IW_ESSID_MAX_SIZE + 1] = "\0";
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices directly set cur_essid */
	if (dev->test_device)
	{
		if (dev->options.wireless.cur_essid)
			g_free (dev->options.wireless.cur_essid);
		dev->options.wireless.cur_essid = g_strdup (essid);
		return;
	}

	/* Make sure the essid we get passed is a valid size */
	if (!essid)
		safe_essid[0] = '\0';
	else
	{
		strncpy ((char *) safe_essid, essid, IW_ESSID_MAX_SIZE);
		safe_essid[IW_ESSID_MAX_SIZE] = '\0';
	}

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		wreq.u.essid.pointer = (caddr_t) safe_essid;
		wreq.u.essid.length	 = strlen ((char *) safe_essid) + 1;
		wreq.u.essid.flags	 = 1;	/* Enable essid on card */
	
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to SET IWESSID.", nm_device_get_iface (dev));
#endif
		if ((err = iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWESSID, &wreq)) == -1)
		{
			if (errno != ENODEV)
				nm_warning ("nm_device_set_essid(): error setting ESSID '%s' for device %s.  errno = %d", safe_essid, nm_device_get_iface (dev), errno);
		}

		nm_dev_sock_close (sk);

		/* Orinoco cards seem to need extra time here to not screw
		 * up the firmware, which reboots when you set the ESSID.
		 * Unfortunately, there's no way to know when the card is back up
		 * again.  Sigh...
		 */
		sleep (2);
	}
}


/*
 * nm_device_get_frequency
 *
 * For wireless devices, get the frequency we broadcast/receive on.
 *
 */
static double nm_device_get_frequency (NMDevice *dev)
{
	NMSock	*sk;
	int		 err;
	double	 freq = 0;

	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	/* Test devices don't really have a frequency, they always succeed */
	if (dev->test_device)
		return 703000000;

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		struct iwreq		wrq;

#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWFREQ.", nm_device_get_iface (dev));
#endif
		err = iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWFREQ, &wrq);
		if (err >= 0)
			freq = iw_freq2float (&wrq.u.freq);
		if (err == -1)
			nm_warning ("nm_device_get_frequency(): error getting frequency for device %s.  errno = %d", nm_device_get_iface (dev), errno);

		nm_dev_sock_close (sk);
	}
	return (freq);
}


/*
 * nm_device_set_frequency
 *
 * For wireless devices, set the frequency to broadcast/receive on.
 * A frequency <= 0 means "auto".
 *
 */
static void nm_device_set_frequency (NMDevice *dev, const double freq)
{
	NMSock	*sk;
	int		 err;
	
	/* HACK FOR NOW */
	if (freq <= 0)
		return;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices don't really have a frequency, they always succeed */
	if (dev->test_device)
		return;

	if (nm_device_get_frequency (dev) == freq)
		return;

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		struct iwreq		wrq;

		if (freq <= 0)
		{
			/* Auto */
			/* People like to make things hard for us.  Even though iwlib/iwconfig say
			 * that wrq.u.freq.m should be -1 for "auto" mode, nobody actually supports
			 * that.  Madwifi actually uses "0" to mean "auto".  So, we'll try 0 first
			 * and if that doesn't work, fall back to the iwconfig method and use -1.
			 *
			 * As a further note, it appears that Atheros/Madwifi cards can't go back to
			 * any-channel operation once you force set the channel on them.  For example,
			 * if you set a prism54 card to a specific channel, but then set the ESSID to
			 * something else later, it will scan for the ESSID and switch channels just fine.
			 * Atheros cards, however, just stay at the channel you previously set and don't
			 * budge, no matter what you do to them, until you tell them to go back to
			 * any-channel operation.
			 */
			wrq.u.freq.m = 0;
			wrq.u.freq.e = 0;
			wrq.u.freq.flags = 0;
		}
		else
		{
			/* Fixed */
			wrq.u.freq.flags = IW_FREQ_FIXED;
			iw_float2freq (freq, &wrq.u.freq);
		}
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to SET IWFREQ.", nm_device_get_iface (dev));
#endif
		if ((err = iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWFREQ, &wrq)) == -1)
		{
			gboolean	success = FALSE;
			if ((freq <= 0) && ((errno == EINVAL) || (errno == EOPNOTSUPP)))
			{
				/* Ok, try "auto" the iwconfig way if the Atheros way didn't work */
				wrq.u.freq.m = -1;
				wrq.u.freq.e = 0;
				wrq.u.freq.flags = 0;
				if (iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWFREQ, &wrq) != -1)
					success = TRUE;
			}
		}

		nm_dev_sock_close (sk);
	}
}


/*
 * nm_device_get_bitrate
 *
 * For wireless devices, get the bitrate to broadcast/receive at.
 * Returned value is rate in KHz.
 *
 */
static int nm_device_get_bitrate (NMDevice *dev)
{
	NMSock		*sk;
	int			 err = -1;
	struct iwreq	 wrq;
	
	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	/* Test devices don't really have a bitrate, they always succeed */
	if (dev->test_device)
		return 11;

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWRATE.", nm_device_get_iface (dev));
#endif
		err = iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWRATE, &wrq);
		nm_dev_sock_close (sk);
	}

	return ((err >= 0) ? wrq.u.bitrate.value / 1000 : 0);
}


/*
 * nm_device_set_bitrate
 *
 * For wireless devices, set the bitrate to broadcast/receive at.
 * Rate argument should be in Mbps (mega-bits per second), or 0 for automatic.
 *
 */
static void nm_device_set_bitrate (NMDevice *dev, const int Mbps)
{
	NMSock	*sk;
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices don't really have a bitrate, they always succeed */
	if (dev->test_device)
		return;

	if (nm_device_get_bitrate (dev) == Mbps)
		return;

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		struct iwreq		wrq;

		if (Mbps != 0)
		{
			wrq.u.bitrate.value = Mbps * 1000;
			wrq.u.bitrate.fixed = 1;
		}
		else
		{
			/* Auto bitrate */
			wrq.u.bitrate.value = -1;
			wrq.u.bitrate.fixed = 0;
		}
		/* Silently fail as not all drivers support setting bitrate yet (ipw2x00 for example) */
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to SET IWRATE.", nm_device_get_iface (dev));
#endif
		iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWRATE, &wrq);

		nm_dev_sock_close (sk);
	}
}


/*
 * nm_device_get_ap_address
 *
 * If a device is wireless, get the access point's ethernet address
 * that the card is associated with.
 */
void nm_device_get_ap_address (NMDevice *dev, struct ether_addr *addr)
{
	NMSock		*sk;
	struct iwreq	 wrq;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (addr != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	memset (addr, 0, sizeof (struct ether_addr));

	/* Test devices return an invalid address when there's no link,
	 * and a made-up address when there is a link.
	 */
	if (dev->test_device)
	{
		struct ether_addr	good_addr = { {0x70, 0x37, 0x03, 0x70, 0x37, 0x03} };
		struct ether_addr	bad_addr = { {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} };
		gboolean			link = nm_device_has_active_link (dev);

		memcpy ((link ? &good_addr : &bad_addr), &(wrq.u.ap_addr.sa_data), sizeof (struct ether_addr));
		return;
	}

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWAP.", nm_device_get_iface (dev));
#endif
		if (iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWAP, &wrq) >= 0)
			memcpy (addr, &(wrq.u.ap_addr.sa_data), sizeof (struct ether_addr));
		nm_dev_sock_close (sk);
	}
}


/*
 * nm_device_set_enc_key
 *
 * If a device is wireless, set the encryption key that it should use.
 *
 * key:	encryption key to use, or NULL or "" to disable encryption.
 *		NOTE that at this time, the key must be the raw HEX key, not
 *		a passphrase.
 */
void nm_device_set_enc_key (NMDevice *dev, const char *key, NMDeviceAuthMethod auth_method)
{
	NMSock			*sk;
	struct iwreq		wreq;
	int				keylen;
	unsigned char		safe_key[IW_ENCODING_TOKEN_MAX + 1];
	gboolean			set_key = FALSE;
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices just ignore encryption keys */
	if (dev->test_device)
		return;

	/* Make sure the essid we get passed is a valid size */
	if (!key)
		safe_key[0] = '\0';
	else
	{
		strncpy ((char *) safe_key, key, IW_ENCODING_TOKEN_MAX);
		safe_key[IW_ENCODING_TOKEN_MAX] = '\0';
	}

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		wreq.u.data.pointer = (caddr_t) NULL;
		wreq.u.data.length = 0;
		wreq.u.data.flags = IW_ENCODE_ENABLED;

		/* Unfortunately, some drivers (Cisco) don't make a distinction between
		 * Open System authentication mode and whether or not to use WEP.  You
		 * DON'T have to use WEP when using Open System, but these cards force
		 * it.  Therefore, we have to set Open System mode when using WEP.
		 */

		if (strlen ((char *) safe_key) == 0)
		{
			wreq.u.data.flags |= IW_ENCODE_DISABLED | IW_ENCODE_NOKEY;
			set_key = TRUE;
		}
		else
		{
			unsigned char		parsed_key[IW_ENCODING_TOKEN_MAX + 1];

			keylen = iw_in_key_full (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev),
						(char *) safe_key, &parsed_key[0], &wreq.u.data.flags);
			if (keylen > 0)
			{
				switch (auth_method)
				{
					case NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM:
						wreq.u.data.flags |= IW_ENCODE_OPEN;
						break;
					case NM_DEVICE_AUTH_METHOD_SHARED_KEY:
						wreq.u.data.flags |= IW_ENCODE_RESTRICTED;
						break;
					default:
						wreq.u.data.flags |= IW_ENCODE_RESTRICTED;
						break;
				}
				wreq.u.data.pointer	=  (caddr_t) &parsed_key;
				wreq.u.data.length	=  keylen;
				set_key = TRUE;
			}
		}

		if (set_key)
		{
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to SET IWENCODE.", nm_device_get_iface (dev));
#endif
			if (iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWENCODE, &wreq) == -1)
			{
				if (errno != ENODEV)
					nm_warning ("nm_device_set_enc_key(): error setting key for device %s.  errno = %d", nm_device_get_iface (dev), errno);
			}
		}

		nm_dev_sock_close (sk);
	} else nm_warning ("nm_device_set_enc_key(): could not get wireless control socket.");
}


/*
 * nm_device_get_signal_strength
 *
 * Get the current signal strength of a wireless device.  This only works when
 * the card is associated with an access point, so will only work for the
 * active device.
 *
 * Returns:	-1 on error
 *			0 - 100  strength percentage of the connection to the current access point
 *
 */
gint8 nm_device_get_signal_strength (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, -1);
	g_return_val_if_fail (nm_device_is_wireless (dev), -1);

	return (dev->options.wireless.strength);
}


/*
 * nm_device_update_signal_strength
 *
 * Update the device's idea of the strength of its connection to the
 * current access point.
 *
 */
void nm_device_update_signal_strength (NMDevice *dev)
{
	gboolean		has_range = FALSE;
	NMSock *		sk;
	iwrange		range;
	iwstats		stats;
	int			percent = -1;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));
	g_return_if_fail (dev->app_data != NULL);

	/* Grab the scan lock since our strength is meaningless during a scan. */
	if (!nm_try_acquire_mutex (dev->options.wireless.scan_mutex, __FUNCTION__))
		return;

	/* If we aren't the active device, we don't really have a signal strength
	 * that would mean anything.
	 */
	if (!dev->act_request)
	{
		dev->options.wireless.strength = -1;
		goto out;
	}

	/* Fake a value for test devices */
	if (dev->test_device)
	{
		dev->options.wireless.strength = 75;
		goto out;
	}

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		memset (&range, 0, sizeof (iwrange));
		memset (&stats, 0, sizeof (iwstats));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET 'iwrange'.", nm_device_get_iface (dev));
#endif
		has_range = (iw_get_range_info (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &range) >= 0);
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET 'iwstats'.", nm_device_get_iface (dev));
#endif
		if (iw_get_stats (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &stats, &range, has_range) == 0)
		{
			percent = nm_wireless_qual_to_percent (&stats.qual, (const iwqual *)(&dev->options.wireless.max_qual),
					(const iwqual *)(&dev->options.wireless.avg_qual));
		}
		nm_dev_sock_close (sk);
	}

	/* Try to smooth out the strength.  Atmel cards, for example, will give no strength
	 * one second and normal strength the next.
	 */
	if ((percent == -1) && (++dev->options.wireless.invalid_strength_counter <= 3))
		percent = dev->options.wireless.strength;
	else
		dev->options.wireless.invalid_strength_counter = 0;

	if (percent != dev->options.wireless.strength)
		nm_dbus_signal_device_strength_change (dev->app_data->dbus_connection, dev, percent);

	dev->options.wireless.strength = percent;

out:
	nm_unlock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);
}


/*
 * nm_device_get_ip4_address
 *
 * Get a device's IPv4 address
 *
 */
guint32 nm_device_get_ip4_address(NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->ip4_address);
}

void nm_device_update_ip4_address (NMDevice *dev)
{
	guint32		new_address;
	struct ifreq	req;
	NMSock		*sk;
	int			err;
	
	g_return_if_fail (dev  != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (nm_device_get_iface (dev) != NULL);

	/* Test devices get a nice, bogus IP address */
	if (dev->test_device)
	{
		dev->ip4_address = 0x07030703;
		return;
	}

	if ((sk = nm_dev_sock_open (dev, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
		return;
	
	memset (&req, 0, sizeof (struct ifreq));
	strncpy ((char *)(&req.ifr_name), nm_device_get_iface (dev), strlen (nm_device_get_iface (dev)));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IFADDR.", nm_device_get_iface (dev));
#endif
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIFADDR, &req);
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with GET IFADDR.", nm_device_get_iface (dev));
#endif
	nm_dev_sock_close (sk);
	if (err != 0)
		return;

	new_address = ((struct sockaddr_in *)(&req.ifr_addr))->sin_addr.s_addr;
	if (new_address != nm_device_get_ip4_address (dev))
		dev->ip4_address = new_address;
}


/*
 * nm_device_get_ip6_address
 *
 * Get a device's IPv6 address
 *
 */
void nm_device_get_ip6_address(NMDevice *dev)
{
	/* FIXME
	 * Implement
	 */
}


/*
 * nm_device_get_hw_address
 *
 * Get a device's hardware address
 *
 */
void nm_device_get_hw_address (NMDevice *dev, struct ether_addr *addr)
{
	g_return_if_fail (addr != NULL);
	g_return_if_fail (dev != NULL);

	memcpy (addr, &(dev->hw_addr), sizeof (struct ether_addr));
}

void nm_device_update_hw_address (NMDevice *dev)
{
	struct ifreq	req;
	NMSock		*sk;
	int			err;

	g_return_if_fail (dev  != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (nm_device_get_iface (dev) != NULL);

	/* Test devices get a nice, bogus IP address */
	if (dev->test_device)
	{
		memset (&(dev->hw_addr), 0, sizeof (struct ether_addr));
		return;
	}

	if ((sk = nm_dev_sock_open (dev, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
		return;
	
	memset (&req, 0, sizeof (struct ifreq));
	strncpy ((char *)(&req.ifr_name), nm_device_get_iface (dev), strlen (nm_device_get_iface (dev)));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IFHWADDR.", nm_device_get_iface (dev));
#endif
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIFHWADDR, &req);
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with GET IFHWADDR.", nm_device_get_iface (dev));
#endif
	nm_dev_sock_close (sk);
	if (err != 0)
		return;

	memcpy (&(dev->hw_addr), &(req.ifr_hwaddr.sa_data), sizeof (struct ether_addr));
}


/*
 * nm_device_set_up_down
 *
 * Set the up flag on the device on or off
 *
 */
static void nm_device_set_up_down (NMDevice *dev, gboolean up)
{
	g_return_if_fail (dev != NULL);

	/* Test devices do whatever we tell them to do */
	if (dev->test_device)
	{
		dev->test_device_up = up;
		return;
	}

	nm_system_device_set_up_down (dev, up);

	/* Make sure we have a valid MAC address, some cards reload firmware when they
	 * are brought up.
	 */
	if (!nm_ethernet_address_is_valid (&(dev->hw_addr)))
		nm_device_update_hw_address (dev);
}


/*
 * Interface state functions: bring up, down, check
 *
 */
gboolean nm_device_is_up (NMDevice *dev)
{
	NMSock *		sk;
	struct ifreq	ifr;
	int			err;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (dev->test_device)
		return (dev->test_device_up);

	if ((sk = nm_dev_sock_open (dev, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
		return (FALSE);

	/* Get device's flags */
	strcpy (ifr.ifr_name, nm_device_get_iface (dev));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IFFLAGS.", nm_device_get_iface (dev));
#endif
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGIFFLAGS, &ifr);
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with GET IFFLAGS.", nm_device_get_iface (dev));
#endif
	nm_dev_sock_close (sk);
	if (!err)
		return (!((ifr.ifr_flags^IFF_UP) & IFF_UP));

	if (errno != ENODEV)
		nm_warning ("nm_device_is_up() could not get flags for device %s.  errno = %d", nm_device_get_iface (dev), errno );

	return FALSE;
}

/* I really wish nm_v_wait_for_completion_or_timeout could translate these
 * to first class args instead of a all this void * arg stuff, so these
 * helpers could be nice and _tiny_. */
static gboolean nm_completion_device_is_up_test (int tries, nm_completion_args args)
{
	NMDevice *dev = args[0];
	gboolean *err = args[1];
	gboolean cancelable = GPOINTER_TO_INT (args[2]);

	g_return_val_if_fail (dev != NULL, TRUE);
	g_return_val_if_fail (err != NULL, TRUE);

	*err = FALSE;
	if (cancelable && nm_device_activation_should_cancel (dev)) {
		*err = TRUE;
		return TRUE;
	}
	if (nm_device_is_up (dev))
		return TRUE;
	return FALSE;
}

void nm_device_bring_up (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_device_set_up_down (dev, TRUE);
}

gboolean nm_device_bring_up_wait (NMDevice *dev, gboolean cancelable)
{
	gboolean err = FALSE;
	nm_completion_args args;

	g_return_val_if_fail (dev != NULL, TRUE);

	nm_device_bring_up (dev);

	args[0] = dev;
	args[1] = &err;
	args[2] = GINT_TO_POINTER (cancelable);
	nm_wait_for_completion (400, G_USEC_PER_SEC / 200, NULL, nm_completion_device_is_up_test, args);
	if (err)
		nm_info ("failed to bring up device %s", dev->iface);
	return err;
}

void nm_device_bring_down (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_device_set_up_down (dev, FALSE);
}

static gboolean nm_completion_device_is_down_test (int tries, nm_completion_args args)
{
	NMDevice *dev = args[0];
	gboolean *err = args[1];
	gboolean cancelable = GPOINTER_TO_INT (args[2]);

	g_return_val_if_fail (dev != NULL, TRUE);
	g_return_val_if_fail (err != NULL, TRUE);

	*err = FALSE;
	if (cancelable && nm_device_activation_should_cancel (dev)) {
		*err = TRUE;
		return TRUE;
	}
	if (!nm_device_is_up (dev))
		return TRUE;
	return FALSE;
}

static gboolean nm_device_bring_down_wait (NMDevice *dev, gboolean cancelable)
{
	gboolean err = FALSE;
	nm_completion_args args;

	g_return_val_if_fail (dev != NULL, TRUE);

	nm_device_bring_down (dev);

	args[0] = dev;
	args[1] = &err;
	args[2] = GINT_TO_POINTER (cancelable);
	nm_wait_for_completion(400, G_USEC_PER_SEC / 200, NULL,
			nm_completion_device_is_down_test, args);
	if (err)
		nm_info ("failed to bring down device %s", dev->iface);
	return err;
}


/*
 * nm_device_get_mode
 *
 * Get managed/infrastructure/adhoc mode on a device (currently wireless only)
 *
 */
NMNetworkMode nm_device_get_mode (NMDevice *dev)
{
	NMSock		*sk;
	NMNetworkMode	mode = NETWORK_MODE_UNKNOWN;

	g_return_val_if_fail (dev != NULL, NETWORK_MODE_UNKNOWN);
	g_return_val_if_fail (nm_device_is_wireless (dev), NETWORK_MODE_UNKNOWN);

	/* Force the card into Managed/Infrastructure mode */
	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		struct iwreq	wrq;

		memset (&wrq, 0, sizeof (struct iwreq));
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET IWMODE.", nm_device_get_iface (dev));
#endif
		if (iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWMODE, &wrq) == 0)
		{
			switch (wrq.u.mode)
			{
				case IW_MODE_INFRA:
					mode = NETWORK_MODE_INFRA;
					break;
				case IW_MODE_ADHOC:
					mode = NETWORK_MODE_ADHOC;
					break;
				default:
					break;
			}
		}
		else
			nm_warning ("nm_device_get_mode (%s): error getting card mode.  errno = %d", nm_device_get_iface (dev), errno);				
		nm_dev_sock_close (sk);
	}

	return (mode);
}


/*
 * nm_device_set_mode
 *
 * Set managed/infrastructure/adhoc mode on a device (currently wireless only)
 *
 */
gboolean nm_device_set_mode (NMDevice *dev, const NMNetworkMode mode)
{
	NMSock		*sk;
	gboolean		 success = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);
	g_return_val_if_fail ((mode == NETWORK_MODE_INFRA) || (mode == NETWORK_MODE_ADHOC), FALSE);

	if (nm_device_get_mode (dev) == mode)
		return TRUE;

	/* Force the card into Managed/Infrastructure mode */
	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
	{
		struct iwreq	wreq;
		gboolean		mode_good = FALSE;

		switch (mode)
		{
			case NETWORK_MODE_INFRA:
				wreq.u.mode = IW_MODE_INFRA;
				mode_good = TRUE;
				break;
			case NETWORK_MODE_ADHOC:
				wreq.u.mode = IW_MODE_ADHOC;
				mode_good = TRUE;
				break;
			default:
				mode_good = FALSE;
				break;
		}
		if (mode_good)
		{
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to SET IWMODE.", nm_device_get_iface (dev));
#endif
			if (iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWMODE, &wreq) == 0)
				success = TRUE;
			else
			{
				if (errno != ENODEV)
					nm_warning ("nm_device_set_mode (%s): error setting card to %s mode.  errno = %d",
						nm_device_get_iface (dev),
						mode == NETWORK_MODE_INFRA ? "Infrastructure" : (mode == NETWORK_MODE_ADHOC ? "adhoc" : "unknown"),
						errno);
			}
		}
		nm_dev_sock_close (sk);
	}

	return (success);
}


/*
 * nm_device_activation_start
 *
 * Tell the device thread to begin activation.
 *
 * Returns:	TRUE on success activation beginning
 *			FALSE on error beginning activation (bad params, couldn't create thread)
 *
 */
gboolean nm_device_activation_start (NMActRequest *req)
{
	NMData *		data = NULL;
	NMDevice *	dev = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	g_return_val_if_fail (!nm_device_is_activating (dev), TRUE);	/* Return if activation has already begun */

	nm_act_request_ref (req);
	dev->act_request = req;
	dev->quit_activation = FALSE;

	nm_info ("Activation (%s) started...", nm_device_get_iface (dev));

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_PREPARE);
	nm_device_activate_schedule_stage1_device_prepare (req);

	nm_schedule_state_change_signal_broadcast (data);
	nm_dbus_schedule_device_status_change_signal (data, dev, NULL, DEVICE_ACTIVATING);

	return TRUE;
}


/*
 * nm_device_activation_handle_cancel
 *
 * Cancel activation on a device and clean up.
 *
 */
static gboolean nm_device_activation_handle_cancel (NMActRequest *req)
{
	NMDevice *		dev;
	NMData *			data;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	if ((req = nm_device_get_act_request (dev)) && nm_device_is_activating (dev))
	{
		dev->act_request = NULL;
		nm_act_request_unref (req);
	}
	nm_schedule_state_change_signal_broadcast (dev->app_data);

	nm_info ("Activation (%s) cancellation handled.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_schedule_activation_handle_cancel
 *
 * Schedule the activation cancel handler
 *
 */
static void nm_device_schedule_activation_handle_cancel (NMActRequest *req)
{
	NMDevice *	dev;
	NMData *		data;
	GSource *		source;

	g_return_if_fail (req != NULL);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_info ("Activation (%s) cancellation handler scheduled...", nm_device_get_iface (dev));
	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activation_handle_cancel, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
}


/*
 * nm_device_get_act_request
 *
 * Return the devices activation request, if any.
 *
 */
NMActRequest *nm_device_get_act_request (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return dev->act_request;
}


/*
 * get_initial_auth_method
 *
 * Update the auth method of the AP from the last-known-good one saved in the allowed list
 * (which is found from NMI) and ensure that its valid with the encryption status of the AP.
 *
 */
static NMDeviceAuthMethod get_initial_auth_method (NMAccessPoint *ap, NMAccessPointList *allowed_list)
{
	g_return_val_if_fail (ap != NULL, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);

	if (nm_ap_get_encrypted (ap))
	{
		NMDeviceAuthMethod	 auth = nm_ap_get_auth_method (ap);
		NMAccessPoint		*allowed_ap = nm_ap_list_get_ap_by_essid (allowed_list, nm_ap_get_essid (ap));
		
		/* Prefer default auth method if we found one for this AP in our allowed list. */
		if (allowed_ap)
			auth = nm_ap_get_auth_method (allowed_ap);

		if (    (auth == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
			|| (auth == NM_DEVICE_AUTH_METHOD_SHARED_KEY))
			return (auth);
		else
			return (NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
	}

	return (NM_DEVICE_AUTH_METHOD_NONE);
}


/*
 * nm_device_activate_stage1_device_prepare
 *
 * Prepare for device activation
 *
 */
static gboolean nm_device_activate_stage1_device_prepare (NMActRequest *req)
{
	NMDevice *		dev;
	NMData *			data;
	NMAccessPoint *	ap;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_info ("Activation (%s) Stage 1 (Device Prepare) started...", nm_device_get_iface (dev));

	if (nm_device_is_wireless (dev))
	{
		ap = nm_act_request_get_ap (req);
		g_assert (ap);

		if (nm_ap_get_artificial (ap))
		{
			/* Some Cisco cards (340/350 PCMCIA) don't return non-broadcasting APs
			 * in their scan results, so we can't know beforehand whether or not the
			 * AP was encrypted.  We have to update their encryption status on the fly.
			 */
			if (nm_ap_get_encrypted (ap) || nm_ap_is_enc_key_valid (ap))
			{
				nm_ap_set_encrypted (ap, TRUE);
				nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
			}
		}

		/* Initial authentication method */
		nm_ap_set_auth_method (ap, get_initial_auth_method (ap, data->allowed_ap_list));
	}

	if (nm_device_activation_should_cancel (dev))
		nm_device_schedule_activation_handle_cancel (req);
	else
		nm_device_activate_schedule_stage2_device_config (req);

	nm_info ("Activation (%s) Stage 1 (Device Prepare) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage1_device_prepare
 *
 * Prepare a device for activation
 *
 */
void nm_device_activate_schedule_stage1_device_prepare (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_PREPARE);
	nm_info ("Activation (%s) Stage 1 (Device Prepare) scheduled...", nm_device_get_iface (dev));

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage1_device_prepare, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
}


static gboolean nm_device_link_test (int tries, nm_completion_args args)
{
	NMDevice *dev = args[0];
	gboolean *err = args[1];

	g_return_val_if_fail (dev != NULL, TRUE);
	g_return_val_if_fail (err != NULL, TRUE);

	if (nm_device_wireless_is_associated (dev) && nm_device_get_essid (dev))
	{
		*err = FALSE;
		return TRUE;
	}
	*err = TRUE;
	return FALSE;
}
 
static gboolean nm_device_is_up_and_associated_wait (NMDevice *dev, int timeout, int interval)
{
	gboolean err;
	const gint delay = (G_USEC_PER_SEC * nm_device_get_association_pause_value (dev)) / interval;
	const gint max_cycles = timeout * interval;
	nm_completion_args args;

	g_return_val_if_fail (dev != NULL, TRUE);

	args[0] = dev;
	args[1] = &err;
	nm_wait_for_completion (max_cycles, delay, NULL, nm_device_link_test, args);
	return !err;
}


/*
 * nm_device_set_wireless_config
 *
 * Bring up a wireless card with the essid and wep key of its "best" ap
 *
 * Returns:	TRUE on successful activation
 *			FALSE on unsuccessful activation (ie no best AP)
 *
 */
static gboolean nm_device_set_wireless_config (NMDevice *dev, NMAccessPoint *ap)
{
	NMDeviceAuthMethod	 auth;
	const char		*essid = NULL;

	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);
	g_return_val_if_fail (ap != NULL, FALSE);
	g_return_val_if_fail (nm_ap_get_essid (ap) != NULL, FALSE);
	g_return_val_if_fail (nm_ap_get_auth_method (ap) != NM_DEVICE_AUTH_METHOD_UNKNOWN, FALSE);

	dev->options.wireless.failed_link_count = 0;

	/* Force the card into Managed/Infrastructure mode */
	nm_device_bring_down_wait (dev, 0);
	nm_device_bring_up_wait (dev, 0);

	nm_device_set_mode (dev, NETWORK_MODE_INFRA);

	essid = nm_ap_get_essid (ap);
	auth = nm_ap_get_auth_method (ap);

	nm_device_set_mode (dev, nm_ap_get_mode (ap));
	nm_device_set_bitrate (dev, 0);

	if (nm_ap_get_user_created (ap) || (nm_ap_get_freq (ap) && (nm_ap_get_mode (ap) == NETWORK_MODE_ADHOC)))
		nm_device_set_frequency (dev, nm_ap_get_freq (ap));
	else
		nm_device_set_frequency (dev, 0);	/* auto */

	if (nm_ap_get_encrypted (ap) && nm_ap_is_enc_key_valid (ap))
	{
		char *	hashed_key = nm_ap_get_enc_key_hashed (ap);

		if (auth == NM_DEVICE_AUTH_METHOD_NONE)
		{
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
			nm_warning ("Activation (%s/wireless): AP '%s' said it was encrypted, but had "
					"'none' for authentication method.  Using Open System authentication method.",
					nm_device_get_iface (dev), nm_ap_get_essid (ap));
		}
		nm_device_set_enc_key (dev, hashed_key, auth);
		g_free (hashed_key);
	}
	else
		nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);

	nm_device_set_essid (dev, essid);

	nm_info ("Activation (%s/wireless): using essid '%s', with %s authentication.",
			nm_device_get_iface (dev), essid, (auth == NM_DEVICE_AUTH_METHOD_NONE) ? "no" :
				((auth == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM) ? "Open System" :
				((auth == NM_DEVICE_AUTH_METHOD_SHARED_KEY) ? "Shared Key" : "unknown")));

	/* Bring the device up and pause to allow card to associate.  After we set the ESSID
	 * on the card, the card has to scan all channels to find our requested AP (which can
	 * take a long time if it is an A/B/G chipset like the Atheros 5212, for example).
	 */
	nm_device_is_up_and_associated_wait (dev, 2, 100);

	/* Some cards don't really work well in ad-hoc mode unless you explicitly set the bitrate
	 * on them. (Netgear WG511T/Atheros 5212 with madwifi drivers).  Until we can get rate information
	 * from scanned access points out of iwlib, clamp bitrate for these cards at 11Mbps.
	 */
	if ((nm_ap_get_mode (ap) == NETWORK_MODE_ADHOC) && (nm_device_get_bitrate (dev) <= 0))
		nm_device_set_bitrate (dev, 11000);	/* In Kbps */

	return TRUE;
}


/*
 * nm_device_wireless_configure_adhoc
 *
 * Create an ad-hoc network (rather than associating with one).
 *
 */
static void nm_device_wireless_configure_adhoc (NMActRequest *req)
{
	NMData *			data;
	NMDevice *		dev;
	NMAccessPoint *	ap;
	NMDeviceAuthMethod	auth = NM_DEVICE_AUTH_METHOD_NONE;
	NMAPListIter *		iter;
	NMAccessPoint *	tmp_ap;
	double			card_freqs[IW_MAX_FREQUENCIES];
	int				num_freqs = 0, i;
	double			freq_to_use = 0;
	iwrange			range;
	NMSock *			sk;
	int				err;

	g_return_if_fail (req != NULL);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	ap = nm_act_request_get_ap (req);
	g_assert (ap);

	if (nm_ap_get_encrypted (ap))
		auth = NM_DEVICE_AUTH_METHOD_SHARED_KEY;

	/* Build our local list of frequencies to whittle down until we find a free one */
	memset (&card_freqs, 0, sizeof (card_freqs));
	num_freqs = MIN (dev->options.wireless.num_freqs, IW_MAX_FREQUENCIES);
	for (i = 0; i < num_freqs; i++)
		card_freqs[i] = dev->options.wireless.freqs[i];

	/* We need to find a clear wireless channel to use.  We will
	 * only use 802.11b channels for now.
	 */
	iter = nm_ap_list_iter_new (nm_device_ap_list_get (dev));
	while ((tmp_ap = nm_ap_list_iter_next (iter)))
	{
		double ap_freq = nm_ap_get_freq (tmp_ap);
		for (i = 0; i < num_freqs && ap_freq; i++)
		{
			if (card_freqs[i] == ap_freq)
				card_freqs[i] = 0;
		}
	}
	nm_ap_list_iter_free (iter);

	if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)) == NULL)
	{
		nm_policy_schedule_activation_failed (req);
		return;
	}

	err = iw_get_range_info (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), &range);
	nm_dev_sock_close (sk);
	if (err < 0)
	{
		nm_policy_schedule_activation_failed (req);
		return;
	}

	/* Ok, find the first non-zero freq in our table and use it.
	 * For now we only try to use a channel in the 802.11b channel
	 * space so that most everyone can see it.
	 */
	for (i = 0; i < num_freqs; i++)
	{
		int channel = iw_freq_to_channel (card_freqs[i], &range);
		if (card_freqs[i] && (channel > 0) && (channel < 15))
		{
			freq_to_use = card_freqs[i];
			break;
		}
	}

	/* Hmm, no free channels in 802.11b space.  Pick one more or less randomly */
	if (!freq_to_use)
	{
		double pfreq;
		int	channel = (int)(random () % 14);
		int	err;

		err = iw_channel_to_freq (channel, &pfreq, &range);
		if (err == channel)
			freq_to_use = pfreq;
	}

	if (!freq_to_use)
	{
		nm_policy_schedule_activation_failed (req);
		return;
	}

	nm_ap_set_freq (ap, freq_to_use);

	nm_info ("Will create network '%s' with frequency %f.", nm_ap_get_essid (ap), nm_ap_get_freq (ap));
	nm_device_set_wireless_config (dev, ap);

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		return;
	}

	nm_device_activate_schedule_stage3_ip_config_start (req);
}


static gboolean nm_dwwfl_test (int tries, nm_completion_args args)
{
	NMDevice *	dev = args[0];
	guint *		assoc_count = args[1];
	double *		last_freq = args[2];
	char *		essid = args[3];
	int			required = GPOINTER_TO_INT (args[4]);

	double		cur_freq = nm_device_get_frequency (dev);
	gboolean		assoc = nm_device_wireless_is_associated (dev);
	const char *	cur_essid = nm_device_get_essid (dev);

	/* If we've been cancelled, return that we should stop */
	if (nm_device_activation_should_cancel (dev))
		return TRUE;

	/* If we're on the same frequency and essid, and we're associated,
	 * increment the count for how many iterations we've been associated;
	 * otherwise start over. */
	/* XXX floating point comparison this way is dangerous, IIRC */
	if ((cur_freq == *last_freq) && assoc && !strcmp (essid, cur_essid))
	{
		(*assoc_count)++;
	}
	else
	{
		*assoc_count = 0;
		*last_freq = cur_freq;
	}

	/* If we're told to cancel, return that we're finished.
	 * If the card's frequency has been stable for more than the required
	 * interval, return that we're finished.
	 * Otherwise, we're not finished. */
	if (nm_device_activation_should_cancel (dev) || (*assoc_count >= required))
		return TRUE;

	return FALSE;
}


/*
 * nm_device_wireless_wait_for_link
 *
 * Try to be clever about when the wireless card really has associated with the access point.
 * Return TRUE when we think that it has, and FALSE when we thing it has not associated.
 *
 */
static gboolean nm_device_wireless_wait_for_link (NMDevice *dev, const char *essid)
{
	guint		assoc = 0;
	double		last_freq = 0;
	struct timeval	timeout = { .tv_sec = 0, .tv_usec = 0 };
	nm_completion_args args;

	/* we want to sleep for a very short amount of time, to minimize
	 * hysteresis on the boundaries of our required time.  But we
	 * also want the maximum to be based on what the card */
	const guint	delay = 30;
	const guint	required_tries = 10;
	const guint	min_delay = 2 * (delay / required_tries);

	g_return_val_if_fail (dev != NULL, FALSE);

	/* for cards which don't scan many frequencies, this will return 
	 * 5 seconds, which we'll bump up to 6 seconds below.  Oh well. */
	timeout.tv_sec = (time_t) nm_device_get_association_pause_value (dev);

	/* Refuse to to have a timeout that's _less_ than twice the total time
	 * required before calling a link valid */
	if (timeout.tv_sec < min_delay)
		timeout.tv_sec = min_delay;

	/* We more or less keep asking the driver for the frequency the
	 * card is listening on until it connects to an AP.  Once it's 
	 * associated, the driver stops scanning.  To detect that, we look
	 * for the essid and frequency to remain constant for 3 seconds.
	 * When it remains constant, we assume it's a real link. */
	args[0] = dev;
	args[1] = &assoc;
	args[2] = &last_freq;
	args[3] = (void *)essid;
	args[4] = (void *)(required_tries * 2);
	nm_wait_for_timeout (&timeout, G_USEC_PER_SEC / delay, nm_dwwfl_test, nm_dwwfl_test, args);

	/* If we've had a reasonable association count, we say we have a link */
	if (assoc > required_tries)
		return TRUE;
	return FALSE;
}


static gboolean ap_need_key (NMDevice *dev, NMAccessPoint *ap)
{
	char		*essid;
	gboolean	 need_key = FALSE;

	g_return_val_if_fail (ap != NULL, FALSE);

	essid = nm_ap_get_essid (ap);

	if (!nm_ap_get_encrypted (ap))
	{
		nm_info ("Activation (%s/wireless): access point '%s' is unencrypted, no key needed.", 
			 nm_device_get_iface (dev), essid ? essid : "(null)");
	}
	else
	{
		if (nm_ap_is_enc_key_valid (ap))
		{
			nm_info ("Activation (%s/wireless): access point '%s' "
				 "is encrypted, and a key exists.  No new key needed.",
			  	 nm_device_get_iface (dev), essid ? essid : "(null)");
		}
		else
		{
			nm_info ("Activation (%s/wireless): access point '%s' "
				 "is encrypted, but NO valid key exists.  New key needed.",
				 nm_device_get_iface (dev), 
				 essid ? essid : "(null)");
			need_key = TRUE;
		}
	}

	return need_key;
}


/*
 * nm_device_activate_wireless_configure
 *
 * Configure a wireless device for association with a particular access point.
 *
 */
static void nm_device_wireless_configure (NMActRequest *req)
{
	NMData *			data;
	NMDevice *		dev;
	NMAccessPoint *	ap;
	gboolean			success = FALSE;

	g_return_if_fail (req != NULL);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	ap = nm_act_request_get_ap (req);
	g_assert (ap);

	nm_device_bring_up_wait (dev, 1);

	nm_info ("Activation (%s/wireless) Stage 2 (Device Configure) will connect to access point '%s'.", nm_device_get_iface (dev), nm_ap_get_essid (ap));

	if (ap_need_key (dev, ap))
	{
		nm_dbus_get_user_key_for_network (data->dbus_connection, req, FALSE);
		return;
	}

	while (success == FALSE)
	{
		gboolean	link = FALSE;

		if (nm_device_activation_should_cancel (dev))
			break;

		nm_device_set_wireless_config (dev, ap);

		success = link = nm_device_wireless_wait_for_link (dev, nm_ap_get_essid (ap));

		if (nm_device_activation_should_cancel (dev))
			break;

		if (!link)
		{
			if (nm_ap_get_auth_method (ap) == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
			{
				nm_debug ("Activation (%s/wireless): no hardware link to '%s' in Open System mode, trying Shared Key.",
						nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
				/* Back down to Shared Key mode */
				nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_SHARED_KEY);
				success = FALSE;
				continue;
			}
			else if (nm_ap_get_auth_method (ap) == NM_DEVICE_AUTH_METHOD_SHARED_KEY)
			{
				/* Didn't work in Shared Key either. */
				nm_debug ("Activation (%s/wireless): no hardware link to '%s' in Shared Key mode, need correct key?",
						nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
				nm_dbus_get_user_key_for_network (data->dbus_connection, req, TRUE);
				break;
			}
			else
			{
				nm_debug ("Activation (%s/wireless): no hardware link to '%s' in non-encrypted mode.",
						nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
				nm_policy_schedule_activation_failed (req);
				break;
			}
		}
	}

	if (success)
	{
		nm_info ("Activation (%s/wireless) Stage 2 (Device Configure) successful.  Connected to access point '%s'.", nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
		nm_device_activate_schedule_stage3_ip_config_start (req);
	}
	else if (!nm_device_activation_should_cancel (dev) && (nm_act_request_get_stage (req) != NM_ACT_STAGE_NEED_USER_KEY))
		nm_policy_schedule_activation_failed (req);
}


/*
 * nm_device_wired_configure
 *
 * Configure a wired device for activation
 *
 */
static void nm_device_wired_configure (NMActRequest *req)
{
	NMData *			data;
	NMDevice *		dev;

	g_return_if_fail (req != NULL);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_info ("Activation (%s/wired) Stage 2 (Device Configure) successful.", nm_device_get_iface (dev));
	nm_device_activate_schedule_stage3_ip_config_start (req);
}


/*
 * nm_device_activate_stage2_device_config
 *
 * Determine device parameters and set those on the device, ie
 * for wireless devices, set essid, keys, etc.
 *
 */
static gboolean nm_device_activate_stage2_device_config (NMActRequest *req)
{
	NMDevice *		dev;
	NMData *			data;
	NMAccessPoint *	ap;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	ap = nm_act_request_get_ap (req);
	if (nm_device_is_wireless (dev))
		g_assert (ap);

	nm_info ("Activation (%s) Stage 2 (Device Configure) starting...", nm_device_get_iface (dev));

	/* Bring the device up */
	if (!nm_device_is_up (dev))
		nm_device_bring_up (dev);

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	if (nm_device_is_wireless (dev))
	{
		if (nm_ap_get_user_created (ap))
			nm_device_wireless_configure_adhoc (req);
		else
			nm_device_wireless_configure (req);
	}
	else if (nm_device_is_wired (dev))
		nm_device_wired_configure (req);

	if (nm_device_activation_should_cancel (dev))
		nm_device_schedule_activation_handle_cancel (req);

out:
	nm_info ("Activation (%s) Stage 2 (Device Configure) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage2_device_config
 *
 * Schedule setup of the hardware device
 *
 */
void nm_device_activate_schedule_stage2_device_config (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_DEVICE_CONFIG);

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage2_device_config, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
	nm_info ("Activation (%s) Stage 2 (Device Configure) scheduled...", nm_device_get_iface (dev));
}


/*
 * nm_device_activate_stage3_ip_config_start
 *
 * Begin IP configuration with either DHCP or static IP.
 *
 */
static gboolean nm_device_activate_stage3_ip_config_start (NMActRequest *req)
{
	NMData *			data = NULL;
	NMDevice *		dev = NULL;
	NMAccessPoint *	ap = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_info ("Activation (%s) Stage 3 (IP Configure Start) started...", nm_device_get_iface (dev));

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	if (nm_device_is_wireless (dev))
		ap = nm_act_request_get_ap (req);

	if (!(ap && nm_ap_get_user_created (ap)) && nm_device_get_use_dhcp (dev))
	{
		/* Begin a DHCP transaction on the interface */
		if (!nm_dhcp_manager_begin_transaction (data->dhcp_manager, req))
			nm_policy_schedule_activation_failed (req);
		goto out;
	}

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	/* Static IP and user-created wireless networks skip directly to IP configure stage */
	nm_device_activate_schedule_stage4_ip_config_get (req);

out:
	nm_info ("Activation (%s) Stage 3 (IP Configure Start) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage3_ip_config_start
 *
 * Schedule IP configuration start
 */
static void nm_device_activate_schedule_stage3_ip_config_start (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_START);

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage3_ip_config_start, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
	nm_info ("Activation (%s) Stage 3 (IP Configure Start) scheduled.", nm_device_get_iface (dev));
}


/*
 * nm_device_new_ip4_autoip_config
 *
 * Build up an IP config with a Link Local address
 *
 */
static NMIP4Config *nm_device_new_ip4_autoip_config (NMDevice *dev)
{
	struct in_addr		ip;
	NMIP4Config *		config = NULL;

	g_return_val_if_fail (dev != NULL, NULL);

	if (get_autoip (dev, &ip))
	{
		#define LINKLOCAL_BCAST		0xa9feffff

		config = nm_ip4_config_new ();

		nm_ip4_config_set_address (config, (guint32)(ip.s_addr));
		nm_ip4_config_set_netmask (config, (guint32)(ntohl (0xFFFF0000)));
		nm_ip4_config_set_broadcast (config, (guint32)(ntohl (LINKLOCAL_BCAST)));
		nm_ip4_config_set_gateway (config, 0);
	}

	return config;
}


/*
 * nm_device_activate_stage4_ip_config_get
 *
 * Retrieve the correct IP config.
 *
 */
static gboolean nm_device_activate_stage4_ip_config_get (NMActRequest *req)
{
	NMData *			data = NULL;
	NMDevice *		dev = NULL;
	NMAccessPoint *	ap = NULL;
	NMIP4Config *		ip4_config = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (data);

	if (nm_device_is_wireless (dev))
	{
		ap = nm_act_request_get_ap (req);
		g_assert (ap);
	}

	nm_info ("Activation (%s) Stage 4 (IP Configure Get) started...", nm_device_get_iface (dev));

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	if (ap && nm_ap_get_user_created (ap))
		ip4_config = nm_device_new_ip4_autoip_config (dev);
	else if (nm_device_get_use_dhcp (dev))
		ip4_config = nm_dhcp_manager_get_ip4_config (data->dhcp_manager, req);
	else
		ip4_config = nm_system_device_new_ip4_system_config (dev);

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	if (ip4_config)
	{
		nm_act_request_set_ip4_config (req, ip4_config);
		nm_device_activate_schedule_stage5_ip_config_commit (req);
	}
	else
	{
		/* Interfaces cannot be down if they are the active interface,
		 * otherwise we cannot use them for scanning or link detection.
		 */
		if (nm_device_is_wireless (dev))
		{
			nm_device_set_essid (dev, "");
			nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);
		}

		if (!nm_device_is_up (dev))
			nm_device_bring_up (dev);

		nm_policy_schedule_activation_failed (req);
	}

out:
	nm_info ("Activation (%s) Stage 4 (IP Configure Get) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage4_ip_config_get
 *
 * Schedule creation of the IP config
 *
 */
void nm_device_activate_schedule_stage4_ip_config_get (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_GET);
	nm_info ("Activation (%s) Stage 4 (IP Configure Get) scheduled...", nm_device_get_iface (dev));

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage4_ip_config_get, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
}


/*
 * nm_device_activate_stage4_ip_config_timeout
 *
 * Retrieve the correct IP config.
 *
 */
static gboolean nm_device_activate_stage4_ip_config_timeout (NMActRequest *req)
{
	NMData *			data = NULL;
	NMDevice *		dev = NULL;
	NMIP4Config *		ip4_config = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_info ("Activation (%s) Stage 4 (IP Configure Timeout) started...", nm_device_get_iface (dev));

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	if (nm_device_is_wired (dev))
	{
		/* Wired network, no DHCP reply.  Let's get an IP via Zeroconf. */
		nm_info ("No DHCP reply received.  Automatically obtaining IP via Zeroconf.");
		ip4_config = nm_device_new_ip4_autoip_config (dev);
	}
	else if (nm_device_is_wireless (dev))
	{
		NMAccessPoint *ap = nm_act_request_get_ap (req);

		g_assert (ap);

		/* For those broken cards that report successful hardware link even when WEP key is wrong,
		 * and also for Open System mode (where you cannot know WEP key is wrong ever), we try to
		 * do DHCP and if that fails, fall back to next auth mode and try again.
		 */
		if (nm_ap_get_auth_method (ap) == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
		{
			/* Back down to Shared Key mode */
			nm_debug ("Activation (%s/wireless): could not get IP configuration info for '%s' in Open System mode, trying Shared Key.",
					nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_SHARED_KEY);
			nm_device_activate_schedule_stage2_device_config (req);
		}
		else if ((nm_ap_get_auth_method (ap) == NM_DEVICE_AUTH_METHOD_SHARED_KEY))
		{
			/* Shared Key mode failed, we must have bad WEP key */
			nm_debug ("Activation (%s/wireless): could not get IP configuration info for '%s' in Shared Key mode, asking for new key.",
					nm_device_get_iface (dev), nm_ap_get_essid (ap) ? nm_ap_get_essid (ap) : "(none)");
			nm_dbus_get_user_key_for_network (data->dbus_connection, req, TRUE);
		}
		else
		{
			/*
			 * Wireless, not encrypted, no DHCP Reply.  Try Zeroconf.  We do not do this in
			 * the encrypted case, because the problem could be (and more likely is) a bad key.
			 */
			nm_info ("No DHCP reply received.  Automatically obtaining IP via Zeroconf.");
			ip4_config = nm_device_new_ip4_autoip_config (dev);
		}
	}

	if (ip4_config)
	{
		nm_act_request_set_ip4_config (req, ip4_config);
		nm_device_activate_schedule_stage5_ip_config_commit (req);
	}

out:
	nm_info ("Activation (%s) Stage 4 (IP Configure Timeout) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage4_ip_config_timeout
 *
 * Deal with a timed out DHCP transaction
 *
 */
void nm_device_activate_schedule_stage4_ip_config_timeout (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_GET);

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage4_ip_config_timeout, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
	nm_info ("Activation (%s) Stage 4 (IP Configure Timeout) scheduled...", nm_device_get_iface (dev));
}


/*
 * nm_device_activate_stage5_ip_config_commit
 *
 * Commit the IP config on the device
 *
 */
static gboolean nm_device_activate_stage5_ip_config_commit (NMActRequest *req)
{
	NMData *			data = NULL;
	NMDevice *		dev = NULL;
	NMIP4Config *		ip4_config = NULL;

	g_return_val_if_fail (req != NULL, FALSE);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	ip4_config = nm_act_request_get_ip4_config (req);
	g_assert (ip4_config);

	nm_info ("Activation (%s) Stage 5 (IP Configure Commit) started...", nm_device_get_iface (dev));

	if (nm_device_activation_should_cancel (dev))
	{
		nm_device_schedule_activation_handle_cancel (req);
		goto out;
	}

	nm_device_set_ip4_config (dev, ip4_config);
	if (nm_system_device_set_from_ip4_config (dev))
	{
		nm_device_update_ip4_address (dev);
		nm_system_device_add_ip6_link_address (dev);
		nm_system_restart_mdns_responder ();
		nm_policy_schedule_activation_finish (req);
		nm_device_set_link_active (dev, nm_device_probe_link_state (dev));
	}
	else
		nm_policy_schedule_activation_failed (req);

out:
	nm_info ("Activation (%s) Stage 5 (IP Configure Commit) complete.", nm_device_get_iface (dev));
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage5_ip_config_commit
 *
 * Schedule commit of the IP config
 */
static void nm_device_activate_schedule_stage5_ip_config_commit (NMActRequest *req)
{
	GSource *		source = NULL;
	NMDevice *	dev = NULL;

	g_return_if_fail (req != NULL);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	nm_act_request_set_stage (req, NM_ACT_STAGE_IP_CONFIG_COMMIT);

	source = g_idle_source_new ();
	g_source_set_callback (source, (GSourceFunc) nm_device_activate_stage5_ip_config_commit, req, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
	nm_info ("Activation (%s) Stage 5 (IP Configure Commit) scheduled...", nm_device_get_iface (dev));
}


/*
 * nm_device_is_activating
 *
 * Return whether or not the device is currently activating itself.
 *
 */
gboolean nm_device_is_activating (NMDevice *dev)
{
	NMActRequest *	req;
	NMActStage	stage;
	gboolean		activating = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (!(req = nm_device_get_act_request (dev)))
		return FALSE;

	stage = nm_act_request_get_stage (req);
	switch (stage)
	{
		case NM_ACT_STAGE_DEVICE_PREPARE:
		case NM_ACT_STAGE_DEVICE_CONFIG:
		case NM_ACT_STAGE_NEED_USER_KEY:
		case NM_ACT_STAGE_IP_CONFIG_START:
		case NM_ACT_STAGE_IP_CONFIG_GET:
		case NM_ACT_STAGE_IP_CONFIG_COMMIT:
			activating = TRUE;
			break;

		case NM_ACT_STAGE_ACTIVATED:
		case NM_ACT_STAGE_FAILED:
		case NM_ACT_STAGE_CANCELLED:
		case NM_ACT_STAGE_UNKNOWN:
		default:
			break;
	}

	return activating;
}


/*
 * nm_device_is_activated
 *
 * Return whether or not the device is successfully activated.
 *
 */
static gboolean nm_device_is_activated (NMDevice *dev)
{
	NMActRequest *	req;
	NMActStage	stage;
	gboolean		activated = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (!(req = nm_device_get_act_request (dev)))
		return FALSE;

	stage = nm_act_request_get_stage (req);
	switch (stage)
	{
		case NM_ACT_STAGE_ACTIVATED:
			activated = TRUE;
			break;

		case NM_ACT_STAGE_DEVICE_PREPARE:
		case NM_ACT_STAGE_DEVICE_CONFIG:
		case NM_ACT_STAGE_NEED_USER_KEY:
		case NM_ACT_STAGE_IP_CONFIG_START:
		case NM_ACT_STAGE_IP_CONFIG_GET:
		case NM_ACT_STAGE_IP_CONFIG_COMMIT:
		case NM_ACT_STAGE_FAILED:
		case NM_ACT_STAGE_CANCELLED:
		case NM_ACT_STAGE_UNKNOWN:
		default:
			break;
	}

	return activated;
}


/*
 * nm_device_activation_should_cancel
 *
 * Return whether or not we've been told to cancel activation
 *
 */
gboolean nm_device_activation_should_cancel (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->quit_activation);
}


static gboolean nm_ac_test (int tries, nm_completion_args args)
{
	NMDevice *dev = args[0];

	g_return_val_if_fail (dev != NULL, TRUE);

	if (nm_device_is_activating (dev))
	{
		if (tries % 20 == 0)
			nm_info ("Activation (%s): waiting for device to cancel activation.", nm_device_get_iface(dev));
		return FALSE;
	}

	return TRUE;
}


/*
 * nm_device_activation_cancel
 *
 * Signal activation worker that it should stop and die.
 *
 */
void nm_device_activation_cancel (NMDevice *dev)
{
	nm_completion_args	args;

	g_return_if_fail (dev != NULL);
	g_assert (dev->app_data);

	if (nm_device_is_activating (dev))
	{
		NMActRequest *	req = nm_device_get_act_request (dev);
		gboolean		clear_act_request = FALSE;

		nm_info ("Activation (%s): cancelling...", nm_device_get_iface (dev));
		dev->quit_activation = TRUE;

		/* If the device is waiting for DHCP or a user key, force its current request to stop. */
		if (nm_act_request_get_stage (req) == NM_ACT_STAGE_NEED_USER_KEY)
		{
			nm_dbus_cancel_get_user_key_for_network (dev->app_data->dbus_connection, req);
			clear_act_request = TRUE;
		}
		else if (nm_act_request_get_stage (req) == NM_ACT_STAGE_IP_CONFIG_START)
		{
			nm_dhcp_manager_cancel_transaction (dev->app_data->dhcp_manager, req);
			clear_act_request = TRUE;
		}

		if (clear_act_request)
		{
			dev->act_request = NULL;
			nm_act_request_unref (req);
		}

		/* Spin until cancelled.  Possible race conditions or deadlocks here.
		 * The other problem with waiting here is that we hold up dbus traffic
		 * that we should respond to.
		 */
		args[0] = dev;
		nm_wait_for_completion (NM_COMPLETION_TRIES_INFINITY, G_USEC_PER_SEC / 20, nm_ac_test, NULL, args);
		nm_info ("Activation (%s): cancelled.", nm_device_get_iface(dev));
		nm_schedule_state_change_signal_broadcast (dev->app_data);
		dev->quit_activation = FALSE;
	}
}


/*
 * nm_device_deactivate_quickly
 *
 * Quickly deactivate a device, for things like sleep, etc.  Doesn't
 * clean much stuff up, and nm_device_deactivate() should be called
 * on the device eventually.
 *
 */
gboolean nm_device_deactivate_quickly (NMDevice *dev)
{
	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	nm_vpn_manager_deactivate_vpn_connection (dev->app_data->vpn_manager, dev);

	if (nm_device_is_activated (dev))
		nm_dbus_schedule_device_status_change_signal (dev->app_data, dev, NULL, DEVICE_NO_LONGER_ACTIVE);
	else if (nm_device_is_activating (dev))
		nm_device_activation_cancel (dev);

	/* Tear down an existing activation request, which may not have happened
	 * in nm_device_activation_cancel() above, for various reasons.
	 */
	if (dev->act_request)
	{
 		nm_dhcp_manager_cancel_transaction (dev->app_data->dhcp_manager, dev->act_request);
		nm_act_request_unref (dev->act_request);
		dev->act_request = NULL;
	}

	return TRUE;
}


/*
 * nm_device_deactivate
 *
 * Remove a device's routing table entries and IP address.
 *
 */
gboolean nm_device_deactivate (NMDevice *dev)
{
	NMIP4Config *	config;

	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	nm_info ("Deactivating device %s.", nm_device_get_iface (dev));

	nm_device_deactivate_quickly (dev);

	if (!(nm_device_get_capabilities (dev) & NM_DEVICE_CAP_NM_SUPPORTED))
		return TRUE;

	/* Remove any device nameservers and domains */
	if ((config = nm_device_get_ip4_config (dev)))
	{
		nm_named_manager_remove_ip4_config (dev->app_data->named_manager, config);
		nm_device_set_ip4_config (dev, NULL);
	}

	/* Take out any entries in the routing table and any IP address the device had. */
	nm_system_device_flush_routes (dev);
	nm_system_device_flush_addresses (dev);
	nm_device_update_ip4_address (dev);

	/* Clean up stuff, don't leave the card associated */
	if (nm_device_is_wireless (dev))
	{
		nm_device_set_essid (dev, "");
		nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);
		nm_device_set_mode (dev, NETWORK_MODE_INFRA);
		nm_wireless_set_scan_interval (dev->app_data, dev, NM_WIRELESS_SCAN_INTERVAL_ACTIVE);
	}

	nm_schedule_state_change_signal_broadcast (dev->app_data);

	return TRUE;
}


/*
 * nm_device_set_user_key_for_network
 *
 * Called upon receipt of a NetworkManagerInfo reply with a
 * user-supplied key.
 *
 */
void nm_device_set_user_key_for_network (NMActRequest *req, const char *key, const NMEncKeyType enc_type)
{
	NMData *			data;
	NMDevice *		dev;
	NMAccessPoint *	ap;
	const char *		cancel_message = "***canceled***";

	g_return_if_fail (key != NULL);

	data = nm_act_request_get_data (req);
	g_assert (data);

	dev = nm_act_request_get_dev (req);
	g_assert (dev);

	ap = nm_act_request_get_ap (req);
	g_assert (ap);

	/* If the user canceled, mark the ap as invalid */
	if (strncmp (key, cancel_message, strlen (cancel_message)) == 0)
	{
		nm_ap_list_append_ap (data->invalid_ap_list, ap);
		nm_device_deactivate (dev);
		nm_policy_schedule_device_change_check (data);
	}
	else
	{
		NMAccessPoint * allowed_ap;

		/* Start off at Open System auth mode with the new key */
		nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
		nm_ap_set_enc_key_source (ap, key, enc_type);

		/* Be sure to update NMI with the new auth mode */
		if ((allowed_ap = nm_ap_list_get_ap_by_essid (data->allowed_ap_list, nm_ap_get_essid (ap))))
			nm_ap_set_auth_method (allowed_ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);

		nm_device_activate_schedule_stage1_device_prepare (req);
	}
}


/*
 * nm_device_ap_list_add_ap
 *
 * Add an access point to the devices internal AP list.
 *
 */
static void nm_device_ap_list_add_ap (NMDevice *dev, NMAccessPoint *ap)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (ap  != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	nm_ap_list_append_ap (dev->options.wireless.ap_list, ap);
	/* Transfer ownership of ap to the list by unrefing it here */
	nm_ap_unref (ap);
}


/*
 * nm_device_ap_list_clear
 *
 * Clears out the device's internal list of available access points.
 *
 */
void	nm_device_ap_list_clear (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	if (!dev->options.wireless.ap_list)
		return;

	nm_ap_list_unref (dev->options.wireless.ap_list);
	dev->options.wireless.ap_list = NULL;
}


/*
 * nm_device_ap_list_get_ap_by_essid
 *
 * Get the access point for a specific essid
 *
 */
NMAccessPoint *nm_device_ap_list_get_ap_by_essid (NMDevice *dev, const char *essid)
{
	NMAccessPoint	*ret_ap = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (essid != NULL, NULL);

	if (!dev->options.wireless.ap_list)
		return (NULL);

	ret_ap = nm_ap_list_get_ap_by_essid (dev->options.wireless.ap_list, essid);

	return (ret_ap);
}


/*
 * nm_device_ap_list_get_ap_by_address
 *
 * Get the access point for a specific MAC address
 *
 */
NMAccessPoint *nm_device_ap_list_get_ap_by_address (NMDevice *dev, const struct ether_addr *addr)
{
	NMAccessPoint	*ret_ap = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (addr != NULL, NULL);

	if (!dev->options.wireless.ap_list)
		return (NULL);

	ret_ap = nm_ap_list_get_ap_by_address (dev->options.wireless.ap_list, addr);

	return (ret_ap);
}


/*
 * nm_device_ap_list_get_ap_by_obj_path
 *
 * Get the access point for a dbus object path.  Requires an _unescaped_
 * object path.
 *
 */
NMAccessPoint *nm_device_ap_list_get_ap_by_obj_path (NMDevice *dev, const char *obj_path)
{
	NMAccessPoint *	ret_ap = NULL;
	char *			built_path;
	char *			dev_path;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (obj_path != NULL, NULL);

	if (!dev->options.wireless.ap_list)
		return (NULL);

	dev_path = nm_dbus_get_object_path_for_device (dev);
	dev_path = nm_dbus_unescape_object_path (dev_path);
	built_path = g_strdup_printf ("%s/Networks/", dev_path);
	g_free (dev_path);

	if (strncmp (built_path, obj_path, strlen (built_path)) == 0)
	{
		char *essid = g_strdup (obj_path + strlen (built_path));

		ret_ap = nm_ap_list_get_ap_by_essid (dev->options.wireless.ap_list, essid);
		g_free (essid);
	}
	g_free (built_path);

	return (ret_ap);
}


/*
 * nm_device_ap_list_get
 *
 * Return a pointer to the AP list
 *
 */
NMAccessPointList *nm_device_ap_list_get (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);

	return (dev->options.wireless.ap_list);
}


static gboolean link_to_specific_ap (NMDevice *dev, NMAccessPoint *ap, gboolean default_link)
{
	gboolean link = FALSE;

	/* Checking hardware's ESSID during a scan is doesn't work. */
	nm_lock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);

	if (nm_device_wireless_is_associated (dev))
	{
		char *	dev_essid = nm_device_get_essid (dev);
		char *	ap_essid = nm_ap_get_essid (ap);

		if (dev_essid && ap_essid && !strcmp (dev_essid, ap_essid))
		{
			dev->options.wireless.failed_link_count = 0;
			link = TRUE;
		}
	}

	nm_unlock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);

	if (!link)
	{
		dev->options.wireless.failed_link_count++;
		if (dev->options.wireless.failed_link_count < 3)
			link = default_link;
	}

	return link;
}

/*
 * nm_device_update_best_ap
 *
 * Recalculate the "best" access point we should be associating with.
 *
 */
NMAccessPoint * nm_device_get_best_ap (NMDevice *dev)
{
	NMAccessPointList *	ap_list;
	NMAPListIter *		iter;
	NMAccessPoint *	scan_ap = NULL;
	NMAccessPoint *	best_ap = NULL;
	NMAccessPoint *	cur_ap = NULL;
	NMActRequest *		req = NULL;
	NMAccessPoint *	trusted_best_ap = NULL;
	NMAccessPoint *	untrusted_best_ap = NULL;
	GTimeVal			trusted_latest_timestamp = {0, 0};
	GTimeVal		 	untrusted_latest_timestamp = {0, 0};

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_assert (dev->app_data);

	/* Devices that can't scan don't do anything automatic.
	 * The user must choose the access point from the menu.
	 */
	if (!nm_device_get_supports_wireless_scan (dev) && !nm_device_has_active_link (dev))
		return NULL;

	if (!(ap_list = nm_device_ap_list_get (dev)))
		return NULL;

	/* We prefer the currently selected access point if its user-chosen or if there
	 * is still a hardware link to it.
	 */
	if ((req = nm_device_get_act_request (dev)))
	{
		if ((cur_ap = nm_act_request_get_ap (req)))
		{
			char *	essid = nm_ap_get_essid (cur_ap);
			gboolean	keep = FALSE;

			if (nm_ap_get_user_created (cur_ap))
				keep = TRUE;
			else if (nm_act_request_get_user_requested (req))
				keep = TRUE;
			else if (link_to_specific_ap (dev, cur_ap, TRUE))
				keep = TRUE;

			/* Only keep if its not in the invalid list and its _is_ in our scaned list */
			if ( keep
				&& !nm_ap_list_get_ap_by_essid (dev->app_data->invalid_ap_list, essid)
				&& nm_device_ap_list_get_ap_by_essid (dev, essid))
			{
				nm_ap_ref (cur_ap);
				return cur_ap;
			}
		}
	}

	if (!(iter = nm_ap_list_iter_new (ap_list)))
		return NULL;
	while ((scan_ap = nm_ap_list_iter_next (iter)))
	{
		NMAccessPoint	*tmp_ap;
		char			*ap_essid = nm_ap_get_essid (scan_ap);

		/* Access points in the "invalid" list cannot be used */
		if (nm_ap_list_get_ap_by_essid (dev->app_data->invalid_ap_list, ap_essid))
			continue;

		if ((tmp_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, ap_essid)))
		{
			const GTimeVal *curtime = nm_ap_get_timestamp (tmp_ap);

			gboolean blacklisted = nm_ap_has_manufacturer_default_essid (scan_ap);
			if (blacklisted)
			{
				GSList *elt, *user_addrs;
				const struct ether_addr *ap_addr;
				char char_addr[20];

				ap_addr = nm_ap_get_address (scan_ap);
				user_addrs = nm_ap_get_user_addresses (tmp_ap);

				memset (&char_addr[0], 0, 20);
				ether_ntoa_r (ap_addr, &char_addr[0]);

				for (elt = user_addrs; elt; elt = g_slist_next (elt))
				{
					if (elt->data && !strcmp (elt->data, &char_addr[0]))
					{
						blacklisted = FALSE;
						break;
					}
				}

				g_slist_foreach (user_addrs, (GFunc)g_free, NULL);
				g_slist_free (user_addrs);
			}

			if (!blacklisted && nm_ap_get_trusted (tmp_ap) && (curtime->tv_sec > trusted_latest_timestamp.tv_sec))
			{
				trusted_latest_timestamp = *nm_ap_get_timestamp (tmp_ap);
				trusted_best_ap = scan_ap;
				/* Merge access point data (mainly to get updated WEP key) */
				nm_ap_set_enc_key_source (trusted_best_ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
			}
			else if (!blacklisted && !nm_ap_get_trusted (tmp_ap) && (curtime->tv_sec > untrusted_latest_timestamp.tv_sec))
			{
				untrusted_latest_timestamp = *nm_ap_get_timestamp (tmp_ap);
				untrusted_best_ap = scan_ap;
				/* Merge access point data (mainly to get updated WEP key) */
				nm_ap_set_enc_key_source (untrusted_best_ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
			}
		}
	}
	best_ap = trusted_best_ap ? trusted_best_ap : untrusted_best_ap;
	nm_ap_list_iter_free (iter);

	if (best_ap)
		nm_ap_ref (best_ap);

	return best_ap;
}


/*
 * nm_device_wireless_get_activation_ap
 *
 * Return an access point suitable for use in the device activation
 * request.
 *
 */
NMAccessPoint * nm_device_wireless_get_activation_ap (NMDevice *dev, const char *essid, const char *key, NMEncKeyType key_type)
{
	gboolean			 encrypted = FALSE;
	NMAccessPoint		*ap = NULL;
	NMAccessPoint		*tmp_ap = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (dev->app_data != NULL, NULL);
	g_return_val_if_fail (essid != NULL, NULL);

	nm_debug ("Forcing AP '%s'", essid);

	if (    key
		&& strlen (key)
		&& (key_type != NM_ENC_TYPE_UNKNOWN)
		&& (key_type != NM_ENC_TYPE_NONE))
		encrypted = TRUE;

	/* Find the AP in our card's scan list first.
	 * If its not there, create an entirely new AP.
	 */
	if (!(ap = nm_ap_list_get_ap_by_essid (nm_device_ap_list_get (dev), essid)))
	{
		/* Okay, the card didn't see it in the scan, Cisco cards sometimes do this.
		 * So we make a "fake" access point and add it to the scan list.
		 */
		ap = nm_ap_new ();
		nm_ap_set_essid (ap, essid);
		nm_ap_set_encrypted (ap, encrypted);		
		if (encrypted)
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
		else
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_NONE);
		nm_ap_set_artificial (ap, TRUE);
		nm_ap_list_append_ap (nm_device_ap_list_get (dev), ap);
		nm_ap_unref (ap);
	}
	else
	{
		/* If the AP is in the ignore list, we have to remove it since
		 * the User Knows What's Best.
		 */
		nm_ap_list_remove_ap_by_essid (dev->app_data->invalid_ap_list, nm_ap_get_essid (ap));
	}

	/* Now that this AP has an essid, copy over encryption keys and whatnot */
	if ((tmp_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, nm_ap_get_essid (ap))))
	{
		nm_ap_set_enc_key_source (ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
		nm_ap_set_auth_method (ap, nm_ap_get_auth_method (tmp_ap));
		nm_ap_set_invalid (ap, nm_ap_get_invalid (tmp_ap));
		nm_ap_set_timestamp (ap, nm_ap_get_timestamp (tmp_ap));
	}

	/* Use the encryption key and type the user sent us if its valid */
	if (encrypted)
		nm_ap_set_enc_key_source (ap, key, key_type);

	return ap;
}


/*
 * nm_device_fake_ap_list
 *
 * Fake the access point list, used for test devices.
 *
 */
static void nm_device_fake_ap_list (NMDevice *dev)
{
	#define NUM_FAKE_APS	4

	int				i;
	NMAccessPointList *	old_ap_list = nm_device_ap_list_get (dev);

	const char		*fake_essids[NUM_FAKE_APS] = { "green", "bay", "packers", "rule" };
	struct ether_addr	 fake_addrs[NUM_FAKE_APS] =  {{{0x70, 0x37, 0x03, 0x70, 0x37, 0x03}},
											{{0x12, 0x34, 0x56, 0x78, 0x90, 0xab}},
											{{0xcd, 0xef, 0x12, 0x34, 0x56, 0x78}},
											{{0x90, 0xab, 0xcd, 0xef, 0x12, 0x34}} };
	guint8			 fake_qualities[NUM_FAKE_APS] = { 150, 26, 200, 100 };
	double			 fake_freqs[NUM_FAKE_APS] = { 3.1416, 4.1416, 5.1415, 6.1415 };
	gboolean			 fake_enc[NUM_FAKE_APS] = { FALSE, TRUE, FALSE, TRUE };

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);

	dev->options.wireless.ap_list = nm_ap_list_new (NETWORK_TYPE_DEVICE);

	for (i = 0; i < NUM_FAKE_APS; i++)
	{
		NMAccessPoint		*nm_ap  = nm_ap_new ();
		NMAccessPoint		*list_ap;

		/* Copy over info from scan to local structure */
		nm_ap_set_essid (nm_ap, fake_essids[i]);

		if (fake_enc[i])
			nm_ap_set_encrypted (nm_ap, FALSE);
		else
			nm_ap_set_encrypted (nm_ap, TRUE);

		nm_ap_set_address (nm_ap, (const struct ether_addr *)(&fake_addrs[i]));
		nm_ap_set_strength (nm_ap, fake_qualities[i]);
		nm_ap_set_freq (nm_ap, fake_freqs[i]);

		/* Merge settings from wireless networks, mainly keys */
		if ((list_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, nm_ap_get_essid (nm_ap))))
		{
			nm_ap_set_timestamp (nm_ap, nm_ap_get_timestamp (list_ap));
			nm_ap_set_enc_key_source (nm_ap, nm_ap_get_enc_key_source (list_ap), nm_ap_get_enc_type (list_ap));
		}

		/* Add the AP to the device's AP list */
		nm_device_ap_list_add_ap (dev, nm_ap);
	}

	if (nm_device_get_act_request (dev))
		nm_ap_list_diff (dev->app_data, dev, old_ap_list, nm_device_ap_list_get (dev));
	if (old_ap_list)
		nm_ap_list_unref (old_ap_list);
}


/*
 * nm_device_wireless_schedule_scan
 *
 * Schedule a wireless scan in the /device's/ thread.
 *
 */
static void nm_device_wireless_schedule_scan (NMDevice *dev)
{
	GSource			*wscan_source;
	guint			 wscan_source_id;
	NMWirelessScanCB	*scan_cb;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	scan_cb = g_malloc0 (sizeof (NMWirelessScanCB));
	scan_cb->dev = dev;
	scan_cb->force = FALSE;

	wscan_source = g_timeout_source_new (dev->options.wireless.scan_interval * 1000);
	g_source_set_callback (wscan_source, nm_device_wireless_scan, scan_cb, NULL);
	wscan_source_id = g_source_attach (wscan_source, dev->context);
	g_source_unref (wscan_source);
}


/*
 * nm_device_wireless_process_scan_results
 *
 * Process results of an iwscan() into our own AP lists.  We're an idle function,
 * but we never reschedule ourselves.
 *
 */
static gboolean nm_device_wireless_process_scan_results (gpointer user_data)
{
	NMWirelessScanResults *	cb_data = (NMWirelessScanResults *)user_data;
	NMDevice *			dev;
	GTimeVal				cur_time;
	NMAPListIter *			iter = NULL;

	g_return_val_if_fail (cb_data != NULL, FALSE);	

	dev = cb_data->dev;
	if (!dev || !cb_data->results)
		return FALSE;

	if (!process_scan_results (dev, cb_data->results, cb_data->results_len))
		nm_warning ("nm_device_wireless_process_scan_results(%s): process_scan_results() returned an error.", nm_device_get_iface (dev));

	/* Once we have the list, copy in any relevant information from our Allowed list. */
	nm_ap_list_copy_properties (nm_device_ap_list_get (dev), dev->app_data->allowed_ap_list);

	/* Walk the access point list and remove any access points older than 180s */
	g_get_current_time (&cur_time);
	if (nm_device_ap_list_get (dev) && (iter = nm_ap_list_iter_new (nm_device_ap_list_get (dev))))
	{
		NMAccessPoint *outdated_ap;
		GSList *		outdated_list = NULL;
		GSList *		elt;
		NMActRequest *	req = nm_device_get_act_request (dev);
		NMAccessPoint *cur_ap = NULL;

		if (req)
		{
			cur_ap = nm_act_request_get_ap (req);
			g_assert (cur_ap);
		}

		while ((outdated_ap = nm_ap_list_iter_next (iter)))
		{
			const GTimeVal	*ap_time = nm_ap_get_last_seen (outdated_ap);
			gboolean		 keep_around = FALSE;

			/* Don't ever prune the AP we're currently associated with */
			if (	    nm_ap_get_essid (outdated_ap)
				&&  (cur_ap && (nm_null_safe_strcmp (nm_ap_get_essid (cur_ap), nm_ap_get_essid (outdated_ap))) == 0))
				keep_around = TRUE;

			if (!keep_around && (ap_time->tv_sec + 180 < cur_time.tv_sec))
				outdated_list = g_slist_append (outdated_list, outdated_ap);
		}
		nm_ap_list_iter_free (iter);

		/* Ok, now remove outdated ones.  We have to do it after the lock
		 * because nm_ap_list_remove_ap() locks the list too.
		 */
		for (elt = outdated_list; elt; elt = g_slist_next (elt))
		{
			if ((outdated_ap = (NMAccessPoint *)(elt->data)))
			{
				nm_dbus_signal_wireless_network_change	(dev->app_data->dbus_connection, dev, outdated_ap, NETWORK_STATUS_DISAPPEARED, -1);
				nm_ap_list_remove_ap (nm_device_ap_list_get (dev), outdated_ap);
			}
		}
		g_slist_free (outdated_list);
	}

	nm_policy_schedule_device_change_check (dev->app_data);

	return FALSE;
}


/*
 * nm_device_wireless_scan
 *
 * Get a list of access points this device can see.
 *
 */
static gboolean nm_device_wireless_scan (gpointer user_data)
{
	NMWirelessScanCB *		scan_cb = (NMWirelessScanCB *)(user_data);
	NMDevice *			dev = NULL;
	NMWirelessScanResults *	scan_results = NULL;
	guint32				caps;
	guint8 * 				results = NULL;
	guint32				results_len = 0;

	g_return_val_if_fail (scan_cb != NULL, FALSE);

	dev = scan_cb->dev;
	if (!dev || !dev->app_data)
	{
		g_free (scan_cb);
		return FALSE;
	}

	caps = nm_device_get_capabilities (dev);
	if (!(caps & NM_DEVICE_CAP_NM_SUPPORTED) || !(caps & NM_DEVICE_CAP_WIRELESS_SCAN))
	{
		g_free (scan_cb);
		return FALSE;
	}

	/* Reschedule ourselves if all wireless is disabled, we're asleep,
	 * or we are currently activating.
	 */
	if (    (dev->app_data->wireless_enabled == FALSE)
		|| (dev->app_data->asleep == TRUE)
		|| (nm_device_is_activating (dev) == TRUE))
	{
		nm_wireless_set_scan_interval (dev->app_data, dev, NM_WIRELESS_SCAN_INTERVAL_INIT);
		goto reschedule;
	}

	/*
	 * A/B/G cards should only scan if they are disconnected.  Set the timeout to active
	 * for the case we lose this connection shortly, it will reach this point and then
	 * nm_device_is_activated will return FALSE, letting the scan proceed.
	 */
	if (dev->options.wireless.num_freqs > 14 && nm_device_is_activated (dev) == TRUE)
	{
		nm_wireless_set_scan_interval (dev->app_data, dev, NM_WIRELESS_SCAN_INTERVAL_ACTIVE);
		goto reschedule;
	}

	/* Test devices get a fake ap list */
	if (dev->test_device)
	{
		nm_device_fake_ap_list (dev);
		nm_wireless_set_scan_interval (dev->app_data, dev, NM_WIRELESS_SCAN_INTERVAL_ACTIVE);
		goto reschedule;
	}

	/* Grab the scan mutex */
	if (nm_try_acquire_mutex (dev->options.wireless.scan_mutex, __FUNCTION__))
	{
		NMSock	*sk;
		gboolean	 devup_err;

		/* Device must be up before we can scan */
		devup_err = nm_device_bring_up_wait(dev, 1);
		if (devup_err)
		{
			nm_unlock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);
			goto reschedule;
		}

		if ((sk = nm_dev_sock_open (dev, DEV_WIRELESS, __FUNCTION__, NULL)))
		{
			int			err;
			NMNetworkMode	orig_mode = NETWORK_MODE_INFRA;
			double		orig_freq = 0;
			int			orig_rate = 0;
			const int		interval = 20;
			struct iwreq	wrq;

			orig_mode = nm_device_get_mode (dev);
			if (orig_mode == NETWORK_MODE_ADHOC)
			{
				orig_freq = nm_device_get_frequency (dev);
				orig_rate = nm_device_get_bitrate (dev);
			}

			/* Must be in infrastructure mode during scan, otherwise we don't get a full
			 * list of scan results.  Scanning doesn't work well in Ad-Hoc mode :( 
			 */
			nm_device_set_mode (dev, NETWORK_MODE_INFRA);
			nm_device_set_frequency (dev, 0);

			wrq.u.data.pointer = NULL;
			wrq.u.data.flags = 0;
			wrq.u.data.length = 0;
			if (iw_set_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCSIWSCAN, &wrq) < 0)
			{
				nm_warning ("nm_device_wireless_scan(%s): couldn't trigger wireless scan.  errno = %d",
					nm_device_get_iface (dev), errno);
			}
			else
			{
				/* Initial pause for card to return data */
				g_usleep (G_USEC_PER_SEC / 4);

				if ((results = get_scan_results (dev, sk, &results_len)))
				{
					scan_results = g_malloc0 (sizeof (NMWirelessScanResults));
					nm_device_ref (dev);
					scan_results->dev = dev;
					scan_results->results = results;
					scan_results->results_len = results_len;
				}
				else
					nm_warning ("nm_device_wireless_scan(%s): get_scan_results() returned an error.", nm_device_get_iface (dev));
			}

			nm_device_set_mode (dev, orig_mode);
			/* Only set frequency if ad-hoc mode */
			if (orig_mode == NETWORK_MODE_ADHOC)
			{
				nm_device_set_frequency (dev, orig_freq);
				nm_device_set_bitrate (dev, orig_rate);
			}

			nm_dev_sock_close (sk);
		}
		nm_unlock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);
	}

	/* We run the scan processing function from the main thread, since it must deliver
	 * messages over DBUS.  Plus, that way the main thread is the only thread that has
	 * to modify the device's access point list.
	 */
	if (scan_results != NULL)
	{
		guint	scan_process_source_id = 0;
		GSource *	scan_process_source = g_idle_source_new ();
		GTimeVal	cur_time;

		g_source_set_callback (scan_process_source, nm_device_wireless_process_scan_results, scan_results, NULL);
		scan_process_source_id = g_source_attach (scan_process_source, dev->app_data->main_context);
		g_source_unref (scan_process_source);

		g_get_current_time (&cur_time);
		dev->options.wireless.last_scan = cur_time.tv_sec;
	}

reschedule:
	/* Make sure we reschedule ourselves so we keep scanning */
	nm_device_wireless_schedule_scan (dev);

	g_free (scan_cb);
	return FALSE;
}


/* IP Configuration stuff */

gboolean nm_device_get_use_dhcp (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return dev->use_dhcp;
}

void nm_device_set_use_dhcp (NMDevice *dev, gboolean use_dhcp)
{
	g_return_if_fail (dev != NULL);

	dev->use_dhcp = use_dhcp;
}


NMIP4Config *nm_device_get_ip4_config (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return dev->ip4_config;
}


void nm_device_set_ip4_config (NMDevice *dev, NMIP4Config *config)
{
	NMIP4Config *old_config;

	g_return_if_fail (dev != NULL);

	old_config = dev->ip4_config;
	if (config)
		nm_ip4_config_ref (config);
	dev->ip4_config = config;
	if (old_config)
		nm_ip4_config_unref (old_config);
}

void nm_device_set_wireless_scan_interval (NMDevice *dev, NMWirelessScanInterval interval)
{
	guint seconds;

	g_return_if_fail (dev != NULL);

	switch (interval)
	{
		case NM_WIRELESS_SCAN_INTERVAL_INIT:
			seconds = 10;
			break;

		case NM_WIRELESS_SCAN_INTERVAL_INACTIVE:
			seconds = 120;
			break;

		case NM_WIRELESS_SCAN_INTERVAL_ACTIVE:
		default:
			seconds = 20;
			break;
	}

	dev->options.wireless.scan_interval = seconds;
}


/*
 * nm_device_get_system_config_data
 *
 * Return distro-specific system configuration data for this device.
 *
 */
void *nm_device_get_system_config_data (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return dev->system_config_data;
}


/* Define types for stupid headers */
typedef u_int8_t u8;
typedef u_int16_t u16;
typedef u_int32_t u32;
typedef u_int64_t u64;


/**************************************/
/*    Ethtool capability detection    */
/**************************************/
#include <linux/sockios.h>
#include <linux/ethtool.h>

static gboolean supports_ethtool_carrier_detect (NMDevice *dev)
{
	NMSock			*sk;
	struct ifreq		ifr;
	gboolean			supports_ethtool = FALSE;
	struct ethtool_cmd	edata;

	g_return_val_if_fail (dev != NULL, FALSE);

	if ((sk = nm_dev_sock_open (dev, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
	{
		nm_warning ("cannot open socket on interface %s for ethtool detect; errno=%d", nm_device_get_iface (dev), errno);
		return (FALSE);
	}

	strncpy (ifr.ifr_name, nm_device_get_iface (dev), sizeof(ifr.ifr_name)-1);
	edata.cmd = ETHTOOL_GLINK;
	ifr.ifr_data = (char *) &edata;
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to ETHTOOL\n", nm_device_get_iface (dev));
#endif
	if (ioctl (nm_dev_sock_get_fd (sk), SIOCETHTOOL, &ifr) == -1)
		goto out;

	supports_ethtool = TRUE;

out:
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with ETHTOOL\n", nm_device_get_iface (dev));
#endif
	nm_dev_sock_close (sk);
	return (supports_ethtool);
}



/**************************************/
/*    MII capability detection        */
/**************************************/
#include <linux/mii.h>

static int mdio_read (NMDevice *dev, NMSock *sk, struct ifreq *ifr, int location)
{
	struct mii_ioctl_data *mii;
	int val = -1;

	g_return_val_if_fail (sk != NULL, -1);
	g_return_val_if_fail (ifr != NULL, -1);

	mii = (struct mii_ioctl_data *) &(ifr->ifr_data);
	mii->reg_num = location;

#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET MIIREG\n", nm_device_get_iface (dev));
#endif
	if (ioctl (nm_dev_sock_get_fd (sk), SIOCGMIIREG, ifr) >= 0)
		val = mii->val_out;
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with GET MIIREG\n", nm_device_get_iface (dev));
#endif

	return val;
}

static gboolean supports_mii_carrier_detect (NMDevice *dev)
{
	NMSock *		sk;
	struct ifreq	ifr;
	int			bmsr;
	gboolean		supports_mii = FALSE;
	int			err;

	g_return_val_if_fail (dev != NULL, FALSE);

	if ((sk = nm_dev_sock_open (dev, DEV_GENERAL, __FUNCTION__, NULL)) == NULL)
	{
		nm_warning ("cannot open socket on interface %s for MII detect; errno=%d", nm_device_get_iface (dev), errno);
		return (FALSE);
	}

	strncpy (ifr.ifr_name, nm_device_get_iface (dev), sizeof(ifr.ifr_name)-1);
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to GET MIIPHY\n", nm_device_get_iface (dev));
#endif
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCGMIIPHY, &ifr);
#ifdef IOCTL_DEBUG
	nm_info ("%s: Done with GET MIIPHY\n", nm_device_get_iface (dev));
#endif
	if (err < 0)
		goto out;

	/* If we can read the BMSR register, we assume that the card supports MII link detection */
	bmsr = mdio_read (dev, sk, &ifr, MII_BMSR);
	supports_mii = (bmsr != -1) ? TRUE : FALSE;

out:
	nm_dev_sock_close (sk);
	return supports_mii;	
}


/****************************************/
/* Test device routes                   */
/****************************************/

/*
 * nm_device_is_test_device
 *
 */
gboolean nm_device_is_test_device (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->test_device);
}


/*****************************************/
/* Start code ripped from wpa_supplicant */
/*****************************************/
/*
 * Copyright (c) 2003-2005, Jouni Malinen <jkmaline@cc.hut.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


static int hex2num(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}


static int hex2byte(const char *hex)
{
	int a, b;
	a = hex2num(*hex++);
	if (a < 0)
		return -1;
	b = hex2num(*hex++);
	if (b < 0)
		return -1;
	return (a << 4) | b;
}

static int hexstr2bin(const char *hex, u8 *buf, size_t len)
{
	int i, a;
	const char *ipos = hex;
	u8 *opos = buf;

	for (i = 0; i < len; i++) {
		a = hex2byte(ipos);
		if (a < 0)
			return -1;
		*opos++ = a;
		ipos += 2;
	}
	return 0;
}

#define SCAN_SLEEP_CENTISECONDS		10	/* sleep 1/10 of a second, waiting for data */
static guint8 * get_scan_results (NMDevice *dev, NMSock *sk, guint32 *data_len)
{
	struct iwreq iwr;
	guint8 *res_buf;
	size_t len, res_buf_len = 1000;
	guint8 tries = 0;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (sk != NULL, NULL);

	*data_len = 0;

	res_buf_len = IW_SCAN_MAX_DATA;
	for (;;)
	{
		res_buf = g_malloc (res_buf_len);
		if (res_buf == NULL)
			return NULL;
		memset (&iwr, 0, sizeof(iwr));
		iwr.u.data.pointer = res_buf;
		iwr.u.data.flags = 0;
		iwr.u.data.length = res_buf_len;

		if (iw_get_ext (nm_dev_sock_get_fd (sk), nm_device_get_iface (dev), SIOCGIWSCAN, &iwr) == 0)
			break;

		if ((errno == E2BIG) && (res_buf_len < 100000))
		{
			g_free (res_buf);
			res_buf = NULL;
			res_buf_len *= 2;
		}
		else if (errno == EAGAIN)
		{
			/* If the card doesn't return results after 20s, it sucks. */
			if (tries > 20 * SCAN_SLEEP_CENTISECONDS)
			{
				nm_warning ("get_scan_results(): card took too much time scanning.  Get a better one.");
				free (res_buf);
				return NULL;
			}

			g_free (res_buf);
			g_usleep (G_USEC_PER_SEC / SCAN_SLEEP_CENTISECONDS);
			tries++;
		}
		else
		{
			nm_warning ("get_scan_results(): card returned too much scan info.");
			g_free (res_buf);
			return NULL;
		}
	}

	*data_len = iwr.u.data.length;
	return res_buf;
}


static void add_new_ap_to_device_list (NMDevice *dev, NMAccessPoint *ap)
{
	gboolean new = FALSE;
	gboolean strength_changed = FALSE;
	GTimeVal cur_time;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (ap != NULL);

	g_get_current_time (&cur_time);
	nm_ap_set_last_seen (ap, &cur_time);

	/* If the AP is not broadcasting its ESSID, try to fill it in here from our
	 * allowed list where we cache known MAC->ESSID associations.
	 */
	if (!nm_ap_get_essid (ap))
		nm_ap_list_copy_one_essid_by_address (ap, dev->app_data->allowed_ap_list);

	/* Add the AP to the device's AP list */
	if (nm_ap_list_merge_scanned_ap (nm_device_ap_list_get (dev), ap, &new, &strength_changed))
	{
		DBusConnection *con = dev->app_data->dbus_connection;
		/* Handle dbus signals that we need to broadcast when the AP is added to the list or changes strength */
		if (new)
			nm_dbus_signal_wireless_network_change (con, dev, ap, NETWORK_STATUS_APPEARED, -1);
		else if (strength_changed)
		{
			nm_dbus_signal_wireless_network_change (con, dev, ap, NETWORK_STATUS_STRENGTH_CHANGED,
				nm_ap_get_strength (ap));
		}
	}
}

static gboolean process_scan_results (NMDevice *dev, const guint8 *res_buf, guint32 res_buf_len)
{
	char *pos, *end, *custom, *genie, *gpos, *gend;
	NMAccessPoint *ap = NULL;
	size_t clen;
	struct iw_event iwe_buf, *iwe = &iwe_buf;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (res_buf != NULL, FALSE);
	g_return_val_if_fail (res_buf_len > 0, FALSE);

	pos = (char *) res_buf;
	end = (char *) res_buf + res_buf_len;

	while (pos + IW_EV_LCP_LEN <= end)
	{
		int ssid_len;

		/* Event data may be unaligned, so make a local, aligned copy
		 * before processing. */
		memcpy (&iwe_buf, pos, IW_EV_LCP_LEN);
		if (iwe->len <= IW_EV_LCP_LEN)
			break;

		custom = pos + IW_EV_POINT_LEN;
		if (dev->options.wireless.we_version > 18 &&
		    (iwe->cmd == SIOCGIWESSID ||
		     iwe->cmd == SIOCGIWENCODE ||
		     iwe->cmd == IWEVGENIE ||
		     iwe->cmd == IWEVCUSTOM))
		{
			/* WE-19 removed the pointer from struct iw_point */
			char *dpos = (char *) &iwe_buf.u.data.length;
			int dlen = dpos - (char *) &iwe_buf;
			memcpy (dpos, pos + IW_EV_LCP_LEN, sizeof (struct iw_event) - dlen);
		}
		else
		{
			memcpy (&iwe_buf, pos, sizeof (struct iw_event));
			custom += IW_EV_POINT_OFF;
		}

		switch (iwe->cmd)
		{
			case SIOCGIWAP:
				/* New access point record */

				/* Merge previous AP */
				if (ap)
				{
					add_new_ap_to_device_list (dev, ap);
					nm_ap_unref (ap);
					ap = NULL;
				}

				/* New AP with some defaults */
				ap = nm_ap_new ();
				nm_ap_set_address (ap, (const struct ether_addr *)(iwe->u.ap_addr.sa_data));
				nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_NONE);
				nm_ap_set_mode (ap, NETWORK_MODE_INFRA);
				break;
			case SIOCGIWMODE:
				switch (iwe->u.mode)
				{
					case IW_MODE_ADHOC:
						nm_ap_set_mode (ap, NETWORK_MODE_ADHOC);
						break;
					case IW_MODE_MASTER:
					case IW_MODE_INFRA:
						nm_ap_set_mode (ap, NETWORK_MODE_INFRA);
						break;
					default:
						break;
				}
				break;
			case SIOCGIWESSID:
				ssid_len = iwe->u.essid.length;
				if (custom + ssid_len > end)
					break;
				if (iwe->u.essid.flags && (ssid_len > 0) && (ssid_len <= IW_ESSID_MAX_SIZE))
				{
						gboolean set = TRUE;
						char *essid = g_malloc (IW_ESSID_MAX_SIZE + 1);
						memcpy (essid, custom, ssid_len);
						essid[ssid_len] = '\0';
						if (!strlen(essid))
							set = FALSE;
						else if ((strlen (essid) == 8) && (strcmp (essid, "<hidden>") == 0))	/* Stupid ipw drivers use <hidden> */
							set = FALSE;
						if (set)
							nm_ap_set_essid (ap, essid);
						g_free (essid);
				}
				break;
			case SIOCGIWFREQ:
			     nm_ap_set_freq (ap, iw_freq2float(&(iwe->u.freq)));
				break;
			case IWEVQUAL:
				nm_ap_set_strength (ap, nm_wireless_qual_to_percent (&(iwe->u.qual),
									(const iwqual *)(&dev->options.wireless.max_qual),
									(const iwqual *)(&dev->options.wireless.avg_qual)));
				break;
			case SIOCGIWENCODE:
				if (!(iwe->u.data.flags & IW_ENCODE_DISABLED))
				{
					nm_ap_set_encrypted (ap, TRUE);
					nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
				}
				break;
#if 0
			case SIOCGIWRATE:
				custom = pos + IW_EV_LCP_LEN;
				clen = iwe->len;
				if (custom + clen > end)
					break;
				maxrate = 0;
				while (((ssize_t) clen) >= sizeof(struct iw_param)) {
					/* Note: may be misaligned, make a local,
					 * aligned copy */
					memcpy(&p, custom, sizeof(struct iw_param));
					if (p.value > maxrate)
						maxrate = p.value;
					clen -= sizeof(struct iw_param);
					custom += sizeof(struct iw_param);
				}
				results[ap_num].maxrate = maxrate;
				break;
#endif
			case IWEVGENIE:
				#define GENERIC_INFO_ELEM 0xdd
				#define RSN_INFO_ELEM 0x30
				gpos = genie = custom;
				gend = genie + iwe->u.data.length;
				if (gend > end)
				{
					nm_warning ("get_scan_results(): IWEVGENIE overflow.");
					break;
				}
				while ((gpos + 1 < gend) && (gpos + 2 + (u8) gpos[1] <= gend))
				{
					u8 ie = gpos[0], ielen = gpos[1] + 2;
					if (ielen > AP_MAX_WPA_IE_LEN)
					{
						gpos += ielen;
						continue;
					}
					switch (ie)
					{
						case GENERIC_INFO_ELEM:
							if ((ielen < 2 + 4) || (memcmp (&gpos[2], "\x00\x50\xf2\x01", 4) != 0))
								break;
							nm_ap_set_wpa_ie (ap, gpos, ielen);
							break;
						case RSN_INFO_ELEM:
							nm_ap_set_rsn_ie (ap, gpos, ielen);
							break;
					}
					gpos += ielen;
				}
				break;
			case IWEVCUSTOM:
				clen = iwe->u.data.length;
				if (custom + clen > end)
					break;
				if (clen > 7 && ((strncmp (custom, "wpa_ie=", 7) == 0) || (strncmp (custom, "rsn_ie=", 7) == 0)))
				{
					char *spos;
					int bytes;
					char *ie_buf;

					spos = custom + 7;
					bytes = custom + clen - spos;
					if (bytes & 1)
						break;
					bytes /= 2;
					if (bytes > AP_MAX_WPA_IE_LEN)
					{
						nm_warning ("get_scan_results(): IE was too long (%d bytes).", bytes);
						break;
					}
					ie_buf = g_malloc0 (bytes);
					hexstr2bin (spos, ie_buf, bytes);
					if (strncmp (custom, "wpa_ie=", 7) == 0)
						nm_ap_set_wpa_ie (ap, ie_buf, bytes);
					else if (strncmp (custom, "rsn_ie=", 7) == 0)
						nm_ap_set_rsn_ie (ap, ie_buf, bytes);				
					g_free (ie_buf);
				}
				break;
			default:
				break;
		}

		pos += iwe->len;
	}

	if (ap)
	{
		add_new_ap_to_device_list (dev, ap);
		nm_ap_unref (ap);
		ap = NULL;
	}

	return TRUE;
}

/*****************************************/
/* End code ripped from wpa_supplicant */
/*****************************************/
