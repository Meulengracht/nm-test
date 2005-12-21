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

#ifndef NM_VPN_SERVICE_H
#define NM_VPN_SERVICE_H


#include <dbus/dbus.h>
#include "NetworkManager.h"


typedef struct NMVPNService NMVPNService;

NMVPNService *		nm_vpn_service_new				(void);

void				nm_vpn_service_ref				(NMVPNService *service);
void				nm_vpn_service_unref			(NMVPNService *service);

const char *		nm_vpn_service_get_name			(NMVPNService *service);
void				nm_vpn_service_set_name			(NMVPNService *service, const char *name);

const char *		nm_vpn_service_get_service_name	(NMVPNService *service);
void				nm_vpn_service_set_service_name	(NMVPNService *service, const char *name);

const char *		nm_vpn_service_get_program		(NMVPNService *service);
void				nm_vpn_service_set_program		(NMVPNService *service, const char *program);

NMVPNState		nm_vpn_service_get_state			(NMVPNService *service);
void				nm_vpn_service_set_state			(NMVPNService *service, const NMVPNState state);

gboolean			nm_vpn_service_exec_daemon		(NMVPNService *service);

#endif
