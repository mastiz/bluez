/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012-2013  BMW Car IT GmbH. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>

#include <glib.h>

#include "log.h"

#include "adapter.h"
#include "device.h"
#include "profile.h"
#include "service.h"
#include "server.h"

struct btd_server {
	struct btd_adapter	*adapter;
	struct btd_profile	*profile;
	void			*user_data;
	GSList			*sockets;
};

struct server_socket {
	struct btd_server	*server;
	bool			authorize;
	btd_server_accept_cb	accept_cb;
	btd_server_disconn_cb	disconn_cb;
	GIOChannel		*io;
};

static void server_socket_free(gpointer user_data)
{
	struct server_socket *s = user_data;

	if (s->io != NULL) {
		g_io_channel_shutdown(s->io, TRUE, NULL);
		g_io_channel_unref(s->io);
	}

	g_free(s);
}

static void server_free(struct btd_server *server)
{
	g_slist_free_full(server->sockets, server_socket_free);
	g_free(server);
}

struct btd_server *server_create(struct btd_adapter *adapter,
						struct btd_profile *profile)
{
	struct btd_server *server;
	char addr[18];
	int err;

	server = g_try_new0(struct btd_server, 1);
	if (!server) {
		error("server_create: failed to alloc memory");
		return NULL;
	}

	server->adapter = adapter; /* Weak ref */
	server->profile = profile;

	err = profile->adapter_probe(server);
	if (err == 0)
		return server;

	ba2str(adapter_get_address(server->adapter), addr);
	error("%s profile probe failed for %s (%d)", profile->name, addr, err);

	server_free(server);

	return NULL;
}

void server_destroy(struct btd_server *server)
{
	if (server->profile->adapter_remove != NULL)
		server->profile->adapter_remove(server);

	server_free(server);
}

struct btd_adapter *btd_server_get_adapter(const struct btd_server *server)
{
	return server->adapter;
}

struct btd_profile *btd_server_get_profile(const struct btd_server *server)
{
	return server->profile;
}

void btd_server_set_user_data(struct btd_server *server, void *user_data)
{
	server->user_data = user_data;
}

void *btd_server_get_user_data(const struct btd_server *server)
{
	return server->user_data;
}

static void server_connect_cb(struct btd_connection *conn, int err)
{
	struct server_socket *s = btd_connection_get_user_data(conn);

	if (err < 0)
		return;

	/* Leave user_data in the hands of the client code */
	btd_connection_set_user_data(conn, NULL);

	if (s->accept_cb(conn) >= 0)
		return;

	s->disconn_cb = NULL;
	g_io_channel_shutdown(btd_connection_get_io(conn), FALSE, NULL);
}

static void connect_event_cb(GIOChannel *io, GError *err, gpointer data)
{
	struct server_socket *s = data;
	struct btd_server *server = s->server;
	struct btd_device *device;
	struct btd_service *service;
	struct btd_connection *conn;
	const char *uuid = server->profile->remote_uuid;
	GError *gerr = NULL;
	bdaddr_t src, dst;
	char addr[18];

	bt_io_get(io, &gerr, BT_IO_OPT_SOURCE_BDADDR, &src,
					BT_IO_OPT_DEST_BDADDR, &dst,
					BT_IO_OPT_DEST, addr,
					BT_IO_OPT_INVALID);
	if (gerr != NULL) {
		error("Failed to get connect data: %s", gerr->message);
		g_error_free(gerr);
		return;
	}

	DBG("%s: incoming connect from %s", server->profile->name, addr);

	device = adapter_find_device(server->adapter, &dst);
	if (device == NULL) {
		error("Device %s not found", addr);
		return;
	}

	btd_device_add_uuid(device, uuid);

	service = btd_device_get_service(device, uuid);
	if (service == NULL) {
		error("%s: no service found for %s", server->profile->name,
									addr);
		return;
	}

	/* During the authorization phase, use user_data for server_socket */
	conn = btd_service_incoming_conn(server, service, io, s->authorize,
					server_connect_cb, s->disconn_cb);

	btd_connection_set_user_data(conn, s);
}

static struct server_socket *server_socket_create(struct btd_server *server,
					bool authorize,
					btd_server_accept_cb accept_cb,
					btd_server_disconn_cb disconn_cb)
{
	struct server_socket *s;

	s = g_new0(struct server_socket, 1);
	s->server = server;
	s->authorize = authorize;
	s->accept_cb = accept_cb;
	s->disconn_cb = disconn_cb;

	return s;
}

GIOChannel *btd_server_listen(struct btd_server *server, bool authorize,
					btd_server_accept_cb accept_cb,
					btd_server_disconn_cb disconn_cb,
					BtIOOption opt1, ...)
{
	const bdaddr_t *src = adapter_get_address(server->adapter);
	struct server_socket *s;
	struct BtIOSetOpts *opts;
	const char *name = server->profile->name;
	GError *err = NULL;
	va_list args;
	uint16_t psm = 0;
	uint8_t chan = 0;

	s = server_socket_create(server, authorize, accept_cb, disconn_cb);

	va_start(args, opt1);
	opts = bt_io_set_opts_new();
	bt_io_set_opts_parse_valist(opts, &err, opt1, args);
	va_end(args);

	bt_io_set_opts_parse(opts, &err, BT_IO_OPT_SOURCE_BDADDR, src,
							BT_IO_OPT_INVALID);

	s->io = bt_io_listen_opts(connect_event_cb, NULL, s, NULL, &err, opts);
	g_free(opts);

	if (s->io == NULL) {
		if (err != NULL)
			error("%s: failed to listen on socket: %s", name,
								err->message);
		else
			error("%s: failed to listen on socket", name);

		server_socket_free(s);
		g_clear_error(&err);
		return NULL;
	}

	bt_io_get(s->io, NULL, BT_IO_OPT_PSM, &psm, BT_IO_OPT_INVALID);
	bt_io_get(s->io, NULL, BT_IO_OPT_CHANNEL, &chan, BT_IO_OPT_INVALID);

	if (psm != 0)
		DBG("%s: listening on PSM %u", name, psm);
	else
		DBG("%s: listening on RFCOMM channel %u", name, chan);

	server->sockets = g_slist_prepend(server->sockets, s);

	return s->io;
}
