/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service - keyfile plugin
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
 * Copyright (C) 2008 Red Hat, Inc.
 */

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <dbus/dbus-glib.h>

#include <nm-utils.h>
#include <nm-setting-connection.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>

#include "nm-test-helpers.h"

#include "reader.h"
#include "writer.h"

#define TEST_WIRED_FILE    TEST_KEYFILES_DIR"/Test_Wired_Connection"
#define TEST_WIRELESS_FILE TEST_KEYFILES_DIR"/Test_Wireless_Connection"

static void
test_read_valid_wired_connection (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	NMSettingIP4Config *s_ip4;
	NMSettingIP6Config *s_ip6;
	GError *error = NULL;
	const GByteArray *array;
	char expected_mac_address[ETH_ALEN] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
	const char *tmp;
	const char *expected_id = "Test Wired Connection";
	const char *expected_uuid = "4e80a56d-c99f-4aad-a6dd-b449bc398c57";
	const guint64 expected_timestamp = 6654332;
	guint64 timestamp;
	const char *expected_dns1 = "4.2.2.1";
	const char *expected_dns2 = "4.2.2.2";
	struct in_addr addr;
	const char *expected_address1 = "192.168.0.5";
	const char *expected_address2 = "1.2.3.4";
	const char *expected_address1_gw = "192.168.0.1";
	const char *expected_address2_gw = "1.2.1.1";
	NMIP4Address *ip4_addr;
	const char *expected6_dns1 = "1111:dddd::aaaa";
	const char *expected6_dns2 = "1::cafe";
	const char *expected6_dnssearch1 = "super-domain.com";
	const char *expected6_dnssearch2 = "redhat.com";
	const char *expected6_dnssearch3 = "gnu.org";
	struct in6_addr addr6;
	const char *expected6_address1 = "abcd:1234:ffff::cdde";
	const char *expected6_address2 = "1:2:3:4:5:6:7:8";
	const char *expected6_route_dest = "a:b:c:d::";
	const char *expected6_route_nh = "f:e:d:c:1:2:3:4";
	NMIP6Address *ip6_addr;
	NMIP6Route *ip6_route;

	connection = connection_from_file (TEST_WIRED_FILE);
	ASSERT (connection != NULL,
			"connection-read", "failed to read %s", TEST_WIRED_FILE);

	ASSERT (nm_connection_verify (connection, &error),
	        "connection-verify", "failed to verify %s: %s", TEST_WIRED_FILE, error->message);

	/* ===== CONNECTION SETTING ===== */

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	ASSERT (s_con != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s setting",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME);

	/* ID */
	tmp = nm_setting_connection_get_id (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);
	ASSERT (strcmp (tmp, expected_id) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);

	/* UUID */
	tmp = nm_setting_connection_get_uuid (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);
	ASSERT (strcmp (tmp, expected_uuid) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);

	/* Timestamp */
	timestamp = nm_setting_connection_get_timestamp (s_con);
	ASSERT (timestamp == expected_timestamp,
	        "connection-verify-connection", "failed to verify %s: unexpected %s /%s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_TIMESTAMP);

	/* Autoconnect */
	ASSERT (nm_setting_connection_get_autoconnect (s_con) == TRUE,
	        "connection-verify-connection", "failed to verify %s: unexpected %s /%s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_AUTOCONNECT);

	/* ===== WIRED SETTING ===== */

	s_wired = NM_SETTING_WIRED (nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRED));
	ASSERT (s_wired != NULL,
	        "connection-verify-wired", "failed to verify %s: missing %s setting",
	        TEST_WIRED_FILE,
	        NM_SETTING_WIRED_SETTING_NAME);

	/* MAC address */
	array = nm_setting_wired_get_mac_address (s_wired);
	ASSERT (array != NULL,
	        "connection-verify-wired", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);
	ASSERT (array->len == ETH_ALEN,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value length",
	        TEST_WIRED_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);
	ASSERT (memcmp (array->data, &expected_mac_address[0], sizeof (expected_mac_address)) == 0,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);

	ASSERT (nm_setting_wired_get_mtu (s_wired) == 1400,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MTU);

	/* ===== IPv4 SETTING ===== */

	s_ip4 = NM_SETTING_IP4_CONFIG (nm_connection_get_setting (connection, NM_TYPE_SETTING_IP4_CONFIG));
	ASSERT (s_ip4 != NULL,
	        "connection-verify-ip4", "failed to verify %s: missing %s setting",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME);

	/* Method */
	tmp = nm_setting_ip4_config_get_method (s_ip4);
	ASSERT (strcmp (tmp, NM_SETTING_IP4_CONFIG_METHOD_MANUAL) == 0,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_METHOD);

	/* DNS Addresses */
	ASSERT (nm_setting_ip4_config_get_num_dns (s_ip4) == 2,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);

	ASSERT (inet_pton (AF_INET, expected_dns1, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert DNS IP address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);
	ASSERT (nm_setting_ip4_config_get_dns (s_ip4, 0) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);

	ASSERT (inet_pton (AF_INET, expected_dns2, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert DNS IP address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);
	ASSERT (nm_setting_ip4_config_get_dns (s_ip4, 1) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);

	ASSERT (nm_setting_ip4_config_get_num_addresses (s_ip4) == 2,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);

	/* Address #1 */
	ip4_addr = nm_setting_ip4_config_get_address (s_ip4, 0);
	ASSERT (ip4_addr,
	        "connection-verify-wired", "failed to verify %s: missing IP4 address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (nm_ip4_address_get_prefix (ip4_addr) == 24,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #1 prefix",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET, expected_address1, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);
	ASSERT (nm_ip4_address_get_address (ip4_addr) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET, expected_address1_gw, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #1 gateway",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);
	ASSERT (nm_ip4_address_get_gateway (ip4_addr) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #1 gateway",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);
	
	/* Address #2 */
	ip4_addr = nm_setting_ip4_config_get_address (s_ip4, 1);
	ASSERT (ip4_addr,
	        "connection-verify-wired", "failed to verify %s: missing IP4 address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (nm_ip4_address_get_prefix (ip4_addr) == 16,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #2 prefix",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET, expected_address2, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_DNS);
	ASSERT (nm_ip4_address_get_address (ip4_addr) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET, expected_address2_gw, &addr) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #2 gateway",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);
	ASSERT (nm_ip4_address_get_gateway (ip4_addr) == addr.s_addr,
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #2 gateway",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_ADDRESSES);

	/* ===== IPv6 SETTING ===== */

	s_ip6 = NM_SETTING_IP6_CONFIG (nm_connection_get_setting (connection, NM_TYPE_SETTING_IP6_CONFIG));
	ASSERT (s_ip6 != NULL,
	        "connection-verify-ip6", "failed to verify %s: missing %s setting",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME);

	/* Method */
	tmp = nm_setting_ip6_config_get_method (s_ip6);
	ASSERT (strcmp (tmp, NM_SETTING_IP6_CONFIG_METHOD_MANUAL) == 0,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_METHOD);

	/* DNS Addresses */
	ASSERT (nm_setting_ip6_config_get_num_dns (s_ip6) == 2,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);

	ASSERT (inet_pton (AF_INET6, expected6_dns1, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert DNS IP6 address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_setting_ip6_config_get_dns (s_ip6, 0), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);

	ASSERT (inet_pton (AF_INET6, expected6_dns2, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert DNS IP address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_setting_ip6_config_get_dns (s_ip6, 1), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);

	ASSERT (nm_setting_ip6_config_get_num_addresses (s_ip6) == 2,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);

	/* DNS Searches */
	ASSERT (nm_setting_ip6_config_get_num_dns_searches (s_ip6) == 3,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS_SEARCH);

	ASSERT (!strcmp (nm_setting_ip6_config_get_dns_search (s_ip6, 0), expected6_dnssearch1),
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS_SEARCH);
	ASSERT (!strcmp (nm_setting_ip6_config_get_dns_search (s_ip6, 1), expected6_dnssearch2),
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS_SEARCH);
	ASSERT (!strcmp (nm_setting_ip6_config_get_dns_search (s_ip6, 2), expected6_dnssearch3),
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value #3",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS_SEARCH);

	/* Address #1 */
	ip6_addr = nm_setting_ip6_config_get_address (s_ip6, 0);
	ASSERT (ip6_addr,
	        "connection-verify-wired", "failed to verify %s: missing IP6 address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	ASSERT (nm_ip6_address_get_prefix (ip6_addr) == 64,
	        "connection-verify-wired", "failed to verify %s: unexpected IP6 address #1 prefix",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET6, expected6_address1, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_ip6_address_get_address (ip6_addr), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 address #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	/* Address #2 */
	ip6_addr = nm_setting_ip6_config_get_address (s_ip6, 1);
	ASSERT (ip6_addr,
	        "connection-verify-wired", "failed to verify %s: missing IP6 address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	ASSERT (nm_ip6_address_get_prefix (ip6_addr) == 96,
	        "connection-verify-wired", "failed to verify %s: unexpected IP6 address #2 prefix",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	ASSERT (inet_pton (AF_INET6, expected6_address2, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_ip6_address_get_address (ip6_addr), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected IP6 address #2",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ADDRESSES);

	/* Route #1 */
	ip6_route = nm_setting_ip6_config_get_route (s_ip6, 0);
	ASSERT (ip6_route,
	        "connection-verify-wired", "failed to verify %s: missing IP6 route #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ROUTES);

	ASSERT (inet_pton (AF_INET6, expected6_route_dest, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP route dest #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_ip6_route_get_dest (ip6_route), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 route dest #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ROUTES);

	ASSERT (nm_ip6_route_get_prefix (ip6_route) == 64,
	        "connection-verify-wired", "failed to verify %s: unexpected IP6 route #1 prefix",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ROUTES);

	ASSERT (inet_pton (AF_INET6, expected6_route_nh, &addr6) > 0,
	        "connection-verify-wired", "failed to verify %s: couldn't convert IP route next hop #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_DNS);
	ASSERT (IN6_ARE_ADDR_EQUAL (nm_ip6_route_get_next_hop (ip6_route), &addr6),
	        "connection-verify-wired", "failed to verify %s: unexpected IP4 route dest #1",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ROUTES);

	ASSERT (nm_ip6_route_get_metric (ip6_route) == 99,
	        "connection-verify-wired", "failed to verify %s: unexpected IP6 route #1 metric",
	        TEST_WIRED_FILE,
	        NM_SETTING_IP6_CONFIG_SETTING_NAME,
	        NM_SETTING_IP6_CONFIG_ROUTES);

	g_object_unref (connection);
}

static void
add_one_ip4_address (NMSettingIP4Config *s_ip4,
                     const char *addr,
                     const char *gw,
                     guint32 prefix)
{
	struct in_addr tmp;
	NMIP4Address *ip4_addr;

	ip4_addr = nm_ip4_address_new ();
	nm_ip4_address_set_prefix (ip4_addr, prefix);

	inet_pton (AF_INET, addr, &tmp);
	nm_ip4_address_set_address (ip4_addr, tmp.s_addr);

	inet_pton (AF_INET, gw, &tmp);
	nm_ip4_address_set_gateway (ip4_addr, tmp.s_addr);

	nm_setting_ip4_config_add_address (s_ip4, ip4_addr);
	nm_ip4_address_unref (ip4_addr);
}

static void
add_one_ip4_route (NMSettingIP4Config *s_ip4,
                   const char *dest,
                   const char *nh,
                   guint32 prefix,
                   guint32 metric)
{
	struct in_addr addr;
	NMIP4Route *route;

	route = nm_ip4_route_new ();
	nm_ip4_route_set_prefix (route, prefix);
	nm_ip4_route_set_metric (route, metric);

	inet_pton (AF_INET, dest, &addr);
	nm_ip4_route_set_dest (route, addr.s_addr);

	inet_pton (AF_INET, nh, &addr);
	nm_ip4_route_set_next_hop (route, addr.s_addr);

	nm_setting_ip4_config_add_route (s_ip4, route);
	nm_ip4_route_unref (route);
}

static void
add_one_ip6_address (NMSettingIP6Config *s_ip6,
                     const char *addr,
                     guint32 prefix)
{
	struct in6_addr tmp;
	NMIP6Address *ip6_addr;

	ip6_addr = nm_ip6_address_new ();
	nm_ip6_address_set_prefix (ip6_addr, prefix);

	inet_pton (AF_INET6, addr, &tmp);
	nm_ip6_address_set_address (ip6_addr, &tmp);

	nm_setting_ip6_config_add_address (s_ip6, ip6_addr);
	nm_ip6_address_unref (ip6_addr);
}

static void
add_one_ip6_route (NMSettingIP6Config *s_ip6,
                   const char *dest,
                   const char *nh,
                   guint32 prefix,
                   guint32 metric)
{
	struct in6_addr addr;
	NMIP6Route *route;

	route = nm_ip6_route_new ();
	nm_ip6_route_set_prefix (route, prefix);
	nm_ip6_route_set_metric (route, metric);

	inet_pton (AF_INET6, dest, &addr);
	nm_ip6_route_set_dest (route, &addr);

	inet_pton (AF_INET6, nh, &addr);
	nm_ip6_route_set_next_hop (route, &addr);

	nm_setting_ip6_config_add_route (s_ip6, route);
	nm_ip6_route_unref (route);
}


static void
test_write_wired_connection (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	NMSettingIP4Config *s_ip4;
	NMSettingIP6Config *s_ip6;
	char *uuid;
	GByteArray *mac;
	unsigned char tmpmac[] = { 0x99, 0x88, 0x77, 0x66, 0x55, 0x44 };
	gboolean success;
	NMConnection *reread;
	char *testfile = NULL;
	GError *error = NULL;
	pid_t owner_grp;
	uid_t owner_uid;
	struct in_addr addr;
	struct in6_addr addr6;
	const char *dns1 = "4.2.2.1";
	const char *dns2 = "4.2.2.2";
	const char *address1 = "192.168.0.5";
	const char *address1_gw = "192.168.0.1";
	const char *address2 = "1.2.3.4";
	const char *address2_gw = "1.2.1.1";
	const char *route1 = "10.10.10.2";
	const char *route1_nh = "10.10.10.1";
	const char *route2 = "0.0.0.0";
	const char *route2_nh = "1.2.1.1";
	const char *dns6_1 = "1::cafe";
	const char *dns6_2 = "2::cafe";
	const char *address6_1 = "abcd::beef";
	const char *address6_2 = "dcba::beef";
	const char *route6_1 = "1:2:3:4:5:6:7:8";
	const char *route6_1_nh = "8:7:6:5:4:3:2:1";
	const char *route6_2 = "::";
	const char *route6_2_nh = "2001::1111";
	guint64 timestamp = 0x12345678L;

	connection = nm_connection_new ();
	ASSERT (connection != NULL,
			"connection-write", "failed to allocate new connection");

	/* Connection setting */

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	ASSERT (s_con != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_CONNECTION_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_con));

	uuid = nm_utils_uuid_generate ();
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_ID, "Work Wired",
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME,
	              NM_SETTING_CONNECTION_TIMESTAMP, timestamp,
	              NULL);
	g_free (uuid);

	/* Wired setting */

	s_wired = NM_SETTING_WIRED (nm_setting_wired_new ());
	ASSERT (s_wired != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_WIRED_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_wired));

	mac = g_byte_array_sized_new (ETH_ALEN);
	g_byte_array_append (mac, &tmpmac[0], sizeof (tmpmac));
	g_object_set (s_wired,
	              NM_SETTING_WIRED_MAC_ADDRESS, mac,
	              NM_SETTING_WIRED_MTU, 900,
	              NULL);
	g_byte_array_free (mac, TRUE);

	/* IP4 setting */

	s_ip4 = NM_SETTING_IP4_CONFIG (nm_setting_ip4_config_new ());
	ASSERT (s_ip4 != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_IP4_CONFIG_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_ip4));

	g_object_set (s_ip4,
	              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
	              NULL);

	/* Addresses */
	add_one_ip4_address (s_ip4, address1, address1_gw, 24);
	add_one_ip4_address (s_ip4, address2, address2_gw, 8);

	/* Routes */
	add_one_ip4_route (s_ip4, route1, route1_nh, 24, 3);
	add_one_ip4_route (s_ip4, route2, route2_nh, 8, 1);

	/* DNS servers */
	inet_pton (AF_INET, dns1, &addr);
	nm_setting_ip4_config_add_dns (s_ip4, addr.s_addr);
	inet_pton (AF_INET, dns2, &addr);
	nm_setting_ip4_config_add_dns (s_ip4, addr.s_addr);

	/* IP6 setting */

	s_ip6 = NM_SETTING_IP6_CONFIG (nm_setting_ip6_config_new ());
	ASSERT (s_ip6 != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_IP6_CONFIG_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_ip6));

	g_object_set (s_ip6,
	              NM_SETTING_IP6_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_MANUAL,
	              NULL);

	/* Addresses */
	add_one_ip6_address (s_ip6, address6_1, 64);
	add_one_ip6_address (s_ip6, address6_2, 56);

	/* Routes */
	add_one_ip6_route (s_ip6, route6_1, route6_1_nh, 64, 3);
	add_one_ip6_route (s_ip6, route6_2, route6_2_nh, 56, 1);

	/* DNS servers */
	inet_pton (AF_INET6, dns6_1, &addr6);
	nm_setting_ip6_config_add_dns (s_ip6, &addr6);
	inet_pton (AF_INET6, dns6_2, &addr6);
	nm_setting_ip6_config_add_dns (s_ip6, &addr6);

	/* DNS searches */
	nm_setting_ip6_config_add_dns_search (s_ip6, "wallaceandgromit.com");

	/* Write out the connection */
	owner_uid = geteuid ();
	owner_grp = getegid ();
	success = write_connection (connection, TEST_SCRATCH_DIR, owner_uid, owner_grp, &testfile, &error);
	ASSERT (success == TRUE,
			"connection-write", "failed to allocate write keyfile: %s",
			error ? error->message : "(none)");

	ASSERT (testfile != NULL,
			"connection-write", "didn't get keyfile name back after writing connection");

	/* Read the connection back in and compare it to the one we just wrote out */
	reread = connection_from_file (testfile);
	ASSERT (reread != NULL, "connection-write", "failed to re-read test connection");

	ASSERT (nm_connection_compare (connection, reread, NM_SETTING_COMPARE_FLAG_EXACT) == TRUE,
			"connection-write", "written and re-read connection weren't the same");

	g_clear_error (&error);
	unlink (testfile);
	g_free (testfile);

	g_object_unref (reread);
	g_object_unref (connection);
}

#define TEST_WIRED_MAC_CASE_FILE TEST_KEYFILES_DIR"/Test_Wired_Connection_MAC_Case"

static void
test_read_wired_mac_case (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	GError *error = NULL;
	const GByteArray *array;
	char expected_mac_address[ETH_ALEN] = { 0x00, 0x11, 0xaa, 0xbb, 0xcc, 0x55 };
	const char *tmp;
	const char *expected_id = "Test Wired Connection MAC Case";
	const char *expected_uuid = "4e80a56d-c99f-4aad-a6dd-b449bc398c57";

	connection = connection_from_file (TEST_WIRED_MAC_CASE_FILE);
	ASSERT (connection != NULL,
			"connection-read", "failed to read %s", TEST_WIRED_MAC_CASE_FILE);

	ASSERT (nm_connection_verify (connection, &error),
	        "connection-verify", "failed to verify %s: %s", TEST_WIRED_MAC_CASE_FILE, error->message);

	/* ===== CONNECTION SETTING ===== */

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	ASSERT (s_con != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s setting",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME);

	/* ID */
	tmp = nm_setting_connection_get_id (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);
	ASSERT (strcmp (tmp, expected_id) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);

	/* UUID */
	tmp = nm_setting_connection_get_uuid (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);
	ASSERT (strcmp (tmp, expected_uuid) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);

	/* ===== WIRED SETTING ===== */

	s_wired = NM_SETTING_WIRED (nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRED));
	ASSERT (s_wired != NULL,
	        "connection-verify-wired", "failed to verify %s: missing %s setting",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_WIRED_SETTING_NAME);

	/* MAC address */
	array = nm_setting_wired_get_mac_address (s_wired);
	ASSERT (array != NULL,
	        "connection-verify-wired", "failed to verify %s: missing %s / %s key",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);
	ASSERT (array->len == ETH_ALEN,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value length",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);
	ASSERT (memcmp (array->data, &expected_mac_address[0], sizeof (expected_mac_address)) == 0,
	        "connection-verify-wired", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRED_MAC_CASE_FILE,
	        NM_SETTING_WIRED_SETTING_NAME,
	        NM_SETTING_WIRED_MAC_ADDRESS);

	g_object_unref (connection);
}

static void
test_read_valid_wireless_connection (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWireless *s_wireless;
	NMSettingIP4Config *s_ip4;
	GError *error = NULL;
	const GByteArray *array;
	char expected_bssid[ETH_ALEN] = { 0x00, 0x1a, 0x33, 0x44, 0x99, 0x82 };
	const char *tmp;
	const char *expected_id = "Test Wireless Connection";
	const char *expected_uuid = "2f962388-e5f3-45af-a62c-ac220b8f7baa";
	const guint64 expected_timestamp = 1226604314;
	guint64 timestamp;

	connection = connection_from_file (TEST_WIRELESS_FILE);
	ASSERT (connection != NULL,
			"connection-read", "failed to read %s", TEST_WIRELESS_FILE);

	ASSERT (nm_connection_verify (connection, &error),
	        "connection-verify", "failed to verify %s: %s", TEST_WIRELESS_FILE, error->message);

	/* ===== CONNECTION SETTING ===== */

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	ASSERT (s_con != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s setting",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME);

	/* ID */
	tmp = nm_setting_connection_get_id (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);
	ASSERT (strcmp (tmp, expected_id) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_ID);

	/* UUID */
	tmp = nm_setting_connection_get_uuid (s_con);
	ASSERT (tmp != NULL,
	        "connection-verify-connection", "failed to verify %s: missing %s / %s key",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);
	ASSERT (strcmp (tmp, expected_uuid) == 0,
	        "connection-verify-connection", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_UUID);

	/* Timestamp */
	timestamp = nm_setting_connection_get_timestamp (s_con);
	ASSERT (timestamp == expected_timestamp,
	        "connection-verify-connection", "failed to verify %s: unexpected %s /%s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_TIMESTAMP);

	/* Autoconnect */
	ASSERT (nm_setting_connection_get_autoconnect (s_con) == FALSE,
	        "connection-verify-connection", "failed to verify %s: unexpected %s /%s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_CONNECTION_SETTING_NAME,
	        NM_SETTING_CONNECTION_AUTOCONNECT);

	/* ===== WIRED SETTING ===== */

	s_wireless = NM_SETTING_WIRELESS (nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRELESS));
	ASSERT (s_wireless != NULL,
	        "connection-verify-wireless", "failed to verify %s: missing %s setting",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_WIRED_SETTING_NAME);

	/* BSSID */
	array = nm_setting_wireless_get_bssid (s_wireless);
	ASSERT (array != NULL,
	        "connection-verify-wireless", "failed to verify %s: missing %s / %s key",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_WIRELESS_SETTING_NAME,
	        NM_SETTING_WIRELESS_BSSID);
	ASSERT (array->len == ETH_ALEN,
	        "connection-verify-wireless", "failed to verify %s: unexpected %s / %s key value length",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_WIRELESS_SETTING_NAME,
	        NM_SETTING_WIRELESS_BSSID);
	ASSERT (memcmp (array->data, &expected_bssid[0], sizeof (expected_bssid)) == 0,
	        "connection-verify-wireless", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_WIRELESS_SETTING_NAME,
	        NM_SETTING_WIRELESS_BSSID);

	/* ===== IPv4 SETTING ===== */

	s_ip4 = NM_SETTING_IP4_CONFIG (nm_connection_get_setting (connection, NM_TYPE_SETTING_IP4_CONFIG));
	ASSERT (s_ip4 != NULL,
	        "connection-verify-ip4", "failed to verify %s: missing %s setting",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME);

	/* Method */
	tmp = nm_setting_ip4_config_get_method (s_ip4);
	ASSERT (strcmp (tmp, NM_SETTING_IP4_CONFIG_METHOD_AUTO) == 0,
	        "connection-verify-wireless", "failed to verify %s: unexpected %s / %s key value",
	        TEST_WIRELESS_FILE,
	        NM_SETTING_IP4_CONFIG_SETTING_NAME,
	        NM_SETTING_IP4_CONFIG_METHOD);

	g_object_unref (connection);
}

static void
test_write_wireless_connection (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWireless *s_wireless;
	NMSettingIP4Config *s_ip4;
	NMSettingIP6Config *s_ip6;
	char *uuid;
	GByteArray *bssid;
	unsigned char tmpbssid[] = { 0xaa, 0xb9, 0xa1, 0x74, 0x55, 0x44 };
	GByteArray *ssid;
	unsigned char tmpssid[] = { 0x31, 0x33, 0x33, 0x37 };
	gboolean success;
	NMConnection *reread;
	char *testfile = NULL;
	GError *error = NULL;
	pid_t owner_grp;
	uid_t owner_uid;
	guint64 timestamp = 0x12344433L;

	connection = nm_connection_new ();
	ASSERT (connection != NULL,
	        "connection-write", "failed to allocate new connection");

	/* Connection setting */

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	ASSERT (s_con != NULL,
	        "connection-write", "failed to allocate new %s setting",
	        NM_SETTING_CONNECTION_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_con));

	uuid = nm_utils_uuid_generate ();
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_ID, "Work Wireless",
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
	              NM_SETTING_CONNECTION_TIMESTAMP, timestamp,
	              NULL);
	g_free (uuid);

	/* Wireless setting */

	s_wireless = NM_SETTING_WIRELESS (nm_setting_wireless_new ());
	ASSERT (s_wireless != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_WIRELESS_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_wireless));

	bssid = g_byte_array_sized_new (ETH_ALEN);
	g_byte_array_append (bssid, &tmpbssid[0], sizeof (tmpbssid));

	ssid = g_byte_array_sized_new (sizeof (tmpssid));
	g_byte_array_append (ssid, &tmpssid[0], sizeof (tmpssid));

	g_object_set (s_wireless,
	              NM_SETTING_WIRELESS_BSSID, bssid,
	              NM_SETTING_WIRELESS_SSID, ssid,
	              NM_SETTING_WIRED_MTU, 1000,
	              NULL);

	g_byte_array_free (bssid, TRUE);
	g_byte_array_free (ssid, TRUE);

	/* IP4 setting */

	s_ip4 = NM_SETTING_IP4_CONFIG (nm_setting_ip4_config_new ());
	ASSERT (s_ip4 != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_IP4_CONFIG_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_ip4));

	g_object_set (s_ip4,
	              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
	              NULL);

	/* IP6 setting */

	s_ip6 = NM_SETTING_IP6_CONFIG (nm_setting_ip6_config_new ());
	ASSERT (s_ip6 != NULL,
			"connection-write", "failed to allocate new %s setting",
			NM_SETTING_IP6_CONFIG_SETTING_NAME);
	nm_connection_add_setting (connection, NM_SETTING (s_ip6));

	g_object_set (s_ip6,
	              NM_SETTING_IP6_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
	              NULL);

	/* Write out the connection */
	owner_uid = geteuid ();
	owner_grp = getegid ();
	success = write_connection (connection, TEST_SCRATCH_DIR, owner_uid, owner_grp, &testfile, &error);
	ASSERT (success == TRUE,
			"connection-write", "failed to allocate write keyfile: %s",
			error ? error->message : "(none)");

	ASSERT (testfile != NULL,
			"connection-write", "didn't get keyfile name back after writing connection");

	/* Read the connection back in and compare it to the one we just wrote out */
	reread = connection_from_file (testfile);
	ASSERT (reread != NULL, "connection-write", "failed to re-read test connection");

	ASSERT (nm_connection_compare (connection, reread, NM_SETTING_COMPARE_FLAG_EXACT) == TRUE,
			"connection-write", "written and re-read connection weren't the same");

	g_clear_error (&error);
	unlink (testfile);
	g_free (testfile);

	g_object_unref (reread);
	g_object_unref (connection);
}

int main (int argc, char **argv)
{
	GError *error = NULL;
	DBusGConnection *bus;
	char *base;

	g_type_init ();
	bus = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);

	if (!nm_utils_init (&error))
		FAIL ("nm-utils-init", "failed to initialize libnm-util: %s", error->message);

	/* The tests */
	test_read_valid_wired_connection ();
	test_write_wired_connection ();

	test_read_wired_mac_case ();

	test_read_valid_wireless_connection ();
	test_write_wireless_connection ();

	base = g_path_get_basename (argv[0]);
	fprintf (stdout, "%s: SUCCESS\n", base);
	g_free (base);
	dbus_g_connection_unref (bus);
	return 0;
}

