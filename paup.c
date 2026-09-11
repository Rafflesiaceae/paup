#include "pulse.h"

#include <xcb/xcb.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	MAX_VOLUME = 100,
	POPUP_WIDTH = 40,
	POPUP_HEIGHT = 130,
	POPUP_MARGIN = 20,
	EXIT_FEEDBACK_MILLISECONDS = 17,
	VOLUME_REPEAT_DELAY_MILLISECONDS = 200,
	VOLUME_REPEAT_INTERVAL_MILLISECONDS = 10,
	BUFFER_SIZE = 1024,
	KEYSYM_ESCAPE = 0xff1b,
};

typedef struct {
	xcb_connection_t *handle;
	xcb_screen_t *screen;
	xcb_get_keyboard_mapping_reply_t *keyboard_mapping;
	xcb_keycode_t minimum_keycode;
	xcb_keycode_t maximum_keycode;
} Connection;

typedef struct {
	int direction;
	xcb_keycode_t keycode;
	struct timespec next_step;
} VolumeKeyHold;

typedef enum {
	TERMINAL_ACTION_NONE,
	TERMINAL_ACTION_TOGGLE_MUTE,
	TERMINAL_ACTION_SILENCE,
	TERMINAL_ACTION_LOUD,
} TerminalAction;

typedef struct {
	Connection connection;
	PulseClient pulse;
	PulseDevice device;
	bool pulse_initialized;
	bool device_ready;
	int startup_volume_delta;
	bool startup_mute_toggle;
	TerminalAction startup_terminal_action;
	xcb_atom_t active_window_atom;
	xcb_atom_t instance_atom;
	xcb_window_t window;
	xcb_pixmap_t buffer;
	xcb_gcontext_t background;
	xcb_gcontext_t foreground;
	xcb_gcontext_t foreground_muted;
	uint16_t window_width;
	uint16_t window_height;
	int volume;
	bool muted;
	bool redraw_pending;
	bool volume_sync_pending;
	long pending_volume;
	VolumeKeyHold volume_key_hold;
	struct timespec startup_started_at;
} App;

typedef enum {
	INIT_ERROR = -1,
	INIT_EXISTING = 0,
	INIT_READY = 1,
} InitResult;

static bool debug_enabled;

static void debugf(const char *format, ...)
{
	va_list arguments;

	if (!debug_enabled) {
		return;
	}

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fflush(stderr);
}

static bool connection_init(Connection *connection)
{
	const xcb_setup_t *setup;
	xcb_screen_iterator_t screens;
	int screen_number = 0;

	memset(connection, 0, sizeof(*connection));
	connection->handle = xcb_connect(NULL, &screen_number);
	if (connection->handle == NULL
		|| xcb_connection_has_error(connection->handle)) {
		debugf("Failed to connect to X11\n");
		return false;
	}

	/* Select the screen returned by xcb_connect instead of assuming screen 0. */
	setup = xcb_get_setup(connection->handle);
	screens = xcb_setup_roots_iterator(setup);
	while (screen_number-- > 0) {
		xcb_screen_next(&screens);
	}
	connection->screen = screens.data;
	if (connection->screen == NULL) {
		debugf("Failed to initialize the X11 screen\n");
		return false;
	}
	connection->minimum_keycode = setup->min_keycode;
	connection->maximum_keycode = setup->max_keycode;

	return true;
}

static void connection_cleanup(Connection *connection)
{
	free(connection->keyboard_mapping);
	connection->keyboard_mapping = NULL;
	if (connection->handle != NULL) {
		xcb_disconnect(connection->handle);
		connection->handle = NULL;
	}
}

static bool request_succeeded(Connection *connection, xcb_void_cookie_t cookie,
	const char *description)
{
	xcb_generic_error_t *error = xcb_request_check(connection->handle, cookie);

	if (error == NULL) {
		return true;
	}

	debugf("%s failed with X11 error %u\n", description, error->error_code);
	free(error);
	return false;
}

