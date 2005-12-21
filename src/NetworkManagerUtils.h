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

#ifndef NETWORK_MANAGER_UTILS_H
#define NETWORK_MANAGER_UTILS_H

#include <glib.h>
#include <stdio.h>
#include <syslog.h>
#include <net/ethernet.h>
#include <iwlib.h>
#include <sys/time.h>
#include <stdarg.h>

#include "NetworkManager.h"
#include "NetworkManagerMain.h"
#include "NetworkManagerDevice.h"

typedef enum SockType
{
	DEV_WIRELESS,
	DEV_GENERAL,
	NETWORK_CONTROL
} SockType;

typedef struct NMSock NMSock;


gboolean				nm_try_acquire_mutex			(GMutex *mutex, const char *func);
void					nm_lock_mutex					(GMutex *mutex, const char *func);
void					nm_unlock_mutex				(GMutex *mutex, const char *func);
void					nm_register_mutex_desc			(GMutex *mutex, const char *string);

NMSock *				nm_dev_sock_open				(NMDevice *dev, SockType type, const char *func_name, const char *desc);
void					nm_dev_sock_close				(NMSock *sock);
int					nm_dev_sock_get_fd				(NMSock *sock);
void					nm_print_open_socks				(void);

int					nm_null_safe_strcmp				(const char *s1, const char *s2);

gboolean				nm_ethernet_address_is_valid		(const struct ether_addr *test_addr);

void					nm_dispose_scan_results			(wireless_scan *result_list);

int					nm_spawn_process				(const char *args);

NMDriverSupportLevel	nm_get_driver_support_level		(LibHalContext *ctx, NMDevice *dev);

#define NM_COMPLETION_TRIES_INFINITY -1

typedef void * nm_completion_args[8];

typedef gboolean (*nm_completion_func)(int tries, nm_completion_args args);
typedef gboolean (*nm_completion_boolean_function_1)(u_int64_t arg);
typedef gboolean (*nm_completion_boolean_function_2)(
	u_int64_t arg0, u_int64_t arg1);

void nm_wait_for_completion(
	const int max_tries,
	const guint interval_usecs,
	nm_completion_func test_func,
	nm_completion_func action_func,
	nm_completion_args args);

void nm_wait_for_completion_or_timeout(
	const int max_tries,
	const struct timeval *max_time,
	const guint interval_usecs,
	nm_completion_func test_func,
	nm_completion_func action_func,
	nm_completion_args args);

void nm_wait_for_timeout(
	const struct timeval *max_time,
	const guint interval_usecs,
	nm_completion_func test_func,
	nm_completion_func action_func,
	nm_completion_args args);

gboolean nm_completion_boolean_test(int tries, nm_completion_args args);
gboolean nm_completion_boolean_function1_test(int tries,
		nm_completion_args args);
gboolean nm_completion_boolean_function2_test(int tries,
		nm_completion_args args);
#define nm_completion_boolean_function_test nm_completion_boolean_function1_test

#endif
