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
 * Copyright (C) 2004 Red Hat, Inc.
 * Copyright (C) 1996 - 1997 Yoichi Hariguchi <yoichi@fore.com>
 * Copyright (C) January, 1998 Sergei Viznyuk <sv@phystech.com>
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <net/route.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <resolv.h>
#include <netdb.h>
#include <glib.h>
#include <pthread.h>
#include "NetworkManagerSystem.h"
#include "nm-device.h"
#include "NetworkManagerUtils.h"
#include "nm-utils.h"

#include <netlink/route/addr.h>
#include <netlink/netlink.h>
#include <netlink/utils.h>
#include <netlink/route/link.h>


/*
 * nm_system_device_set_ip4_route
 *
 */
static gboolean nm_system_device_set_ip4_route (NMDevice *dev, int ip4_gateway, int ip4_dest, int ip4_netmask, int mss)
{
	NMSock *			sk;
	gboolean			success = FALSE;
	struct rtentry		rtent;
	struct sockaddr_in *p;
	const char *		iface;
	int				err;
	NMIP4Config * config = NULL;

	iface = nm_device_get_iface (dev);

	/*
	 * Zero is not a legal gateway and the ioctl will fail.  But zero is a
	 * way of saying "no route" so we just return here.  Hopefully the
	 * caller flushed the routes, first.
	 */
	if (ip4_gateway == 0)
		return TRUE;

	/*
	 * Do not add the route if the destination is on the same subnet.
	 */
	config = nm_device_get_ip4_config(dev);
	if (config &&
	    ((guint32)ip4_dest & nm_ip4_config_get_netmask(config)) ==
	        (nm_ip4_config_get_address(config) & nm_ip4_config_get_netmask(config)))
		return TRUE;

	if ((sk = nm_dev_sock_open (dev, NETWORK_CONTROL, __FUNCTION__, NULL)) == NULL)
		return FALSE;

	memset (&rtent, 0, sizeof (struct rtentry));
	p				= (struct sockaddr_in *) &rtent.rt_dst;
	p->sin_family		= AF_INET;
	p->sin_addr.s_addr	= ip4_dest;
	p				= (struct sockaddr_in *) &rtent.rt_gateway;
	p->sin_family		= AF_INET;
	p->sin_addr.s_addr	= ip4_gateway;
	p				= (struct sockaddr_in *) &rtent.rt_genmask;
	p->sin_family		= AF_INET;
	p->sin_addr.s_addr	= ip4_netmask;
	rtent.rt_dev		= (char *)iface;
	rtent.rt_metric	= 1;
	rtent.rt_window	= 0;
	rtent.rt_flags		= RTF_UP | RTF_GATEWAY | (rtent.rt_window ? RTF_WINDOW : 0);

	if (mss)
	{
		rtent.rt_flags |= RTF_MTU;
		rtent.rt_mtu = mss;
	}

#ifdef IOCTL_DEBUG
	nm_info ("%s: About to CADDRT\n", nm_device_get_iface (dev));
#endif
	err = ioctl (nm_dev_sock_get_fd (sk), SIOCADDRT, &rtent);
#ifdef IOCTL_DEBUG
	nm_info ("%s: About to CADDRT\n", nm_device_get_iface (dev));
#endif

	if (err == -1)
	{
		if (errno == ENETUNREACH)	/* possibly gateway is over the bridge */
		{						/* try adding a route to gateway first */
			struct rtentry	rtent2;
			
			memset (&rtent2, 0, sizeof(struct rtentry));
			p				= (struct sockaddr_in *)&rtent2.rt_dst;
			p->sin_family		= AF_INET;
			p				= (struct sockaddr_in *)&rtent2.rt_gateway;
			p->sin_family		= AF_INET;
			p->sin_addr.s_addr	= ip4_gateway;
			p				= (struct sockaddr_in *)&rtent2.rt_genmask;
			p->sin_family		= AF_INET;
			p->sin_addr.s_addr	= 0xffffffff;
			rtent2.rt_dev		= (char *)iface;
			rtent2.rt_metric	= 0;
			rtent2.rt_flags	= RTF_UP | RTF_HOST;

			if (mss)
			{
				rtent2.rt_flags |= RTF_MTU;
				rtent2.rt_mtu = mss;
			}

#ifdef IOCTL_DEBUG
			nm_info ("%s: About to CADDRT (2)\n", nm_device_get_iface (dev));
#endif
			err = ioctl (nm_dev_sock_get_fd (sk), SIOCADDRT, &rtent2);
#ifdef IOCTL_DEBUG
			nm_info ("%s: About to CADDRT (2)\n", nm_device_get_iface (dev));
#endif

			if (!err)
			{
#ifdef IOCTL_DEBUG
				nm_info ("%s: About to CADDRT (3)\n", nm_device_get_iface (dev));
#endif
				err = ioctl (nm_dev_sock_get_fd (sk), SIOCADDRT, &rtent);
#ifdef IOCTL_DEBUG
				nm_info ("%s: About to CADDRT (3)\n", nm_device_get_iface (dev));
#endif

				if (!err)
					success = TRUE;
				else
					nm_warning ("Failed to set IPv4 default route on '%s': %s", iface, strerror (errno));
			}
		}
		else
			nm_warning ("Failed to set IPv4 default route on '%s': %s", iface, strerror (errno));
	}
	else
		success = TRUE;

	nm_dev_sock_close (sk);
	return success;
}


