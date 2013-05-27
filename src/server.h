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

#include "btio/btio.h"

struct btd_adapter;
struct btd_profile;
struct btd_connection;

/* Server management functions used by the core */
struct btd_server *server_create(struct btd_adapter *adapter,
						struct btd_profile *profile);
void server_destroy(struct btd_server *server);

/* Public member access */
struct btd_adapter *btd_server_get_adapter(const struct btd_server *server);
struct btd_profile *btd_server_get_profile(const struct btd_server *server);

/* Functions used by profile implementation */
void btd_server_set_user_data(struct btd_server *server, void *user_data);
void *btd_server_get_user_data(const struct btd_server *server);

/* Socket handling helper API */
typedef int (*btd_server_accept_cb) (struct btd_connection *conn);
typedef void (*btd_server_disconn_cb) (struct btd_connection *conn);

GIOChannel *btd_server_listen(struct btd_server *server, bool authorize,
					btd_server_accept_cb accept_cb,
					btd_server_disconn_cb disconn_cb,
					BtIOOption opt1, ...);
