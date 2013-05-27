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

struct btd_service {
	int			ref;
	struct btd_device	*device;
	struct btd_profile	*profile;
	void			*user_data;
	btd_service_state_t	state;
	int			err;
	GSList			*conns;
};

struct service_state_callback {
	btd_service_state_cb	cb;
	void			*user_data;
	unsigned int		id;
};

struct btd_connection {
	struct btd_server	*server;
	struct btd_service	*service;
	GIOChannel		*io;
	bool			connected;
	uint16_t		psm;
	uint8_t			chan;
	guint			io_watch;
	unsigned int		svc_id;
	guint			auth_id;
	btd_connection_connect_cb connect_cb;
	btd_connection_disconn_cb disconn_cb;
	void			*user_data;
};

static GSList *state_callbacks = NULL;

static const char *state2str(btd_service_state_t state)
{
	switch (state) {
	case BTD_SERVICE_STATE_UNAVAILABLE:
		return "unavailable";
	case BTD_SERVICE_STATE_DISCONNECTED:
		return "disconnected";
	case BTD_SERVICE_STATE_CONNECTING:
		return "connecting";
	case BTD_SERVICE_STATE_CONNECTED:
		return "connected";
	case BTD_SERVICE_STATE_DISCONNECTING:
		return "disconnecting";
	}

	return NULL;
}

static void connection_free(gpointer data)
{
	struct btd_connection *conn = data;
	struct btd_service *service = conn->service;

	if (conn->auth_id != 0)
		btd_cancel_authorization(conn->auth_id);

	if (conn->svc_id != 0)
		device_remove_svc_complete_callback(service->device,
								conn->svc_id);

	if (conn->io_watch != 0)
		g_source_remove(conn->io_watch);

	if (conn->io != NULL)
		g_io_channel_shutdown(conn->io, FALSE, NULL);

	if (!conn->connected && conn->connect_cb != NULL)
		conn->connect_cb(conn, -EIO);

	if (conn->connected && conn->disconn_cb != NULL)
		conn->disconn_cb(conn);

	if (conn->io != NULL)
		g_io_channel_unref(conn->io);

	g_free(conn);
}

static void change_state(struct btd_service *service, btd_service_state_t state,
									int err)
{
	btd_service_state_t old = service->state;
	char addr[18];
	GSList *l;

	if (state == old)
		return;

	assert(service->device != NULL);
	assert(service->profile != NULL);

	service->state = state;
	service->err = err;

	ba2str(device_get_address(service->device), addr);
	DBG("%p: device %s profile %s state changed: %s -> %s (%d)", service,
					addr, service->profile->name,
					state2str(old), state2str(state), err);

	for (l = state_callbacks; l != NULL; l = g_slist_next(l)) {
		struct service_state_callback *cb = l->data;

		cb->cb(service, old, state, cb->user_data);
	}
}

struct btd_service *btd_service_ref(struct btd_service *service)
{
	service->ref++;

	DBG("%p: ref=%d", service, service->ref);

	return service;
}

void btd_service_unref(struct btd_service *service)
{
	service->ref--;

	DBG("%p: ref=%d", service, service->ref);

	if (service->ref > 0)
		return;

	g_free(service);
}

struct btd_service *service_create(struct btd_device *device,
						struct btd_profile *profile)
{
	struct btd_service *service;

	service = g_try_new0(struct btd_service, 1);
	if (!service) {
		error("service_create: failed to alloc memory");
		return NULL;
	}

	service->ref = 1;
	service->device = device; /* Weak ref */
	service->profile = profile;
	service->state = BTD_SERVICE_STATE_UNAVAILABLE;

	return service;
}

int service_probe(struct btd_service *service)
{
	char addr[18];
	int err;

	assert(service->state == BTD_SERVICE_STATE_UNAVAILABLE);

	err = service->profile->device_probe(service);
	if (err == 0) {
		change_state(service, BTD_SERVICE_STATE_DISCONNECTED, 0);
		return 0;
	}

	ba2str(device_get_address(service->device), addr);
	error("%s profile probe failed for %s", service->profile->name, addr);

	return err;
}

void service_shutdown(struct btd_service *service)
{
	change_state(service, BTD_SERVICE_STATE_UNAVAILABLE, 0);

	g_slist_free_full(service->conns, connection_free);
	service->conns = NULL;

	service->profile->device_remove(service);
	service->device = NULL;
	service->profile = NULL;
}

int btd_service_connect(struct btd_service *service)
{
	struct btd_profile *profile = service->profile;
	char addr[18];
	int err;

	if (!profile->connect)
		return -ENOTSUP;

	switch (service->state) {
	case BTD_SERVICE_STATE_UNAVAILABLE:
		return -EINVAL;
	case BTD_SERVICE_STATE_DISCONNECTED:
		break;
	case BTD_SERVICE_STATE_CONNECTING:
	case BTD_SERVICE_STATE_CONNECTED:
		return -EALREADY;
	case BTD_SERVICE_STATE_DISCONNECTING:
		return -EBUSY;
	}

	change_state(service, BTD_SERVICE_STATE_CONNECTING, 0);

	err = profile->connect(service);
	if (err == 0)
		return 0;

	ba2str(device_get_address(service->device), addr);
	error("%s profile connect failed for %s: %s", profile->name, addr,
								strerror(-err));

	btd_service_connecting_complete(service, err);

	return err;
}