static void iface_to_rtnl_index (const char *iface, struct nl_handle *nlh, struct rtnl_addr *addr)
{
	struct nl_cache *	cache = NULL;
	int				i;

	g_return_if_fail (iface != NULL);
	g_return_if_fail (nlh != NULL);
	g_return_if_fail (addr != NULL);

	cache = rtnl_link_alloc_cache (nlh);
	if (!cache) {
		nm_warning ("%s: couldn't allocate link cache.", __func__);
		return;
	}

	nl_cache_update (nlh, cache);
	i = rtnl_link_name2i (cache, iface);
	if (RTNL_LINK_NOT_FOUND != i)
		rtnl_addr_set_ifindex (addr, i);
	nl_cache_free (cache);
}


static struct rtnl_link * iface_to_rtnl_link (const char *iface, struct nl_handle *nlh)
{
	struct nl_cache *	cache = NULL;
	struct rtnl_link *	have_link = NULL;

	g_return_val_if_fail (iface != NULL, NULL);
	g_return_val_if_fail (nlh != NULL, NULL);

	cache = rtnl_link_alloc_cache (nlh);
	if (!cache) {
		nm_warning ("%s: couldn't allocate link cache.", __func__);
		return NULL;
	}

	nl_cache_update (nlh, cache);
	have_link = rtnl_link_get_by_name (cache, iface);
	nl_cache_free (cache);

	return have_link;
}

static struct
nl_handle *new_nl_handle (gboolean recursive)
{
	struct nl_handle *nlh = NULL;
	struct nl_cb *cb;

	cb = nl_cb_alloc (NL_CB_VERBOSE);
	nlh = nl_handle_alloc_cb (cb);
	if (!nlh) {
		nm_warning ("%s: couldn't allocate netlink handle: %s", __func__, nl_geterror ());
		return NULL;
	}

	if (nl_connect (nlh, NETLINK_ROUTE) < 0) {
		/* HACK: try one more time. Because the netlink monitor for link state
		 * inits before we get here, it grabs the port that matches the PID
		 * of the NM process, which also happens to be the PID that libnl uses
		 * the first time too.  The real fix is to convert nm-netlink-monitor.c
		 * over to use libnl.
		 */
		nl_handle_destroy (nlh);
		if (recursive)
			return NULL;

		nlh = new_nl_handle (TRUE);
		if (!nlh) {
			nm_warning ("%s: couldn't connect to netlink: %s", __func__, nl_geterror ());
			return NULL;
		}
	}

	return nlh;
}

static void
destroy_nl_handle (struct nl_handle *nlh)
{
	g_return_if_fail (nlh != NULL);

	nl_close (nlh);
	nl_handle_destroy (nlh);
}

int
nm_system_get_rtnl_index_from_iface (const char *iface)
{
	struct nl_handle *	nlh = NULL;
	struct nl_cache *	cache;
	int				i = RTNL_LINK_NOT_FOUND;

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		return RTNL_LINK_NOT_FOUND;

	cache = rtnl_link_alloc_cache (nlh);
	if (!cache) {
		nm_warning ("%s: couldn't allocate link cache.", iface);
		goto out;
	}

	nl_cache_update (nlh, cache);
	i = rtnl_link_name2i (cache, iface);
	nl_cache_free (cache);

out:
	destroy_nl_handle (nlh);
	return i;
}


#define MAX_IFACE_LEN	32
char *
nm_system_get_iface_from_rtnl_index (int rtnl_index)
{
	struct nl_handle *	nlh = NULL;
	struct nl_cache *	cache = NULL;
	char *			buf = NULL;

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		return NULL;

	cache = rtnl_link_alloc_cache (nlh);
	if (!cache) {
		nm_warning ("Couldn't allocate link cache.");
		goto out;
	}

	nl_cache_update (nlh, cache);

	buf = g_malloc0 (MAX_IFACE_LEN);
	if (!buf)
		goto destroy_cache;

	if (!rtnl_link_i2name (cache, rtnl_index, buf, MAX_IFACE_LEN - 1)) {
		g_free (buf);
		buf = NULL;
	}

destroy_cache:
	nl_cache_free (cache);

out:
	destroy_nl_handle (nlh);
	return buf;
}


