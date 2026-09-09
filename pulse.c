#include "pulse.h"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	MIN_VOLUME = 0,
	MAX_PULSE_VOLUME = 150,
};

typedef struct {
	char *name;
	bool received;
} ServerQuery;

typedef struct {
	PulseDevice *device;
	bool found;
} SinkQuery;

static void context_state_callback(pa_context *context, void *userdata)
{
	pa_context_state_t *state = userdata;
	*state = pa_context_get_state(context);
}

static void success_callback(pa_context *context, int success, void *userdata)
{
	int *result = userdata;
	*result = success;

	if (!success) {
		fprintf(stderr, "PulseAudio operation failed: %s\n",
			pa_strerror(pa_context_errno(context)));
	}
}

static char *copy_string(const char *source)
{
	size_t length;
	char *copy;

	if (source == NULL) {
		return NULL;
	}

	length = strlen(source) + 1;
	copy = malloc(length);
	if (copy != NULL) {
		memcpy(copy, source, length);
	}
	return copy;
}

static void server_info_callback(pa_context *context, const pa_server_info *info,
	void *userdata)
{
	ServerQuery *query = userdata;
	(void)context;

	if (info == NULL || info->default_sink_name == NULL) {
		return;
	}

	query->name = copy_string(info->default_sink_name);
	query->received = query->name != NULL;
}

static int volume_as_percent(const pa_cvolume *volume)
{
	/* Use a wide intermediate so the percentage conversion cannot overflow. */
	uint64_t scaled = (uint64_t)pa_cvolume_max(volume) * 100U;
	return (int)((scaled + PA_VOLUME_NORM / 2U) / PA_VOLUME_NORM);
}

static void sink_info_callback(pa_context *context, const pa_sink_info *info,
	int end_of_list, void *userdata)
{
	SinkQuery *query = userdata;

	if (end_of_list < 0) {
		fprintf(stderr, "Failed to query the default sink: %s\n",
			pa_strerror(pa_context_errno(context)));
		return;
	}
	if (end_of_list || info == NULL) {
		return;
	}

	query->device->index = info->index;
	query->device->volume = info->volume;
	query->device->volume_percent = volume_as_percent(&info->volume);
	query->device->muted = info->mute != 0;
	query->found = true;
}

static bool wait_for_operation(PulseClient *client, pa_operation *operation)
{
	bool completed;
	int iteration_result = 0;

	if (operation == NULL) {
		fprintf(stderr, "Failed to start PulseAudio operation: %s\n",
			pa_strerror(pa_context_errno(client->context)));
		return false;
	}

	while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
		if (pa_mainloop_iterate(client->mainloop, 1, &iteration_result) < 0) {
			break;
		}
	}
	completed = pa_operation_get_state(operation) == PA_OPERATION_DONE;
	pa_operation_unref(operation);
	return completed;
}

static long clamp_volume(long volume)
{
	if (volume < MIN_VOLUME) {
		return MIN_VOLUME;
	}
	if (volume > MAX_PULSE_VOLUME) {
		return MAX_PULSE_VOLUME;
	}
	return volume;
}

static pa_cvolume scaled_volume(const PulseDevice *device, long percentage)
{
	pa_cvolume volume = device->volume;
	pa_volume_t pulse_volume;

	percentage = clamp_volume(percentage);
	pulse_volume = (pa_volume_t)((double)percentage * PA_VOLUME_NORM / 100.0);
	pa_cvolume_scale(&volume, pulse_volume);
	return volume;
}

