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
 * (C) Copyright 2004 Red Hat, Inc.
 */

#ifndef NETWORK_MANAGER_WIRELESS_H
#define NETWORK_MANAGER_WIRELESS_H

#include <iwlib.h>
#include "NetworkManager.h"
#include "NetworkManagerDevice.h"
#include "NetworkManagerAPList.h"


char *	nm_wireless_64bit_ascii_to_hex		(const unsigned char *ascii);
char *	nm_wireless_128bit_ascii_to_hex		(const unsigned char *ascii);
char *	nm_wireless_128bit_key_from_passphrase	(const char *passphrase);

int		nm_wireless_qual_to_percent			(const struct iw_quality *qual,
										 const struct iw_quality *max_qual,
										 const struct iw_quality *avg_qual);
void nm_wireless_set_scan_interval	(NMData *data, NMDevice *dev, NMWirelessScanInterval interval);

#endif