static bool read_required_atoms(App *app)
{
	static const char active_window_name[] = "_NET_ACTIVE_WINDOW";
	static const char instance_name[] = "_PAUP_INSTANCE";
	Connection *connection = &app->connection;
	xcb_intern_atom_cookie_t active_cookie;
	xcb_intern_atom_cookie_t instance_cookie;
	xcb_intern_atom_reply_t *active_reply;
	xcb_intern_atom_reply_t *instance_reply;
	xcb_get_keyboard_mapping_cookie_t keyboard_cookie;
	xcb_get_keyboard_mapping_reply_t *keyboard_reply;
	uint8_t keycode_count;

	/* Submit atoms and the keymap together so they share one X11 round trip. */
	active_cookie = xcb_intern_atom(connection->handle, false,
		sizeof(active_window_name) - 1, active_window_name);
	instance_cookie = xcb_intern_atom(connection->handle, false,
		sizeof(instance_name) - 1, instance_name);
	keycode_count = (uint8_t)(connection->maximum_keycode
		- connection->minimum_keycode + 1);
	keyboard_cookie = xcb_get_keyboard_mapping(connection->handle,
		connection->minimum_keycode, keycode_count);
	active_reply = xcb_intern_atom_reply(connection->handle,
		active_cookie, NULL);
	instance_reply = xcb_intern_atom_reply(connection->handle,
		instance_cookie, NULL);
	keyboard_reply = xcb_get_keyboard_mapping_reply(connection->handle,
		keyboard_cookie, NULL);
	if (active_reply == NULL || instance_reply == NULL
		|| keyboard_reply == NULL) {
		debugf("Failed to read required X11 metadata\n");
		free(active_reply);
		free(instance_reply);
		free(keyboard_reply);
		return false;
	}

	app->active_window_atom = active_reply->atom;
	app->instance_atom = instance_reply->atom;
	connection->keyboard_mapping = keyboard_reply;
	free(active_reply);
	free(instance_reply);
	return app->active_window_atom != XCB_ATOM_NONE
		&& app->instance_atom != XCB_ATOM_NONE;
}

static xcb_keysym_t lookup_keysym(const Connection *connection,
	xcb_keycode_t keycode)
{
	const xcb_keysym_t *keysyms;
	size_t offset;

	if (keycode < connection->minimum_keycode
		|| keycode > connection->maximum_keycode) {
		return XCB_NO_SYMBOL;
	}

	keysyms = xcb_get_keyboard_mapping_keysyms(
		connection->keyboard_mapping);
	offset = (size_t)(keycode - connection->minimum_keycode)
		* connection->keyboard_mapping->keysyms_per_keycode;
	return keysyms[offset];
}

static xcb_keycode_t lookup_keycode(const Connection *connection,
	xcb_keysym_t requested_keysym)
{
	const xcb_keysym_t *keysyms = xcb_get_keyboard_mapping_keysyms(
		connection->keyboard_mapping);
	uint16_t keycode;
	uint8_t column;

	/* Search every keysym column, matching xcb-keysyms' lookup behavior. */
	for (keycode = connection->minimum_keycode;
		keycode <= connection->maximum_keycode; keycode++) {
		size_t offset = (size_t)(keycode - connection->minimum_keycode)
			* connection->keyboard_mapping->keysyms_per_keycode;

		for (column = 0;
			column < connection->keyboard_mapping->keysyms_per_keycode;
			column++) {
			if (keysyms[offset + column] == requested_keysym) {
				return (xcb_keycode_t)keycode;
			}
		}
	}
	return XCB_NO_SYMBOL;
}

