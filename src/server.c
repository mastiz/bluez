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
#include "profile.h"
#include "server.h"

struct btd_server {
	struct btd_adapter	*adapter;
	struct btd_profile	*profile;
	void			*user_data;
};

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

	err = profile->adapter_probe(server->profile, server->adapter);
	if (err == 0)
		return server;

	ba2str(adapter_get_address(server->adapter), addr);
	error("%s profile probe failed for %s (%d)", profile->name, addr, err);

	g_free(server);

	return NULL;
}

void server_destroy(struct btd_server *server)
{
	if (server->profile->adapter_remove != NULL)
		server->profile->adapter_remove(server->profile,
							server->adapter);

	g_free(server);
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
