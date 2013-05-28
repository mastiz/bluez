/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <dbus/dbus.h>

#include "log.h"

#include "glib-helper.h"
#include "btio/btio.h"
#include "lib/uuid.h"
#include "../src/adapter.h"
#include "../src/device.h"
#include "../src/profile.h"
#include "../src/service.h"
#include "../src/server.h"

#include "device.h"
#include "server.h"

static int hid_accept_cb(struct btd_connection *conn)
{
	const bdaddr_t *src = btd_connection_get_src(conn);
	const bdaddr_t *dst = btd_connection_get_dst(conn);
	uint16_t psm = btd_connection_get_psm(conn);
	GIOChannel *chan = btd_connection_get_io(conn);
	int ret;

	ret = input_device_set_channel(src, dst, psm, chan);
	if (ret == 0)
		return 0;

	error("Refusing input device connect: %s (%d)", strerror(-ret), -ret);

	/* Send unplug virtual cable to unknown devices */
	if (ret == -ENOENT && psm == L2CAP_PSM_HIDP_CTRL) {
		unsigned char unplug = 0x15;
		int sk = g_io_channel_unix_get_fd(chan);
		if (write(sk, &unplug, sizeof(unplug)) < 0)
			error("Unable to send virtual cable unplug");
	}

	return -EIO;
}

static void hid_disconn_cb(struct btd_connection *conn)
{
	const bdaddr_t *src = btd_connection_get_src(conn);
	const bdaddr_t *dst = btd_connection_get_dst(conn);

	input_device_close_channels(src, dst);
}

int hid_server_probe(struct btd_server *btd_server)
{
	if (btd_server_listen(btd_server, false, hid_accept_cb, hid_disconn_cb,
				BT_IO_OPT_PSM, L2CAP_PSM_HIDP_CTRL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID) == NULL) {
		error("Failed to listen on control channel");
		return -1;
	}

	if (btd_server_listen(btd_server, true, hid_accept_cb, hid_disconn_cb,
				BT_IO_OPT_PSM, L2CAP_PSM_HIDP_INTR,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID) == NULL) {
		error("Failed to listen on interrupt channel");
		return -1;
	}

	return 0;
}

void hid_server_remove(struct btd_server *btd_server)
{
}