/*
 * nm_system_device_set_from_ip4_config
 *
 * Set IPv4 configuration of the device from an NMIP4Config object.
 *
 */
gboolean nm_system_device_set_from_ip4_config (NMDevice *dev)
{
	NMData *			app_data;
	NMIP4Config *		config;
	struct nl_handle *	nlh = NULL;
	struct rtnl_addr *	addr = NULL;
	int				err;

	g_return_val_if_fail (dev != NULL, FALSE);

	app_data = nm_device_get_app_data (dev);
	g_return_val_if_fail (app_data != NULL, FALSE);

	config = nm_device_get_ip4_config (dev);
	g_return_val_if_fail (config != NULL, FALSE);

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		return FALSE;

	nm_system_delete_default_route ();
	nm_system_device_flush_addresses (dev);
	nm_system_device_flush_routes (dev);
	nm_system_flush_arp_cache ();

	if ((addr = nm_ip4_config_to_rtnl_addr (config, NM_RTNL_ADDR_DEFAULT)))
	{
		iface_to_rtnl_index (nm_device_get_iface (dev), nlh, addr);
		if ((err = rtnl_addr_add (nlh, addr, 0)) < 0)
			nm_warning ("nm_system_device_set_from_ip4_config(%s): error %d returned from rtnl_addr_add():\n%s", nm_device_get_iface (dev), err, nl_geterror());
		rtnl_addr_put (addr);
	}
	else
		nm_warning ("nm_system_device_set_from_ip4_config(): couldn't create rtnl address!\n");

	destroy_nl_handle (nlh);

	sleep (1);
	nm_system_device_set_ip4_route (dev, nm_ip4_config_get_gateway (config), 0, 0, nm_ip4_config_get_mss (config));

	nm_named_manager_add_ip4_config (app_data->named_manager, config);

	return TRUE;
}


/*
 * validate_ip4_route
 *
 * Ensure that IP4 routes are in the correct format
 *
 */
static char *validate_ip4_route (const char *route)
{
	char *		ret = NULL;
	char *		temp = NULL;
	int			slash_pos = -1;
	char *		p = NULL;
	int			len, i;
	int			dot_count = 0;
	gboolean		have_slash = FALSE;
	struct in_addr	addr;

	g_return_val_if_fail (route != NULL, NULL);

	len = strlen (route);
	/* Minimum length, ie 1.1.1.1/8 */
	if (len < 9)
		return NULL;

	for (i = 0; i < len; i++)
	{
		/* Ensure there is only one slash */
		if (route[i] == '/')
		{
			if (have_slash)
				goto out;

			have_slash = TRUE;
			slash_pos = i;
			continue;
		}

		if (route[i] == '.')
		{
			if (dot_count >= 4)
				goto out;

			dot_count++;
			continue;
		}

		if (!isdigit (route[i]))
			goto out;
	}

	/* Make sure there is at least one slash and 3 dots */
	if (!have_slash || !slash_pos || (dot_count != 3))
		goto out;

	/* Valid IP address part */
	temp = g_strdup (route);
	temp[slash_pos] = '\0';
	memset (&addr, 0, sizeof (struct in_addr));
	if (inet_aton (temp, &addr) == 0)
		goto out;

	/* Ensure the network # is valid */
	p = temp + slash_pos + 1;
	i = (int) strtol (p, NULL, 10);
	if ((i < 0) || (i > 32))
		goto out;

	/* Success! */
	ret = g_strdup (route);

out:
	g_free (temp);
	return ret;
}


/*
 * nm_system_vpn_device_set_from_ip4_config
 *
 * Set IPv4 configuration of a VPN device from an NMIP4Config object.
 *
 */