int btd_service_disconnect(struct btd_service *service)
{
	struct btd_profile *profile = service->profile;
	char addr[18];
	int err;

	if (!profile->disconnect)
		return -ENOTSUP;

	switch (service->state) {
	case BTD_SERVICE_STATE_UNAVAILABLE:
		return -EINVAL;
	case BTD_SERVICE_STATE_DISCONNECTED:
	case BTD_SERVICE_STATE_DISCONNECTING:
		return -EALREADY;
	case BTD_SERVICE_STATE_CONNECTING:
	case BTD_SERVICE_STATE_CONNECTED:
		break;
	}

	change_state(service, BTD_SERVICE_STATE_DISCONNECTING, 0);

	err = profile->disconnect(service);
	if (err == 0)
		return 0;

	if (err == -ENOTCONN) {
		btd_service_disconnecting_complete(service, 0);
		return 0;
	}

	ba2str(device_get_address(service->device), addr);
	error("%s profile disconnect failed for %s: %s", profile->name, addr,
								strerror(-err));

	btd_service_disconnecting_complete(service, err);

	return err;
}

struct btd_device *btd_service_get_device(const struct btd_service *service)
{
	return service->device;
}

struct btd_profile *btd_service_get_profile(const struct btd_service *service)
{
	return service->profile;
}

void btd_service_set_user_data(struct btd_service *service, void *user_data)
{
	assert(service->state == BTD_SERVICE_STATE_UNAVAILABLE);
	service->user_data = user_data;
}

void *btd_service_get_user_data(const struct btd_service *service)
{
	return service->user_data;
}

btd_service_state_t btd_service_get_state(const struct btd_service *service)
{
	return service->state;
}

int btd_service_get_error(const struct btd_service *service)
{
	return service->err;
}

unsigned int btd_service_add_state_cb(btd_service_state_cb cb, void *user_data)
{
	struct service_state_callback *state_cb;
	static unsigned int id = 0;

	state_cb = g_new0(struct service_state_callback, 1);
	state_cb->cb = cb;
	state_cb->user_data = user_data;
	state_cb->id = ++id;

	state_callbacks = g_slist_append(state_callbacks, state_cb);

	return state_cb->id;
}

bool btd_service_remove_state_cb(unsigned int id)
{
	GSList *l;

	for (l = state_callbacks; l != NULL; l = g_slist_next(l)) {
		struct service_state_callback *cb = l->data;

		if (cb && cb->id == id) {
			state_callbacks = g_slist_remove(state_callbacks, cb);
			g_free(cb);
			return true;
		}
	}

	return false;
}

void btd_service_connecting_complete(struct btd_service *service, int err)
{
	if (service->state != BTD_SERVICE_STATE_DISCONNECTED &&
				service->state != BTD_SERVICE_STATE_CONNECTING)
		return;

	if (err == 0)
		change_state(service, BTD_SERVICE_STATE_CONNECTED, 0);
	else
		change_state(service, BTD_SERVICE_STATE_DISCONNECTED, err);
}

void btd_service_disconnecting_complete(struct btd_service *service, int err)
{
	if (service->state != BTD_SERVICE_STATE_CONNECTED &&
			service->state != BTD_SERVICE_STATE_DISCONNECTING)
		return;

	/* If disconnect fails, we assume it remains connected */
	if (err < 0) {
		change_state(service, BTD_SERVICE_STATE_CONNECTED, err);
		return;
	}

	g_slist_free_full(service->conns, connection_free);
	service->conns = NULL;

	change_state(service, BTD_SERVICE_STATE_DISCONNECTED, 0);
}

static struct btd_connection *connection_add(struct btd_server *server,
					struct btd_service *service,
					btd_connection_connect_cb connect_cb,
					btd_connection_disconn_cb disconn_cb)
{
	struct btd_connection *conn;

	conn = g_new0(struct btd_connection, 1);
	conn->server = server;
	conn->service = service;
	conn->connect_cb = connect_cb;
	conn->disconn_cb = disconn_cb;

	service->conns = g_slist_prepend(service->conns, conn);

	return conn;
}

static void connection_remove(struct btd_connection *conn)
{
	conn->service->conns = g_slist_remove(conn->service->conns, conn);
	connection_free(conn);
}

static gboolean connection_disconnected(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct btd_connection *conn = user_data;
	struct btd_service *service = conn->service;
	struct btd_profile *profile = service->profile;
	char addr[18];

	if (cond & G_IO_NVAL)
		return FALSE;

	ba2str(device_get_address(service->device), addr);
	DBG("%s: connection closed from %s", profile->name, addr);

	connection_remove(conn);

	return FALSE;
}