bool pulse_client_init(PulseClient *client, const char *client_name)
{
	pa_context_state_t state = PA_CONTEXT_UNCONNECTED;
	pa_proplist *properties;

	memset(client, 0, sizeof(*client));
	properties = pa_proplist_new();
	if (properties == NULL) {
		fprintf(stderr, "Failed to allocate PulseAudio properties\n");
		return false;
	}

	/* These properties identify PAUP in PulseAudio clients and diagnostics. */
	pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, client_name);
	pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, "com.falconindy.ponymix");
	pa_proplist_sets(properties, PA_PROP_APPLICATION_VERSION, PAUP_VERSION);
	pa_proplist_sets(properties, PA_PROP_APPLICATION_ICON_NAME, "audio-card");

	client->mainloop = pa_mainloop_new();
	if (client->mainloop != NULL) {
		client->context = pa_context_new_with_proplist(
			pa_mainloop_get_api(client->mainloop), NULL, properties);
	}
	pa_proplist_free(properties);

	if (client->mainloop == NULL || client->context == NULL) {
		fprintf(stderr, "Failed to create PulseAudio client\n");
		pulse_client_cleanup(client);
		return false;
	}

	pa_context_set_state_callback(client->context, context_state_callback, &state);
	if (pa_context_connect(client->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
		fprintf(stderr, "Failed to connect to PulseAudio: %s\n",
			pa_strerror(pa_context_errno(client->context)));
		pulse_client_cleanup(client);
		return false;
	}

	while (state != PA_CONTEXT_READY && state != PA_CONTEXT_FAILED
		&& state != PA_CONTEXT_TERMINATED) {
		if (pa_mainloop_iterate(client->mainloop, 1, NULL) < 0) {
			break;
		}
	}
	if (state != PA_CONTEXT_READY) {
		fprintf(stderr, "Failed to connect to PulseAudio: %s\n",
			pa_strerror(pa_context_errno(client->context)));
		pulse_client_cleanup(client);
		return false;
	}

	/* The connection state outlives this function, but its stack variable does not. */
	pa_context_set_state_callback(client->context, NULL, NULL);
	return true;
}

void pulse_client_cleanup(PulseClient *client)
{
	if (client->context != NULL) {
		pa_context_disconnect(client->context);
		pa_context_unref(client->context);
		client->context = NULL;
	}
	if (client->mainloop != NULL) {
		pa_mainloop_free(client->mainloop);
		client->mainloop = NULL;
	}
}

bool pulse_client_get_default_sink(PulseClient *client, PulseDevice *device)
{
	ServerQuery server = {0};
	SinkQuery sink = {.device = device};
	bool completed;

	memset(device, 0, sizeof(*device));
	completed = wait_for_operation(client,
		pa_context_get_server_info(client->context, server_info_callback, &server));
	if (!completed || !server.received) {
		free(server.name);
		return false;
	}

	completed = wait_for_operation(client,
		pa_context_get_sink_info_by_name(
			client->context, server.name, sink_info_callback, &sink));
	free(server.name);
	return completed && sink.found;
}

bool pulse_client_set_mute(PulseClient *client, PulseDevice *device, bool muted)
{
	int success = 0;
	bool completed;

	completed = wait_for_operation(client,
		pa_context_set_sink_mute_by_index(client->context, device->index,
			muted, success_callback, &success));
	if (completed && success) {
		device->muted = muted;
	}
	return completed && success;
}

bool pulse_client_set_volume(PulseClient *client, PulseDevice *device, long percentage)
{
	pa_cvolume volume = scaled_volume(device, percentage);
	int success = 0;
	bool completed;

	completed = wait_for_operation(client,
		pa_context_set_sink_volume_by_index(client->context, device->index,
			&volume, success_callback, &success));
	if (completed && success) {
		device->volume = volume;
		device->volume_percent = volume_as_percent(&volume);
	}
	return completed && success;
}

bool pulse_client_set_volume_async(PulseClient *client, PulseDevice *device,
	long percentage)
{
	pa_cvolume volume = scaled_volume(device, percentage);
	pa_operation *operation;

	/* libpulse copies the volume before returning, so this local is safe. */
	operation = pa_context_set_sink_volume_by_index(client->context,
		device->index, &volume, NULL, NULL);
	if (operation == NULL) {
		return false;
	}

	pa_operation_unref(operation);
	device->volume = volume;
	device->volume_percent = volume_as_percent(&volume);
	return true;
}

void pulse_client_iterate(PulseClient *client, bool block)
{
	int iteration_result;
	pa_mainloop_iterate(client->mainloop, block ? 1 : 0, &iteration_result);
}
