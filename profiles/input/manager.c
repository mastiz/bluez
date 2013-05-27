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

#include <errno.h>
#include <stdbool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "log.h"
#include "plugin.h"

#include "lib/uuid.h"
#include "../src/adapter.h"
#include "../src/device.h"
#include "../src/profile.h"
#include "../src/service.h"
#include "../src/server.h"

#include "device.h"
#include "server.h"

static struct btd_profile input_profile = {
	.name		= "input-hid",
	.local_uuid	= HID_UUID,
	.remote_uuid	= HID_UUID,

	.auto_connect	= true,
	.connect	= input_device_connect,
	.disconnect	= input_device_disconnect,

	.device_probe	= input_device_register,
	.device_remove	= input_device_unregister,

	.adapter_probe	= hid_server_probe,
	.adapter_remove = hid_server_remove,
};

static GKeyFile *load_config_file(const char *file)
{
	GKeyFile *keyfile;
	GError *err = NULL;

	keyfile = g_key_file_new();

	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			error("Parsing %s failed: %s", file, err->message);
		g_error_free(err);
		g_key_file_free(keyfile);
		return NULL;
	}

	return keyfile;
}

static int input_init(void)
{
	GKeyFile *config;
	GError *err = NULL;

	config = load_config_file(CONFIGDIR "/input.conf");
	if (config) {
		int idle_timeout;

		idle_timeout = g_key_file_get_integer(config, "General",
						"IdleTimeout", &err);
		if (err) {
			DBG("input.conf: %s", err->message);
			g_error_free(err);
		}

		input_set_idle_timeout(idle_timeout * 60);
	}

	btd_profile_register(&input_profile);

	if (config)
		g_key_file_free(config);

	return 0;
}

static void input_exit(void)
{
	btd_profile_unregister(&input_profile);
}

BLUETOOTH_PLUGIN_DEFINE(input, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
							input_init, input_exit)