gboolean
nm_system_vpn_device_set_from_ip4_config (NMNamedManager *named,
                                          NMDevice *active_device,
                                          const char *iface,
                                          NMIP4Config *config,
                                          char **routes,
                                          int num_routes)
{
	NMIP4Config *		ad_config = NULL;
	struct nl_handle *	nlh = NULL;
	struct rtnl_addr *	addr = NULL;
	struct rtnl_link *	request = NULL;

	g_return_val_if_fail (config != NULL, FALSE);

	/* Set up a route to the VPN gateway through the real network device */
	if (active_device && (ad_config = nm_device_get_ip4_config (active_device)))
		nm_system_device_set_ip4_route (active_device, nm_ip4_config_get_gateway (ad_config), nm_ip4_config_get_gateway (config), 0xFFFFFFFF, nm_ip4_config_get_mss (config));

	if (!iface || !strlen (iface))
		goto done;

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		goto done;

	nm_system_device_set_up_down_with_iface (iface, TRUE);

	if ((addr = nm_ip4_config_to_rtnl_addr (config, NM_RTNL_ADDR_PTP_DEFAULT)))
	{
		int err = 0;
		iface_to_rtnl_index (iface, nlh, addr);
		if ((err = rtnl_addr_add (nlh, addr, 0)) < 0)
			nm_warning ("nm_system_device_set_from_ip4_config(): error %d returned from rtnl_addr_add():\n%s", err, nl_geterror());
		rtnl_addr_put (addr);
	}
	else
		nm_warning ("nm_system_vpn_device_set_from_ip4_config(): couldn't create rtnl address!\n");

	/* Set the MTU */
	if ((request = rtnl_link_alloc ()))
	{
		struct rtnl_link * old;

		old = iface_to_rtnl_link (iface, nlh);
		rtnl_link_set_mtu (request, 1412);
		rtnl_link_change (nlh, old, request, 0);

		rtnl_link_put (old);
		rtnl_link_put (request);
	}

	destroy_nl_handle (nlh);

	sleep (1);

	nm_system_device_flush_routes_with_iface (iface);
	if (num_routes <= 0)
	{
		nm_system_delete_default_route ();
		nm_system_device_add_default_route_via_device_with_iface (iface);
	}
	else
	{
		int i;
		for (i = 0; i < num_routes; i++)
		{
			char *valid_ip4_route;

			/* Make sure the route is valid, otherwise it's a security risk as the route
			 * text is simply taken from the user, and passed directly to system().  If
			 * we did not check the route, think of:
			 *
			 *     system("/sbin/ip route add `rm -rf /` dev eth0")
			 *
			 * where `rm -rf /` was the route text.  As UID 0 (root), we have to be careful.
			 */
			if ((valid_ip4_route = validate_ip4_route (routes[i])))
			{
				nm_system_device_add_route_via_device_with_iface (iface, valid_ip4_route);
				g_free (valid_ip4_route);
			}
		}
	}

done:
	nm_named_manager_add_ip4_config (named, config);

	return TRUE;
}


/*
 * nm_system_vpn_device_unset_from_ip4_config
 *
 * Unset an IPv4 configuration of a VPN device from an NMIP4Config object.
 *
 */
gboolean nm_system_vpn_device_unset_from_ip4_config (NMNamedManager *named, NMDevice *active_device, const char *iface, NMIP4Config *config)
{
	g_return_val_if_fail (named != NULL, FALSE);
	g_return_val_if_fail (active_device != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	nm_named_manager_remove_ip4_config (named, config);

	return TRUE;
}


/*
 * nm_system_device_set_up_down
 *
 * Mark the device as up or down.
 *
 */
gboolean nm_system_device_set_up_down (NMDevice *dev, gboolean up)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return nm_system_device_set_up_down_with_iface (nm_device_get_iface (dev), up);
}

gboolean nm_system_device_set_up_down_with_iface (const char *iface, gboolean up)
{
	gboolean success = FALSE;
	struct nl_handle *	nlh = NULL;
	struct rtnl_link *	request = NULL;
	struct rtnl_link *	old = NULL;

	g_return_val_if_fail (iface != NULL, FALSE);

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		return FALSE;

	if (!(request = rtnl_link_alloc ()))
		goto out;

	if (up)
		rtnl_link_set_flags (request, IFF_UP);
	else
		rtnl_link_unset_flags (request, IFF_UP);

	old = iface_to_rtnl_link (iface, nlh);
	if (old)
		rtnl_link_change (nlh, old, request, 0);

	rtnl_link_put (old);
	rtnl_link_put (request);

	success = TRUE;

out:
	destroy_nl_handle (nlh);
	return success;
}


/*
 * nm_system_set_mtu
 *
 * Set the MTU for a given device.
 */
void nm_system_set_mtu (NMDevice *dev)
{
	struct rtnl_link *	request;
	struct rtnl_link *	old;
	unsigned long		mtu;
	struct nl_handle *	nlh;
	const char *		iface;

	mtu = nm_system_get_mtu (dev);
	if (!mtu)
		return;

	nlh = new_nl_handle (FALSE);
	if (!nlh)
		return;

	request = rtnl_link_alloc ();
	if (!request)
		goto out_nl_close;

	iface = nm_device_get_iface (dev);
	old = iface_to_rtnl_link (iface, nlh);
	if (!old)
		goto out_request;

	nm_info ("Setting MTU of interface '%s' to %ld", iface, mtu);
	rtnl_link_set_mtu (request, mtu);
	rtnl_link_change (nlh, old, request, 0);

	rtnl_link_put (old);
out_request:
	rtnl_link_put (request);
out_nl_close:
	destroy_nl_handle (nlh);
}

