#include "pulse.h"

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	MIN_VOLUME = 0,
	MAX_PULSE_VOLUME = 150,
};

static void success_callback(pa_context *context, int success, void *userdata)
{
	int *result = userdata;
	*result = success;

	if (!success) {
		fprintf(stderr, "PulseAudio operation failed: %s\n",
			pa_strerror(pa_context_errno(context)));
	}
}

static int poll_with_extra_descriptor(struct pollfd *pulse_fds,
	unsigned long pulse_fd_count, int timeout, void *userdata)
{
	PulseClient *client = userdata;
	size_t total_count = (size_t)pulse_fd_count;
	struct pollfd *larger_buffer;
	int result;

	if (client->extra_poll_fd >= 0) {
		total_count++;
	}
	if (total_count > client->poll_buffer_capacity) {
		larger_buffer = realloc(client->poll_buffer,
			total_count * sizeof(*client->poll_buffer));
		if (larger_buffer == NULL) {
			return -1;
		}
		client->poll_buffer = larger_buffer;
		client->poll_buffer_capacity = total_count;
	}

	/* Preserve PulseAudio's descriptors and append X11 only for this wait. */
	if (pulse_fd_count > 0) {
		memcpy(client->poll_buffer, pulse_fds,
			(size_t)pulse_fd_count * sizeof(*pulse_fds));
	}
	if (client->extra_poll_fd >= 0) {
		client->poll_buffer[pulse_fd_count] = (struct pollfd){
			.fd = client->extra_poll_fd,
			.events = POLLIN,
		};
	}

	do {
		result = poll(client->poll_buffer, total_count, timeout);
	} while (result < 0 && errno == EINTR);
	if (result >= 0 && pulse_fd_count > 0) {
		memcpy(pulse_fds, client->poll_buffer,
			(size_t)pulse_fd_count * sizeof(*pulse_fds));
	}
	return result;
}

static void sink_info_callback(pa_context *context, const pa_sink_info *info,
	int end_of_list, void *userdata);

static void server_info_callback(pa_context *context, const pa_server_info *info,
	void *userdata)
{
	PulseClient *client = userdata;
	pa_operation *operation;

	if (info == NULL || info->default_sink_name == NULL) {
		client->state = PULSE_CLIENT_FAILED;
		return;
	}

	/* Start the dependent sink lookup directly from the server callback. */
	operation = pa_context_get_sink_info_by_name(context,
		info->default_sink_name, sink_info_callback, client);
	if (operation == NULL) {
		client->state = PULSE_CLIENT_FAILED;
		return;
	}
	pa_operation_unref(operation);
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
	PulseClient *client = userdata;

	if (end_of_list < 0) {
		fprintf(stderr, "Failed to query the default sink: %s\n",
			pa_strerror(pa_context_errno(context)));
		client->state = PULSE_CLIENT_FAILED;
		return;
	}
	if (end_of_list) {
		client->state = client->startup_sink_found
			? PULSE_CLIENT_READY : PULSE_CLIENT_FAILED;
		return;
	}
	if (info == NULL) {
		return;
	}

	client->startup_device->index = info->index;
	client->startup_device->volume = info->volume;
	client->startup_device->volume_percent = volume_as_percent(&info->volume);
	client->startup_device->muted = info->mute != 0;
	client->startup_sink_found = true;
}

static void context_state_callback(pa_context *context, void *userdata)
{
	PulseClient *client = userdata;
	pa_operation *operation;

	switch (pa_context_get_state(context)) {
		case PA_CONTEXT_READY:
			/* Default-sink discovery continues asynchronously on this loop. */
			operation = pa_context_get_server_info(context,
				server_info_callback, client);
			if (operation == NULL) {
				client->state = PULSE_CLIENT_FAILED;
				return;
			}
			pa_operation_unref(operation);
			break;
		case PA_CONTEXT_FAILED:
			fprintf(stderr, "PulseAudio connection failed: %s\n",
				pa_strerror(pa_context_errno(context)));
			client->state = PULSE_CLIENT_FAILED;
			break;
		case PA_CONTEXT_TERMINATED:
			client->state = PULSE_CLIENT_FAILED;
			break;
		default:
			break;
	}
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

bool pulse_client_start(PulseClient *client, const char *client_name,
	PulseDevice *device)
{
	pa_proplist *properties;

	memset(client, 0, sizeof(*client));
	memset(device, 0, sizeof(*device));
	client->state = PULSE_CLIENT_STARTING;
	client->startup_device = device;
	client->extra_poll_fd = -1;
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

	/* The custom poller lets PulseAudio sleep on X11 without another thread. */
	pa_mainloop_set_poll_func(client->mainloop,
		poll_with_extra_descriptor, client);
	pa_context_set_state_callback(client->context,
		context_state_callback, client);
	if (pa_context_connect(client->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
		fprintf(stderr, "Failed to connect to PulseAudio: %s\n",
			pa_strerror(pa_context_errno(client->context)));
		pulse_client_cleanup(client);
		return false;
	}

	/* Connection and default-sink discovery progress in the application loop. */
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
	free(client->poll_buffer);
	client->poll_buffer = NULL;
	client->poll_buffer_capacity = 0;
}

PulseClientState pulse_client_state(const PulseClient *client)
{
	return client->state;
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

	client->extra_poll_fd = -1;
	pa_mainloop_iterate(client->mainloop, block ? 1 : 0, &iteration_result);
}

bool pulse_client_wait(PulseClient *client, int extra_fd,
	int timeout_milliseconds)
{
	int timeout_microseconds;
	bool succeeded;

	/* pa_mainloop_prepare() uses microseconds, unlike poll()'s milliseconds. */
	if (timeout_milliseconds < 0) {
		timeout_microseconds = -1;
	} else if (timeout_milliseconds > INT_MAX / 1000) {
		timeout_microseconds = INT_MAX;
	} else {
		timeout_microseconds = timeout_milliseconds * 1000;
	}

	client->extra_poll_fd = extra_fd;
	succeeded = pa_mainloop_prepare(client->mainloop,
		timeout_microseconds) >= 0
		&& pa_mainloop_poll(client->mainloop) >= 0
		&& pa_mainloop_dispatch(client->mainloop) >= 0;
	client->extra_poll_fd = -1;
	return succeeded;
}
