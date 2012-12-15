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
	gint			ref;
	struct btd_device	*device;
	struct btd_profile	*profile;
	btd_service_state_t	state;
};

static const char *state2str(btd_service_state_t state)
{
	switch (state) {
	case BTD_SERVICE_STATE_UNAVAILABLE:
		return "unavailable";
	case BTD_SERVICE_STATE_PROBED:
		return "probed";
	}

	return NULL;
}

static void service_set_state(struct btd_service *service,
						btd_service_state_t state)
{
	btd_service_state_t old = service->state;

	if (state == old)
		return;

	service->state = state;

	DBG("State changed %p: %s -> %s", service, state2str(old),
							state2str(state));
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

void service_probed(struct btd_service *service)
{
	assert(service->state == BTD_SERVICE_STATE_UNAVAILABLE);
	service_set_state(service, BTD_SERVICE_STATE_PROBED);
}

void service_unavailable(struct btd_service *service)
{
	service->device = NULL;
	service->profile = NULL;
	service_set_state(service, BTD_SERVICE_STATE_UNAVAILABLE);
}

struct btd_device *btd_service_get_device(const struct btd_service *service)
{
	return service->device;
}

struct btd_profile *btd_service_get_profile(const struct btd_service *service)
{
	return service->profile;
}

btd_service_state_t btd_service_get_state(const struct btd_service *service)
{
	return service->state;
}
