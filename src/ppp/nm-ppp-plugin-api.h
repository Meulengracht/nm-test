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
 * Copyright (C) 2016 Red Hat, Inc.
 */

#ifndef __NM_PPP_PLUGIN_API_H__
#define __NM_PPP_PLUGIN_API_H__

typedef const struct {
	NMPPPManager *(*create) (const char *iface);

	gboolean      (*start) (NMPPPManager *manager,
	                        NMActRequest *req,
	                        const char *ppp_name,
	                        guint32 timeout_secs,
	                        guint baud_override,
	                        GError **err);

	void          (*stop_async) (NMPPPManager *manager,
	                             GCancellable *cancellable,
	                             GAsyncReadyCallback callback,
	                             gpointer user_data);

	gboolean      (*stop_finish) (NMPPPManager *manager,
	                              GAsyncResult *res,
	                              GError **error);

	void          (*stop_sync) (NMPPPManager *manager);
} NMPPPOps;

#endif /* __NM_PPP_PLUGIN_API_H__ */