static void grab_key(Connection *connection, xcb_keysym_t keysym)
{
	xcb_keycode_t keycode;

	keycode = lookup_keycode(connection, keysym);
	if (keycode == XCB_NO_SYMBOL) {
		debugf("No X11 keycode for keysym 0x%x\n", keysym);
		return;
	}

	/* Grab failures are reported asynchronously with the other setup errors. */
	xcb_grab_key(connection->handle, true,
		connection->screen->root, 0, keycode,
		XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	debugf("Requested grab of keysym 0x%x as keycode %u\n",
		keysym, keycode);
}

static xcb_alloc_color_cookie_t request_color(Connection *connection,
	uint16_t red, uint16_t green, uint16_t blue)
{
	/* X11 accepts 16-bit components, while PAUP's palette uses 8-bit RGB. */
	red = (uint16_t)(65535U * (red & 0xffU) / 255U);
	green = (uint16_t)(65535U * (green & 0xffU) / 255U);
	blue = (uint16_t)(65535U * (blue & 0xffU) / 255U);
	return xcb_alloc_color(connection->handle,
		connection->screen->default_colormap, red, green, blue);
}

static bool get_palette(App *app, uint32_t *foreground,
	uint32_t *muted, uint32_t *background)
{
	Connection *connection = &app->connection;
	xcb_alloc_color_cookie_t foreground_cookie;
	xcb_alloc_color_cookie_t muted_cookie;
	xcb_alloc_color_cookie_t background_cookie;
	xcb_alloc_color_reply_t *foreground_reply;
	xcb_alloc_color_reply_t *muted_reply;
	xcb_alloc_color_reply_t *background_reply;
	bool succeeded;

	/* Pipeline the palette lookups instead of waiting for each color in turn. */
	foreground_cookie = request_color(connection, 0xa6, 0xe2, 0x2e);
	muted_cookie = request_color(connection, 0xff, 0x45, 0x35);
	background_cookie = request_color(connection, 0x38, 0x38, 0x30);
	foreground_reply = xcb_alloc_color_reply(connection->handle,
		foreground_cookie, NULL);
	muted_reply = xcb_alloc_color_reply(connection->handle,
		muted_cookie, NULL);
	background_reply = xcb_alloc_color_reply(connection->handle,
		background_cookie, NULL);
	succeeded = foreground_reply != NULL && muted_reply != NULL
		&& background_reply != NULL;
	if (succeeded) {
		*foreground = foreground_reply->pixel;
		*muted = muted_reply->pixel;
		*background = background_reply->pixel;
	} else {
		debugf("Failed to allocate the X11 palette\n");
	}

	free(foreground_reply);
	free(muted_reply);
	free(background_reply);
	return succeeded;
}

static void create_graphics_context(App *app, xcb_gcontext_t *context,
	uint32_t pixel)
{
	uint32_t values[] = {pixel, false};
	uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;

	*context = xcb_generate_id(app->connection.handle);
	/* Setup errors arrive through the normal event queue without a sync. */
	xcb_create_gc(app->connection.handle, *context,
		app->connection.screen->root, mask, values);
}

static void focus_popup(Connection *connection, xcb_window_t window)
{
	/* Override-redirect windows must be raised and focused without a WM. */
	uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
	xcb_configure_window(connection->handle, window,
		XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
	xcb_set_input_focus(connection->handle, XCB_INPUT_FOCUS_POINTER_ROOT,
		window, XCB_CURRENT_TIME);
}

static InitResult create_popup_or_focus_existing(App *app, uint32_t event_mask)
{
	Connection *connection = &app->connection;
	xcb_get_selection_owner_cookie_t owner_cookie;
	xcb_get_selection_owner_reply_t *owner_reply;
	xcb_window_t existing_window;
	uint32_t create_mask;
	uint32_t create_values[2];
	xcb_void_cookie_t create_cookie;

	/* The server grab makes selection ownership atomic across launches. */
	xcb_grab_server(connection->handle);
	owner_cookie = xcb_get_selection_owner(connection->handle,
		app->instance_atom);
	owner_reply = xcb_get_selection_owner_reply(connection->handle,
		owner_cookie, NULL);
	if (owner_reply == NULL) {
		xcb_ungrab_server(connection->handle);
		xcb_flush(connection->handle);
		debugf("Failed to query the existing PAUP instance\n");
		return INIT_ERROR;
	}

	existing_window = owner_reply->owner;
	free(owner_reply);
	if (existing_window != XCB_NONE) {
		xcb_ungrab_server(connection->handle);
		focus_popup(connection, existing_window);
		xcb_flush(connection->handle);
		debugf("Focused existing popup window %u\n", existing_window);
		return INIT_EXISTING;
	}

	create_mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
	create_values[0] = true;
	create_values[1] = event_mask;
	app->window = xcb_generate_id(connection->handle);
	create_cookie = xcb_create_window_checked(connection->handle,
		XCB_COPY_FROM_PARENT, app->window, connection->screen->root,
		POPUP_MARGIN, POPUP_MARGIN, POPUP_WIDTH, POPUP_HEIGHT, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, connection->screen->root_visual,
		create_mask, create_values);
	if (!request_succeeded(connection, create_cookie, "Popup creation")) {
		xcb_ungrab_server(connection->handle);
		xcb_flush(connection->handle);
		return INIT_ERROR;
	}

	/* X11 drops this single-instance claim when the popup is destroyed. */
	xcb_set_selection_owner(connection->handle, app->window,
		app->instance_atom, XCB_CURRENT_TIME);
	xcb_ungrab_server(connection->handle);
	return INIT_READY;
}

static void draw(App *app)
{
	xcb_connection_t *connection = app->connection.handle;
	xcb_rectangle_t foreground_rectangle;
	xcb_rectangle_t background_rectangle;
	int displayed_volume = app->volume;
	uint16_t filled_height;

	/* Protect X11 rectangle arithmetic if PulseAudio reports amplification. */
	if (displayed_volume < 0) {
		displayed_volume = 0;
	} else if (displayed_volume > MAX_VOLUME) {
		displayed_volume = MAX_VOLUME;
	}
	filled_height = (uint16_t)((app->window_height * displayed_volume)
		/ MAX_VOLUME);
	foreground_rectangle = (xcb_rectangle_t){
		.x = 0,
		.y = (int16_t)(app->window_height - filled_height),
		.width = app->window_width,
		.height = filled_height,
	};
	background_rectangle = (xcb_rectangle_t){
		.x = 0,
		.y = 0,
		.width = app->window_width,
		.height = app->window_height,
	};

	xcb_poly_fill_rectangle(connection, app->buffer, app->background,
		1, &background_rectangle);
	xcb_poly_fill_rectangle(connection, app->buffer,
		app->muted ? app->foreground_muted : app->foreground,
		1, &foreground_rectangle);
	xcb_copy_area(connection, app->buffer, app->window, app->foreground,
		0, 0, 0, 0, app->window_width, app->window_height);
	xcb_flush(connection);
	app->redraw_pending = false;
	debugf("Redrew, volume=%d muted=%d size=%ux%u\n", app->volume,
		app->muted, app->window_width, app->window_height);
}

static void request_draw(App *app)
{
	app->redraw_pending = true;
}

static void toggle_mute(App *app)
{
	app->muted = !app->muted;
	if (app->device_ready) {
		pulse_client_set_mute(&app->pulse, &app->device, app->muted);
	} else {
		/* Replay the toggle after asynchronous sink discovery completes. */
		app->startup_mute_toggle = !app->startup_mute_toggle;
	}
	request_draw(app);
}

static void show_exit_feedback(App *app)
{
	struct timespec remaining = {
		.tv_sec = 0,
		.tv_nsec = EXIT_FEEDBACK_MILLISECONDS * 1000000L,
	};

	/* Leave the final state mapped for one nominal 60 Hz display frame. */
	draw(app);
	while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
		/* Resume the unslept portion when a signal interrupts feedback. */
	}
}

static struct timespec monotonic_now(void)
{
	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now;
}

static struct timespec add_milliseconds(struct timespec time, long milliseconds)
{
	time.tv_sec += milliseconds / 1000;
	time.tv_nsec += (milliseconds % 1000) * 1000000L;
	if (time.tv_nsec >= 1000000000L) {
		time.tv_sec++;
		time.tv_nsec -= 1000000000L;
	}
	return time;
}

static int compare_times(struct timespec left, struct timespec right)
{
	if (left.tv_sec != right.tv_sec) {
		return left.tv_sec < right.tv_sec ? -1 : 1;
	}
	if (left.tv_nsec != right.tv_nsec) {
		return left.tv_nsec < right.tv_nsec ? -1 : 1;
	}
	return 0;
}

static double elapsed_milliseconds(struct timespec start, struct timespec end)
{
	int64_t nanoseconds = (int64_t)(end.tv_sec - start.tv_sec) * 1000000000LL;
	nanoseconds += end.tv_nsec - start.tv_nsec;
	return (double)nanoseconds / 1000000.0;
}

static void request_volume_sync(App *app)
{
	app->pending_volume = app->volume;
	app->volume_sync_pending = true;
}

static bool adjust_volume(App *app, int delta)
{
	if (!app->device_ready) {
		int pending_delta = app->startup_volume_delta + delta;

		/* Any larger delta is equivalent once the real 0-100 value arrives. */
		if (pending_delta < -MAX_VOLUME) {
			pending_delta = -MAX_VOLUME;
		} else if (pending_delta > MAX_VOLUME) {
			pending_delta = MAX_VOLUME;
		}
		if (pending_delta == app->startup_volume_delta) {
			return false;
		}
		app->startup_volume_delta = pending_delta;
		return true;
	}

	int requested_volume = app->volume + delta;
	int new_volume = requested_volume;

	if (new_volume < 0) {
		new_volume = 0;
	} else if (new_volume > MAX_VOLUME) {
		new_volume = MAX_VOLUME;
	}
	if (new_volume == app->volume) {
		return false;
	}

	app->volume = new_volume;
	request_volume_sync(app);
	request_draw(app);
	return true;
}

static void start_volume_hold(App *app, int direction, xcb_keycode_t keycode)
{
	struct timespec now;

	/* Ignore X11 auto-repeat presses; the monotonic timer owns repetition. */
	if (app->volume_key_hold.direction != 0
		&& app->volume_key_hold.keycode == keycode) {
		return;
	}

	now = monotonic_now();
	app->volume_key_hold.direction = direction;
	app->volume_key_hold.keycode = keycode;
	app->volume_key_hold.next_step = add_milliseconds(now,
		VOLUME_REPEAT_DELAY_MILLISECONDS);
	adjust_volume(app, direction);
}

static bool key_is_down(Connection *connection, xcb_keycode_t keycode)
{
	xcb_query_keymap_cookie_t cookie;
	xcb_query_keymap_reply_t *reply;
	bool is_down;

	/* The server keymap distinguishes releases from legacy auto-repeat. */
	cookie = xcb_query_keymap(connection->handle);
	reply = xcb_query_keymap_reply(connection->handle, cookie, NULL);
	if (reply == NULL) {
		return false;
	}

	is_down = (reply->keys[keycode / 8U]
		& (uint8_t)(1U << (keycode % 8U))) != 0;
	free(reply);
	return is_down;
}

static void stop_volume_hold_if_released(App *app, xcb_keycode_t keycode)
{
	if (app->volume_key_hold.direction == 0
		|| app->volume_key_hold.keycode != keycode) {
		return;
	}
	if (!key_is_down(&app->connection, keycode)) {
		app->volume_key_hold = (VolumeKeyHold){0};
	}
}

static void advance_volume_hold(App *app)
{
	struct timespec now = monotonic_now();

	while (app->volume_key_hold.direction != 0
		&& compare_times(now, app->volume_key_hold.next_step) >= 0) {
		if (!adjust_volume(app, app->volume_key_hold.direction)) {
			app->volume_key_hold = (VolumeKeyHold){0};
			return;
		}
		app->volume_key_hold.next_step = add_milliseconds(
			app->volume_key_hold.next_step,
			VOLUME_REPEAT_INTERVAL_MILLISECONDS);
	}
}

static int volume_hold_timeout_milliseconds(const App *app)
{
	struct timespec now;
	int64_t nanoseconds;
	int64_t milliseconds;

	if (app->volume_key_hold.direction == 0) {
		return -1;
	}

	now = monotonic_now();
	nanoseconds = (int64_t)(app->volume_key_hold.next_step.tv_sec
		- now.tv_sec) * 1000000000LL;
	nanoseconds += app->volume_key_hold.next_step.tv_nsec - now.tv_nsec;
	if (nanoseconds <= 0) {
		return 0;
	}

	/* poll() takes whole milliseconds, so round up to avoid early wakeups. */
	milliseconds = (nanoseconds + 999999LL) / 1000000LL;
	return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static bool finish_pulse_startup(App *app)
{
	PulseClientState state;
	int requested_volume;

	if (app->device_ready) {
		return true;
	}

	state = pulse_client_state(&app->pulse);
	if (state == PULSE_CLIENT_STARTING) {
		return true;
	}
	if (state == PULSE_CLIENT_FAILED) {
		debugf("PulseAudio startup failed\n");
		return false;
	}

	app->device_ready = true;
	app->volume = app->device.volume_percent;
	app->muted = app->device.muted;
	if (app->startup_terminal_action == TERMINAL_ACTION_TOGGLE_MUTE) {
		/* Preserve the net mute state requested during sink discovery. */
		if (app->startup_mute_toggle) {
			app->muted = !app->muted;
			pulse_client_set_mute(&app->pulse, &app->device, app->muted);
		}
		show_exit_feedback(app);
		return false;
	}
	if (app->startup_terminal_action == TERMINAL_ACTION_SILENCE) {
		app->muted = true;
		pulse_client_set_mute(&app->pulse, &app->device, true);
		show_exit_feedback(app);
		return false;
	}
	if (app->startup_terminal_action == TERMINAL_ACTION_LOUD) {
		app->muted = false;
		app->volume = MAX_VOLUME;
		pulse_client_set_mute(&app->pulse, &app->device, false);
		pulse_client_set_volume(&app->pulse, &app->device, MAX_VOLUME);
		show_exit_feedback(app);
		return false;
	}

	/* Replay input received while the sink state was still being discovered. */
	requested_volume = app->volume + app->startup_volume_delta;
	if (requested_volume < 0) {
		requested_volume = 0;
	} else if (requested_volume > MAX_VOLUME) {
		requested_volume = MAX_VOLUME;
	}
	if (requested_volume != app->volume) {
		app->volume = requested_volume;
		request_volume_sync(app);
	}
	if (app->startup_mute_toggle) {
		app->muted = !app->muted;
		pulse_client_set_mute(&app->pulse, &app->device, app->muted);
	}
	request_draw(app);
	return true;
}

static bool do_best_effort_work(App *app)
{
	pulse_client_iterate(&app->pulse, false);
	if (!finish_pulse_startup(app)) {
		return false;
	}

	if (app->volume_sync_pending) {
		app->volume_sync_pending = false;
		pulse_client_set_volume_async(&app->pulse, &app->device,
			app->pending_volume);
	}

	if (app->redraw_pending) {
		draw(app);
	}
	return true;
}

static bool handle_event(App *app, xcb_generic_event_t *event)
{
	uint8_t response_type = event->response_type & (uint8_t)~0x80U;

	switch (response_type) {
		case 0: {
			xcb_generic_error_t *error = (xcb_generic_error_t *)event;
			debugf("X11 error: code=%u sequence=%u resource=%u minor=%u major=%u\n",
				error->error_code, error->sequence, error->resource_id,
				error->minor_code, error->major_code);
			break;
		}
		case XCB_EXPOSE: {
			xcb_expose_event_t *expose = (xcb_expose_event_t *)event;
			xcb_copy_area(app->connection.handle, app->buffer, app->window,
				app->foreground, expose->x, expose->y, expose->x, expose->y,
				expose->width, expose->height);
			xcb_flush(app->connection.handle);
			debugf("XCB_EXPOSE\n");
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *configure =
				(xcb_configure_notify_event_t *)event;
			app->window_width = configure->width;
			app->window_height = configure->height;
			request_draw(app);
			debugf("XCB_CONFIGURE_NOTIFY size=%ux%u\n",
				app->window_width, app->window_height);
			break;
		}
		case XCB_FOCUS_IN:
		case XCB_FOCUS_OUT:
			break;
		case XCB_PROPERTY_NOTIFY: {
			xcb_property_notify_event_t *property =
				(xcb_property_notify_event_t *)event;
			if (property->atom == app->active_window_atom) {
				xcb_get_input_focus_cookie_t cookie =
					xcb_get_input_focus(app->connection.handle);
				xcb_get_input_focus_reply_t *reply =
					xcb_get_input_focus_reply(app->connection.handle,
						cookie, NULL);
				if (reply != NULL && reply->focus != app->window) {
					debugf("Focus moved away from the popup; exiting\n");
					free(reply);
					return false;
				}
				free(reply);
			}
			break;
		}
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *press = (xcb_key_press_event_t *)event;
			xcb_keysym_t keysym = lookup_keysym(&app->connection,
				press->detail);
			bool shift_pressed = (press->state & XCB_MOD_MASK_SHIFT) != 0;
			bool control_pressed =
				(press->state & XCB_MOD_MASK_CONTROL) != 0;

			debugf("KEY_PRESS: keysym=%u state=0x%x\n", keysym,
				press->state);
			switch (keysym) {
				case 'j':
					start_volume_hold(app, -1, press->detail);
					break;
				case 'k':
					start_volume_hold(app, 1, press->detail);
					break;
				case 'm':
					toggle_mute(app);
					if (!shift_pressed) {
						break;
					}
					/* Uppercase mute is terminal, matching silence and loud. */
					app->volume_key_hold = (VolumeKeyHold){0};
					if (!app->device_ready) {
						app->startup_terminal_action =
							TERMINAL_ACTION_TOGGLE_MUTE;
						break;
					}
					show_exit_feedback(app);
					return false;
				case 's':
				case 'h':
					/* Silence is terminal, so render its known final state. */
					app->volume_key_hold = (VolumeKeyHold){0};
					app->muted = true;
					if (!app->device_ready) {
						app->volume = MAX_VOLUME;
						app->startup_terminal_action =
							TERMINAL_ACTION_SILENCE;
						request_draw(app);
						break;
					}
					pulse_client_set_mute(&app->pulse, &app->device, true);
					show_exit_feedback(app);
					return false;
				case 'l':
					/* Loud is terminal and means unmuted at full volume. */
					app->volume_key_hold = (VolumeKeyHold){0};
					app->muted = false;
					app->volume = MAX_VOLUME;
					if (!app->device_ready) {
						app->startup_terminal_action =
							TERMINAL_ACTION_LOUD;
						request_draw(app);
						break;
					}
					pulse_client_set_mute(&app->pulse, &app->device, false);
					pulse_client_set_volume(&app->pulse, &app->device,
						MAX_VOLUME);
					show_exit_feedback(app);
					return false;
				case 'q':
				case KEYSYM_ESCAPE:
					return false;
				case 'c':
				case 'd':
					if (control_pressed) {
						return false;
					}
					break;
				default:
					break;
			}
			break;
		}
		case XCB_KEY_RELEASE: {
			xcb_key_release_event_t *release =
				(xcb_key_release_event_t *)event;
			stop_volume_hold_if_released(app, release->detail);
			break;
		}
		case XCB_BUTTON_PRESS:
			/* Preserve the popup's existing click-to-preview behavior. */
			app->volume++;
			request_draw(app);
			break;
		case XCB_MAP_NOTIFY:
			debugf("Popup mapped and ready for input in %.3f ms\n",
				elapsed_milliseconds(app->startup_started_at,
					monotonic_now()));
			break;
		default:
			debugf("Unhandled X11 event %u\n", response_type);
			break;
	}

	return true;
}

static bool run_event_loop(App *app)
{
	bool running = true;

	while (running && !xcb_connection_has_error(app->connection.handle)) {
		xcb_generic_event_t *event =
			xcb_poll_for_event(app->connection.handle);
		if (event == NULL) {
			/* Defer all audio work until the popup's initial X11 events drain. */
			if (!app->pulse_initialized) {
				if (!pulse_client_start(&app->pulse, "paup", &app->device)) {
					return false;
				}
				app->pulse_initialized = true;
			}
			advance_volume_hold(app);
			if (!do_best_effort_work(app)) {
				return pulse_client_state(&app->pulse)
					!= PULSE_CLIENT_FAILED;
			}
			if (!pulse_client_wait(&app->pulse,
				xcb_get_file_descriptor(app->connection.handle),
				volume_hold_timeout_milliseconds(app))) {
				debugf("Failed while waiting for application events\n");
				return false;
			}
			continue;
		}

		running = handle_event(app, event);
		free(event);
		if (running && app->pulse_initialized) {
			advance_volume_hold(app);
			if (!do_best_effort_work(app)) {
				return pulse_client_state(&app->pulse)
					!= PULSE_CLIENT_FAILED;
			}
		}
	}

	debugf("Exiting main loop\n");
	return xcb_connection_has_error(app->connection.handle) == 0;
}

static InitResult app_init(App *app)
{
	uint32_t event_mask;
	uint32_t root_event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	uint32_t foreground_pixel;
	uint32_t muted_pixel;
	uint32_t background_pixel;
	InitResult popup_result;

	memset(app, 0, sizeof(*app));
	app->startup_started_at = monotonic_now();
	app->window_width = POPUP_WIDTH;
	app->window_height = POPUP_HEIGHT;
	if (!connection_init(&app->connection)) {
		return INIT_ERROR;
	}

	if (!read_required_atoms(app)) {
		return INIT_ERROR;
	}

	event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS
		| XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE
		| XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_LEAVE_WINDOW
		| XCB_EVENT_MASK_ENTER_WINDOW;
	popup_result = create_popup_or_focus_existing(app, event_mask);
	if (popup_result != INIT_READY) {
		return popup_result;
	}

	if (!get_palette(app, &foreground_pixel, &muted_pixel,
		&background_pixel)) {
		return INIT_ERROR;
	}
	create_graphics_context(app, &app->foreground, foreground_pixel);
	create_graphics_context(app, &app->foreground_muted, muted_pixel);
	create_graphics_context(app, &app->background, background_pixel);

	app->buffer = xcb_generate_id(app->connection.handle);
	/* These independent requests are intentionally batched before one flush. */
	xcb_create_pixmap(app->connection.handle,
		app->connection.screen->root_depth, app->buffer, app->window,
		BUFFER_SIZE, BUFFER_SIZE);
	xcb_change_window_attributes(app->connection.handle,
		app->connection.screen->root, XCB_CW_EVENT_MASK,
		&root_event_mask);
	xcb_map_window(app->connection.handle, app->window);
	focus_popup(&app->connection, app->window);

	/* Grabs let one focused popup consume its complete command vocabulary. */
	grab_key(&app->connection, 'j');
	grab_key(&app->connection, 'k');
	grab_key(&app->connection, 'q');
	grab_key(&app->connection, 'm');
	grab_key(&app->connection, 's');
	grab_key(&app->connection, 'l');
	grab_key(&app->connection, KEYSYM_ESCAPE);
	xcb_flush(app->connection.handle);
	draw(app);
	return INIT_READY;
}

static void app_cleanup(App *app)
{
	if (app->pulse_initialized) {
		pulse_client_cleanup(&app->pulse);
		app->pulse_initialized = false;
	}

	/* Disconnecting would release these resources too, but explicit cleanup
	 * keeps repeated initialization safe and documents their ownership. */
	if (app->connection.handle != NULL) {
		if (app->buffer != XCB_NONE) {
			xcb_free_pixmap(app->connection.handle, app->buffer);
		}
		if (app->background != XCB_NONE) {
			xcb_free_gc(app->connection.handle, app->background);
		}
		if (app->foreground_muted != XCB_NONE) {
			xcb_free_gc(app->connection.handle, app->foreground_muted);
		}
		if (app->foreground != XCB_NONE) {
			xcb_free_gc(app->connection.handle, app->foreground);
		}
		if (app->window != XCB_NONE) {
			xcb_destroy_window(app->connection.handle, app->window);
		}
		xcb_flush(app->connection.handle);
	}
	connection_cleanup(&app->connection);
}

static void print_help(const char *program_name)
{
	printf(
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Show an interactive popup for the default audio sink.\n"
		"\n"
		"Options:\n"
		"  -d, --debug  Print diagnostic messages\n"
		"  -h, --help   Show this help and exit\n"
		"\n"
		"Keys:\n"
		"  j / k        Decrease / increase volume\n"
		"  m            Toggle mute and remain open\n"
		"  M            Toggle mute, then exit\n"
		"  s / l        Silence / set full volume, then exit\n"
		"  q / Escape   Exit\n",
		program_name);
}

int main(int argc, char **argv)
{
	App app;
	InitResult result;
	bool loop_succeeded;
	int argument;

	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "-h") == 0
			|| strcmp(argv[argument], "--help") == 0) {
			/* Help must work without an X11 or PulseAudio connection. */
			print_help(argv[0]);
			return EXIT_SUCCESS;
		}
		if (strcmp(argv[argument], "-d") == 0
			|| strcmp(argv[argument], "--debug") == 0) {
			debug_enabled = true;
		}
	}

	result = app_init(&app);
	if (result == INIT_EXISTING) {
		app_cleanup(&app);
		return EXIT_SUCCESS;
	}
	if (result == INIT_ERROR) {
		app_cleanup(&app);
		return EXIT_FAILURE;
	}

	loop_succeeded = run_event_loop(&app);
	app_cleanup(&app);
	return loop_succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