static void connection_set_io(struct btd_connection *conn, GIOChannel *io)
{
	conn->io = g_io_channel_ref(io);
	conn->io_watch = g_io_add_watch(io, G_IO_HUP | G_IO_ERR | G_IO_NVAL,
					connection_disconnected, conn);

	bt_io_get(io, NULL, BT_IO_OPT_PSM, &conn->psm, BT_IO_OPT_INVALID);
	bt_io_get(io, NULL, BT_IO_OPT_CHANNEL, &conn->chan, BT_IO_OPT_INVALID);
}

static void connection_connected(GIOChannel *io, GError *err,
							gpointer user_data)
{
	struct btd_connection *conn = user_data;
	struct btd_service *service = conn->service;
	const char *name = service->profile->name;
	char addr[18];

	ba2str(device_get_address(service->device), addr);

	if (err != NULL) {
		error("%s connect failed to %s: %s", name, addr, err->message);
		connection_remove(conn);
	}

	DBG("%s: connected to %s", name, addr);

	if (conn->connect_cb == NULL)
		return;

	conn->connected = true;
	conn->connect_cb(conn, 0);
}

static void connection_accept(struct btd_connection *conn, const char *addr)
{
	GError *err = NULL;

	if (!bt_io_accept(conn->io, connection_connected, conn, NULL, &err)) {
		error("bt_io_accept: %s", err->message);
		g_error_free(err);
		connection_remove(conn);
		return;
	}

	DBG("%s: accepted connection from %s", conn->service->profile->name,
									addr);
}

static void connection_svc_complete(struct btd_device *device, int err,
								void *user_data)
{
	struct btd_connection *conn = user_data;
	char addr[18];

	conn->svc_id = 0;

	ba2str(device_get_address(device), addr);

	if (err < 0) {
		error("Service resolving failed for %s: %s (%d)",
						addr, strerror(-err), -err);
		connection_remove(conn);
		return;
	}

	if (conn->auth_id == 0)
		connection_accept(conn, addr);
	else
		DBG("Services from %s resolved but waiting for authorization",
									addr);
}

static void connection_auth(DBusError *err, void *user_data)
{
	struct btd_connection *conn = user_data;
	struct btd_service *service = conn->service;
	char addr[18];

	conn->auth_id = 0;

	ba2str(device_get_address(service->device), addr);

	if (err && dbus_error_is_set(err)) {
		error("%s rejected %s: %s", service->profile->name, addr,
								err->message);
		connection_remove(conn);
		return;
	}

	if (conn->svc_id == 0)
		connection_accept(conn, addr);
	else
		DBG("%s: connection from %s authorized but waiting for SDP",
						service->profile->name, addr);
}

struct btd_connection *btd_service_incoming_conn(
					struct btd_server *server,
					struct btd_service *service,
					GIOChannel *io, bool authorize,
					btd_connection_connect_cb connect_cb,
					btd_connection_disconn_cb disconn_cb)
{
	struct btd_connection *conn;
	struct btd_device *device = service->device;
	const char *uuid = service->profile->remote_uuid;
	const bdaddr_t *src;
	const bdaddr_t *dst;
	char addr[18];

	src = adapter_get_address(device_get_adapter(device));
	dst = device_get_address(device);
	ba2str(dst, addr);

	conn = connection_add(server, service, connect_cb, disconn_cb);
	connection_set_io(conn, io);
	conn->svc_id = device_wait_for_svc_complete(device,
							connection_svc_complete,
							conn);

	if (!authorize)
		return conn;

	DBG("%s: authorizing connection from %s", service->profile->name, addr);

	conn->auth_id = btd_request_authorization(src, dst, uuid,
							connection_auth,
							conn);

	if (conn->auth_id != 0)
		return conn;

	error("%s: authorization failure", service->profile->name);
	connection_remove(conn);

	return NULL;
}

struct btd_server *btd_connection_get_server(struct btd_connection *conn)
{
	return conn->server;
}

struct btd_service *btd_connection_get_service(struct btd_connection *conn)
{
	return conn->service;
}

GIOChannel *btd_connection_get_io(struct btd_connection *conn)
{
	return conn->io;
}

void btd_connection_set_user_data(struct btd_connection *conn, void *user_data)
{
	conn->user_data = user_data;
}

void *btd_connection_get_user_data(const struct btd_connection *conn)
{
	return conn->user_data;
}

const bdaddr_t *btd_connection_get_src(const struct btd_connection *conn)
{
	return adapter_get_address(device_get_adapter(conn->service->device));
}

const bdaddr_t *btd_connection_get_dst(const struct btd_connection *conn)
{
	return device_get_address(conn->service->device);
}

uint16_t btd_connection_get_psm(const struct btd_connection *conn)
{
	return conn->psm;
}

uint8_t btd_connection_get_channel(const struct btd_connection *conn)
{
	return conn->chan;
}
