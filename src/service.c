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
	void			*user_data;
	btd_service_state_t	state;
	int			err;
};

struct service_state_callback {
	btd_service_state_cb	cb;
	void			*user_data;
	guint			id;
};

static GSList *state_callbacks;

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

static void service_set_state(struct btd_service *service,
						btd_service_state_t state)
{
	btd_service_state_t old = service->state;
	GSList *l;

	if (state == old)
		return;

	service->state = state;

	DBG("State changed %p: %s -> %s", service, state2str(old),
							state2str(state));

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

void service_probed(struct btd_service *service)
{
	assert(service->state == BTD_SERVICE_STATE_UNAVAILABLE);
	service_set_state(service, BTD_SERVICE_STATE_DISCONNECTED);
}

void service_connecting(struct btd_service *service)
{
	assert(service->state == BTD_SERVICE_STATE_DISCONNECTED);
	service->err = 0;
	service_set_state(service, BTD_SERVICE_STATE_CONNECTING);
}

void service_disconnecting(struct btd_service *service)
{
	assert(service->state == BTD_SERVICE_STATE_CONNECTING ||
				service->state == BTD_SERVICE_STATE_CONNECTED);
	service->err = 0;
	service_set_state(service, BTD_SERVICE_STATE_DISCONNECTING);
}

void service_unavailable(struct btd_service *service)
{
	service->device = NULL;
	service->profile = NULL;
	service->err = 0;
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

void btd_service_set_user_data(struct btd_service *service, void *user_data)
{
	assert(service->state == BTD_SERVICE_STATE_UNAVAILABLE);
	service->user_data = user_data;
}

void *btd_service_get_user_data(const struct btd_service *service)
{
	return service->user_data;
}

int btd_service_get_error(const struct btd_service *service)
{
	return service->err;
}

btd_service_state_t btd_service_get_state(const struct btd_service *service)
{
	return service->state;
}

guint btd_service_add_state_cb(btd_service_state_cb cb, void *user_data)
{
	struct service_state_callback *state_cb;
	static guint id = 0;

	state_cb = g_new0(struct service_state_callback, 1);
	state_cb->cb = cb;
	state_cb->user_data = user_data;
	state_cb->id = ++id;

	state_callbacks = g_slist_append(state_callbacks, state_cb);

	return state_cb->id;
}

gboolean btd_service_remove_state_cb(guint id)
{
	GSList *l;

	for (l = state_callbacks; l != NULL; l = g_slist_next(l)) {
		struct service_state_callback *cb = l->data;

		if (cb && cb->id == id) {
			state_callbacks = g_slist_remove_link(state_callbacks,
									l);
			g_free(cb);
			return TRUE;
		}
	}

	return FALSE;
}

void btd_service_connecting_complete(struct btd_service *service, int err)
{
	if (service->state != BTD_SERVICE_STATE_DISCONNECTED &&
				service->state != BTD_SERVICE_STATE_CONNECTING)
		return;

	service->err = err;

	if (err == 0)
		service_set_state(service, BTD_SERVICE_STATE_CONNECTED);
	else
		service_set_state(service, BTD_SERVICE_STATE_DISCONNECTED);
}

void btd_service_disconnecting_complete(struct btd_service *service, int err)
{
	if (service->state != BTD_SERVICE_STATE_CONNECTED &&
			service->state != BTD_SERVICE_STATE_DISCONNECTING)
		return;

	service->err = err;
	service_set_state(service, BTD_SERVICE_STATE_DISCONNECTED);
}
