#ifndef PAUP_PULSE_H
#define PAUP_PULSE_H

#include <stdbool.h>
#include <stdint.h>

#include <pulse/pulseaudio.h>

/* The popup only needs the mutable state of the default PulseAudio sink. */
typedef struct {
	uint32_t index;
	pa_cvolume volume;
	int volume_percent;
	bool muted;
} PulseDevice;

/* PulseAudio's asynchronous API is driven through this private main loop. */
typedef struct {
	pa_mainloop *mainloop;
	pa_context *context;
} PulseClient;

bool pulse_client_init(PulseClient *client, const char *client_name);
void pulse_client_cleanup(PulseClient *client);

bool pulse_client_get_default_sink(PulseClient *client, PulseDevice *device);
bool pulse_client_set_mute(PulseClient *client, PulseDevice *device, bool muted);
bool pulse_client_set_volume(PulseClient *client, PulseDevice *device, long volume);
bool pulse_client_set_volume_async(PulseClient *client, PulseDevice *device, long volume);
void pulse_client_iterate(PulseClient *client, bool block);

#endif
