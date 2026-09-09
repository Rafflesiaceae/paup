#ifndef PAUP_PULSE_H
#define PAUP_PULSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <poll.h>
#include <pulse/pulseaudio.h>

/* The popup only needs the mutable state of the default PulseAudio sink. */
typedef struct {
	uint32_t index;
	pa_cvolume volume;
	int volume_percent;
	bool muted;
} PulseDevice;

typedef enum {
	PULSE_CLIENT_STARTING,
	PULSE_CLIENT_READY,
	PULSE_CLIENT_FAILED,
} PulseClientState;

/* PulseAudio and X11 share one poll cycle through the extra descriptor. */
typedef struct {
	pa_mainloop *mainloop;
	pa_context *context;
	PulseClientState state;
	PulseDevice *startup_device;
	bool startup_sink_found;
	int extra_poll_fd;
	struct pollfd *poll_buffer;
	size_t poll_buffer_capacity;
} PulseClient;

bool pulse_client_start(PulseClient *client, const char *client_name,
	PulseDevice *device);
void pulse_client_cleanup(PulseClient *client);

PulseClientState pulse_client_state(const PulseClient *client);
bool pulse_client_set_mute(PulseClient *client, PulseDevice *device, bool muted);
bool pulse_client_set_volume(PulseClient *client, PulseDevice *device, long volume);
bool pulse_client_set_volume_async(PulseClient *client, PulseDevice *device, long volume);
void pulse_client_iterate(PulseClient *client, bool block);
bool pulse_client_wait(PulseClient *client, int extra_fd,
	int timeout_milliseconds);

#endif
