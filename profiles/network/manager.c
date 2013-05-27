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

#include <stdbool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bnep.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <gdbus/gdbus.h>

#include "log.h"
#include "plugin.h"

#include "lib/uuid.h"
#include "adapter.h"
#include "device.h"
#include "profile.h"
#include "service.h"
#include "../src/server.h"
#include "common.h"
#include "connection.h"
#include "server.h"

static gboolean conf_security = TRUE;

static void read_config(const char *file)
{
	GKeyFile *keyfile;
	GError *err = NULL;

	keyfile = g_key_file_new();

	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		g_clear_error(&err);
		goto done;
	}

	conf_security = !g_key_file_get_boolean(keyfile, "General",
						"DisableSecurity", &err);
	if (err) {
		DBG("%s: %s", file, err->message);
		g_clear_error(&err);
	}

done:
	g_key_file_free(keyfile);

	DBG("Config options: Security=%s",
				conf_security ? "true" : "false");
}

static struct btd_profile panu_profile = {
	.name		= "network-panu",
	.local_uuid	= NAP_UUID,
	.remote_uuid	= PANU_UUID,
	.device_probe	= connection_register,
	.device_remove	= connection_unregister,
	.connect	= connection_connect,
	.disconnect	= connection_disconnect,
	.adapter_probe	= network_server_probe,
	.adapter_remove	= network_server_remove,
};

static struct btd_profile gn_profile = {
	.name		= "network-gn",
	.local_uuid	= PANU_UUID,
	.remote_uuid	= GN_UUID,
	.device_probe	= connection_register,
	.device_remove	= connection_unregister,
	.connect	= connection_connect,
	.disconnect	= connection_disconnect,
	.adapter_probe	= network_server_probe,
	.adapter_remove	= network_server_remove,
};

static struct btd_profile nap_profile = {
	.name		= "network-nap",
	.local_uuid	= PANU_UUID,
	.remote_uuid	= NAP_UUID,
	.device_probe	= connection_register,
	.device_remove	= connection_unregister,
	.connect	= connection_connect,
	.disconnect	= connection_disconnect,
	.adapter_probe	= network_server_probe,
	.adapter_remove	= network_server_remove,
};

static int network_init(void)
{
	read_config(CONFIGDIR "/network.conf");

	if (bnep_init()) {
		error("Can't init bnep module");
		return -1;
	}

	/*
	 * There is one socket to handle the incoming connections. NAP,
	 * GN and PANU servers share the same PSM. The initial BNEP message
	 * (setup connection request) contains the destination service
	 * field that defines which service the source is connecting to.
	 */

	if (server_init(conf_security) < 0)
		return -1;

	btd_profile_register(&panu_profile);
	btd_profile_register(&gn_profile);
	btd_profile_register(&nap_profile);

	return 0;
}

static void network_exit(void)
{
	server_exit();

	btd_profile_unregister(&panu_profile);
	btd_profile_unregister(&gn_profile);
	btd_profile_unregister(&nap_profile);

	bnep_cleanup();
}

BLUETOOTH_PLUGIN_DEFINE(network, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT, network_init, network_exit)
