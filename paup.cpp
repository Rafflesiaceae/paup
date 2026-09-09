#define XK_LATIN1
#define XK_MISCELLANY

// [RUN] make && ./paup

#include "pulse.h"

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>
#include <xcb/xcb_util.h>
#include <X11/keysymdef.h>

#include <poll.h>

#include <chrono>
#include <initializer_list>
#include <iostream>
#include <map>
#include <thread>
#include <typeinfo>
#include <vector>
#include <cassert>
#include <cstdarg>
#include <cstring>

#define XCB_MOD_MASK_SHIFT   1
#define XCB_MOD_MASK_LOCK    2
#define XCB_MOD_MASK_CONTROL 4
#define XCB_MOD_MASK_1       8   // Alt
#define XCB_MOD_MASK_2       16  // Num Lock (or whatever)
#define XCB_MOD_MASK_3       32
#define XCB_MOD_MASK_4       64  // Super/Win
#define XCB_MOD_MASK_5       128

using namespace std;

static bool g_debug = false;

// Unified debug/info print
void debugf(const char *fmt, ...)
{
	if (!g_debug) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
void debugf(std::string msg)
{
	if (!g_debug) return;
	fputs(msg.c_str(), stderr);
	fflush(stderr);
}

namespace xcl
{

class Window;
class Connection;

class Window
{
public:
	uint16_t width, height;

	Window() = delete;
	Window(Connection &con);

	// Accessors
	xcb_window_t handle() const { return handle_; }

protected:
	xcb_window_t handle_;
	Connection &con_;
};

class Connection
{
public:
	vector<Window> windows;

	Connection(map<string, xcb_atom_t> atoms);
	Connection(initializer_list<string> atoms);

	xcb_atom_t readAtom(string atom);
	void grabKey(uint32_t cmodifier, uint32_t ckey);

	xcb_connection_t *handle() const { return handle_; }
	xcb_key_symbols_t *symbols() const { return symbols_; }
	xcb_screen_t *screen() const { return screen_; }

protected:
	xcb_connection_t *handle_;
	map<string, xcb_atom_t> atoms_;
	xcb_key_symbols_t *symbols_;
	xcb_screen_t *screen_;
};

xcb_atom_t Connection::readAtom(std::string atomId)
{
	auto const internAtom = xcb_intern_atom(this->handle(), 0, atomId.size(), atomId.c_str());
	auto const reply = xcb_intern_atom_reply(this->handle(), internAtom, NULL);
	if (!reply) {
		debugf("Failed to get atom '%s'\n", atomId.c_str());
		throw std::runtime_error("Failed to read atom: " + atomId);
	}
	auto result = reply->atom;
	debugf("Read atom '%s' -> %lu\n", atomId.c_str(), (unsigned long)result);
	free(reply);
	return result;
}

void Connection::grabKey(uint32_t cmodifier, uint32_t ckey)
{
	auto key = xcb_key_symbols_get_keycode(this->symbols(), ckey);
	if (!key) {
		debugf("xcb_key_symbols_get_keycode returned NULL for keycode: %u\n", ckey);
		return;
	}
	auto const reply = xcb_grab_key(this->handle(), 1, this->screen()->root, cmodifier, *key, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

	xcb_generic_error_t *err = xcb_request_check(this->handle(), reply);
	if (err) {
		debugf("Key grab failed: key=0x%x, cmodifier=0x%x, error_code=%d\n", ckey, cmodifier, err->error_code);
		free(err);
	} else {
		debugf("Key grab success: key=0x%x, cmodifier=0x%x\n", ckey, cmodifier);
	}
}

Connection::Connection(initializer_list<string> initialAtomsId)
{
	this->handle_ = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(this->handle_)) {
		debugf("xcb_connect failed\n");
		throw std::runtime_error("xcb_connect failed");
	}
	this->screen_ = xcb_setup_roots_iterator(xcb_get_setup(this->handle_)).data;
	this->symbols_ = xcb_key_symbols_alloc(this->handle_);

	this->atoms_ = map<string, xcb_atom_t>();
	for (auto atomId : initialAtomsId) {
		atoms_.insert({atomId, readAtom(atomId)});
	}
}

Window::Window(Connection &con)
	: con_(con)
{
	this->handle_ = xcb_generate_id(con.handle());
}

inline Window newXCBSubWindow(Connection &con, uint16_t width, uint16_t height, uint32_t eventmask, xcb_window_t parent)
{
	auto w = Window(con);
	w.width = width;
	w.height = height;
	const auto mask = XCB_CW_EVENT_MASK;
	const uint32_t values[1] = {eventmask};
	xcb_create_window(con.handle(), (uint8_t)XCB_COPY_FROM_PARENT, w.handle(), parent, (int16_t)20, (int16_t)20, width, height, (uint16_t)0, (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT, con.screen()->root_visual, (uint32_t)mask, values);
	return w;
}

uint32_t newGC(Connection &con, uint32_t mask, uint32_t values[2])
{
	auto result = xcb_generate_id(con.handle());
	xcb_create_gc(con.handle(), result, con.screen()->root, mask, &values[0]);
	debugf("Created GC: %u\n", result);
	return result;
}

}  // namespace xcl

using namespace xcl;

auto con = Connection({"WM_STATE", "WM_NAME", "_NET_ACTIVE_WINDOW"});
uint32_t background, foreground, foreground_muted, buffer;
xcb_window_t subwin;

int vol = 0;
bool muted = false;
const int MAX_VOL = 100;
constexpr uint16_t POPUP_WIDTH = 40;
constexpr uint16_t POPUP_HEIGHT = 130;
constexpr uint16_t POPUP_MARGIN = 20;
constexpr auto EXIT_FEEDBACK_DURATION = std::chrono::milliseconds(10);
constexpr auto VOLUME_REPEAT_DELAY = std::chrono::milliseconds(200);
Device *device;
ServerInfo defaults;
const char *opt_device;
uint32_t col01;
uint16_t win_width = POPUP_WIDTH;
uint16_t win_height = POPUP_HEIGHT;
bool redraw_pending = false;
bool volume_sync_pending = false;
long pending_volume = 0;

struct VolumeKeyHold
{
	int direction = 0;
	xcb_keycode_t keycode = 0;
	std::chrono::steady_clock::time_point started_at;
	std::chrono::steady_clock::time_point next_step;
};

VolumeKeyHold volume_key_hold;

PulseClient pulsecl("paup");

void draw()
{
	const auto conhandle = con.handle();
	uint16_t pme = static_cast<uint16_t>(((float)win_height / 100.0f) * (float)vol);

	xcb_rectangle_t fg_rects = {0, static_cast<int16_t>(win_height - pme), win_width, pme};
	xcb_rectangle_t bg_rects = {0, 0, win_width, win_height};

	xcb_poly_fill_rectangle(conhandle, buffer, background, 1, &bg_rects);
	if (muted) {
		xcb_poly_fill_rectangle(conhandle, buffer, foreground_muted, 1, &fg_rects);
	} else {
		xcb_poly_fill_rectangle(conhandle, buffer, foreground, 1, &fg_rects);
	}
	xcb_flush(conhandle);
	xcb_copy_area(conhandle, buffer, subwin, foreground, 0, 0, 0, 0, win_width, win_height);
	xcb_flush(conhandle);
	debugf("Redrew, vol=%d muted=%d size=%ux%u\n", vol, muted, win_width, win_height);
	redraw_pending = false;
}

void request_draw()
{
	redraw_pending = true;
}

void show_exit_feedback()
{
	// Keep the window alive for a nominal 60 Hz frame after submitting the
	// final state, otherwise disconnecting from X11 can hide it immediately.
	draw();
	std::this_thread::sleep_for(EXIT_FEEDBACK_DURATION);
}

void request_volume_sync()
{
	pending_volume = vol;
	volume_sync_pending = true;
}

bool adjust_volume(int delta)
{
	const int requested_volume = vol + delta;
	const int new_volume = requested_volume < 0 ? 0 : (requested_volume > MAX_VOL ? MAX_VOL : requested_volume);
	if (new_volume == vol) return false;

	vol = new_volume;
	request_volume_sync();
	request_draw();
	return true;
}

void start_volume_hold(int direction, xcb_keycode_t keycode)
{
	// Repeated KeyPress events from X11 must not affect the rate; only the
	// first physical press changes the volume and starts the monotonic timer.
	if (volume_key_hold.direction != 0 && volume_key_hold.keycode == keycode) return;

	const auto now = std::chrono::steady_clock::now();
	volume_key_hold = {direction, keycode, now, now + VOLUME_REPEAT_DELAY};
	adjust_volume(direction);
}

bool key_is_down(xcb_connection_t *conhandle, xcb_keycode_t keycode)
{
	// Querying the server distinguishes a physical release from the synthetic
	// release/press pairs produced by legacy X11 keyboard auto-repeat.
	const auto cookie = xcb_query_keymap(conhandle);
	auto *reply = xcb_query_keymap_reply(conhandle, cookie, NULL);
	if (!reply) return false;

	const bool is_down = reply->keys[keycode / 8] & (1U << (keycode % 8));
	free(reply);
	return is_down;
}

void stop_volume_hold_if_released(xcb_connection_t *conhandle, xcb_keycode_t keycode)
{
	if (volume_key_hold.direction == 0 || volume_key_hold.keycode != keycode) return;
	if (!key_is_down(conhandle, keycode)) volume_key_hold = {};
}

void advance_volume_hold()
{
	const auto now = std::chrono::steady_clock::now();
	while (volume_key_hold.direction != 0 && now >= volume_key_hold.next_step) {
		const auto held_for = volume_key_hold.next_step - volume_key_hold.started_at;
		int step = 1;
		auto interval = std::chrono::milliseconds(10);

		// // Increase both the step and cadence in stages. Short taps remain precise,
		// // while a sustained hold reaches either end of the range very quickly.
		// if (held_for >= std::chrono::milliseconds(9000)) {
		// 	step = 5;
		// 	interval = std::chrono::milliseconds(1);
		// } else if (held_for >= std::chrono::milliseconds(500)) {
		// 	step = 2;
		// 	interval = std::chrono::milliseconds(5);
		// }

		if (!adjust_volume(volume_key_hold.direction * step)) {
			volume_key_hold = {};
			return;
		}
		volume_key_hold.next_step += interval;
	}
}

int volume_hold_timeout_ms()
{
	if (volume_key_hold.direction == 0) return -1;

	const auto remaining = volume_key_hold.next_step - std::chrono::steady_clock::now();
	const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
	if (remaining_us <= 0) return 0;
	return static_cast<int>((remaining_us + 999) / 1000);
}

void do_best_effort_work()
{
	if (volume_sync_pending) {
		volume_sync_pending = false;
		pulsecl.SetVolumeAsync(*device, pending_volume);
	}

	pulsecl.Iterate(0);

	if (redraw_pending) {
		draw();
	}
}

uint32_t get_colorpixel(uint16_t r, uint16_t g, uint16_t b)
{
#define RGB_8_TO_16(i) (65535 * ((i) & 0xFF) / 255)
	int r16 = RGB_8_TO_16(r);
	int g16 = RGB_8_TO_16(g);
	int b16 = RGB_8_TO_16(b);

	xcb_alloc_color_reply_t *reply;

	reply = xcb_alloc_color_reply(con.handle(), xcb_alloc_color(con.handle(), con.screen()->default_colormap, r16, g16, b16), NULL);

	if (!reply) {
		debugf("xcb_alloc_color_reply failed\n");
		throw std::runtime_error("Color allocation failed");
	}

	uint32_t pixel = reply->pixel;
	free(reply);

	return pixel;
}

void focus_popup(xcb_connection_t *conhandle, xcb_window_t popup)
{
	// Override-redirect windows are unmanaged, so explicitly raise and focus
	// the popup instead of asking the window manager to activate it.
	const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
	xcb_configure_window(conhandle, popup, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
	xcb_set_input_focus(conhandle, XCB_INPUT_FOCUS_POINTER_ROOT, popup, XCB_CURRENT_TIME);
}

bool create_popup_or_focus_existing(xcb_connection_t *conhandle, xcb_screen_t *screen, uint32_t windowmask)
{
	const xcb_atom_t instance_atom = con.readAtom("_PAUP_INSTANCE");

	// Holding the X server grab makes checking and claiming the selection
	// atomic, preventing simultaneous launches from mapping two popups.
	xcb_grab_server(conhandle);
	const auto owner_cookie = xcb_get_selection_owner(conhandle, instance_atom);
	auto *owner_reply = xcb_get_selection_owner_reply(conhandle, owner_cookie, NULL);
	if (!owner_reply) {
		xcb_ungrab_server(conhandle);
		xcb_flush(conhandle);
		throw std::runtime_error("Failed to check for an existing PAUP instance");
	}

	const xcb_window_t existing_popup = owner_reply->owner;
	free(owner_reply);
	if (existing_popup != XCB_NONE) {
		xcb_ungrab_server(conhandle);
		focus_popup(conhandle, existing_popup);
		xcb_flush(conhandle);
		debugf("Focused existing popup window %u\n", existing_popup);
		return false;
	}

	const int16_t popup_x = POPUP_MARGIN;
	const int16_t popup_y = POPUP_MARGIN;
	const uint32_t create_mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
	const uint32_t create_values[] = {1, windowmask};
	subwin = xcb_generate_id(conhandle);
	const auto create_cookie = xcb_create_window_checked(conhandle, XCB_COPY_FROM_PARENT, subwin, screen->root, popup_x, popup_y, POPUP_WIDTH, POPUP_HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, create_mask, create_values);
	auto *create_error = xcb_request_check(conhandle, create_cookie);
	if (create_error) {
		const uint8_t error_code = create_error->error_code;
		free(create_error);
		xcb_ungrab_server(conhandle);
		xcb_flush(conhandle);
		throw std::runtime_error("Failed to create PAUP popup (X11 error " + std::to_string(error_code) + ")");
	}

	// The selection belongs to the popup itself, so X11 releases the
	// single-instance claim automatically when the process disconnects.
	xcb_set_selection_owner(conhandle, subwin, instance_atom, XCB_CURRENT_TIME);
	xcb_ungrab_server(conhandle);
	return true;
}

void init(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
			g_debug = true;
			break;
		}
	}

	auto screen = con.screen();
	auto conhandle = con.handle();

	uint32_t windowmask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE;
	if (!create_popup_or_focus_existing(conhandle, screen, windowmask)) {
		return;
	}

	pulsecl.Populate();

	uint32_t values[2];
	values[1] = 0;

	values[0] = get_colorpixel(0xA6, 0xE2, 0x2E);
	foreground = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

	values[0] = get_colorpixel(0xFF, 0x45, 0x35);
	foreground_muted = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

	values[0] = get_colorpixel(0x38, 0x38, 0x30);
	background = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

	buffer = xcb_generate_id(conhandle);
	xcb_create_pixmap_checked(conhandle, screen->root_depth, buffer, subwin, 1024, 1024);

	xcb_map_window(conhandle, subwin);
	focus_popup(conhandle, subwin);

	const auto olo = (uint32_t)XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes_checked(conhandle, screen->root, XCB_CW_EVENT_MASK, &olo);

	con.grabKey(0, XK_j);
	con.grabKey(0, XK_k);
	con.grabKey(0, XK_q);
	con.grabKey(0, XK_m);
	con.grabKey(0, XK_s);
	con.grabKey(0, XK_l);
	con.grabKey(0, XK_Escape);

	xcb_flush(conhandle);

	defaults = pulsecl.GetDefaults();
	opt_device = defaults.GetDefault(DeviceType::SINK).c_str();
	device = pulsecl.GetDevice(opt_device, DeviceType::SINK);

	if (!device) {
		debugf("Failed to get default device\n");
		throw std::runtime_error("No pulseaudio device");
	}

	vol = device->Volume();
	muted = device->Muted();

	draw();

	xcb_generic_event_t *ev;
	std::string logEvent = "";
	while (!xcb_connection_has_error(conhandle)) {
		ev = xcb_poll_for_event(conhandle);
		if (!ev) {
			advance_volume_hold();
			do_best_effort_work();

			// Wake for either an X11 event or the next independently timed
			// volume step, rather than blocking on the keyboard repeat rate.
			pollfd x11_poll = {xcb_get_file_descriptor(conhandle), POLLIN, 0};
			poll(&x11_poll, 1, volume_hold_timeout_ms());
			continue;
		}

		{  // log event
			logEvent = "[";
			if (auto evName = xcb_event_get_label(ev->response_type); evName != NULL)
				logEvent += std::string(evName);
			else
				logEvent += "UNKNOWN-EVENT";
			logEvent += "]\n";
		}

		switch (ev->response_type & ~0x80) {
			case 0:  // Error
				{
					auto err = (xcb_generic_error_t *)ev;
					debugf("XCB ERROR: error_code=%u, sequence=%u, resource_id=%u, minor_code=%u, major_code=%u\n", err->error_code, err->sequence, err->resource_id, err->minor_code, err->major_code);

					switch (err->error_code) {
						case XCB_WINDOW: debugf("XCB error: BadWindow (invalid window parameter)\n"); break;
						case XCB_MATCH: debugf("XCB error: BadMatch (parameter mismatch)\n"); break;
						case XCB_DRAWABLE:
							debugf("XCB error: BadDrawable (invalid drawable parameter)\n");
							break;
						default: break;
					}
					free(ev);
					continue;
					break;
				}
			case XCB_EXPOSE:
				{
					auto e = (xcb_expose_event_t *)(ev);
					xcb_copy_area(conhandle, buffer, subwin, foreground, e->x, e->y, e->x, e->y, e->width, e->height);
					xcb_flush(conhandle);
					debugf("XCB_EXPOSE\n");
					break;
				}
			case XCB_CONFIGURE_NOTIFY:
				{
					auto e = (xcb_configure_notify_event_t *)(ev);
					win_width = e->width;
					win_height = e->height;
					request_draw();
					debugf("XCB_CONFIGURE_NOTIFY size=%ux%u\n", win_width, win_height);
					break;
				}
			case XCB_FOCUS_IN:
				break;
			case XCB_FOCUS_OUT:
				break;
			case XCB_PROPERTY_NOTIFY:
				{
					auto e = (xcb_property_notify_event_t *)(ev);
					if (e->atom == con.readAtom("_NET_ACTIVE_WINDOW")) {
						xcb_get_input_focus_cookie_t cookie = xcb_get_input_focus(con.handle());
						xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(con.handle(), cookie, NULL);
						if (reply && reply->focus != subwin) {
							debugf("Active Window was changed AWAY from our overlay. Exiting.\n");
							free(reply);
							goto exit;
						}
						free(reply);
						break;
					}
					break;
				}
			case XCB_KEY_PRESS:
				{
					logEvent = "";
					auto e = (xcb_key_press_event_t *)(ev);

					bool shift_pressed = e->state & XCB_MOD_MASK_SHIFT;
					bool ctrl_pressed = e->state & XCB_MOD_MASK_CONTROL;
					bool alt_pressed = e->state & XCB_MOD_MASK_1;
					bool super_pressed = e->state & XCB_MOD_MASK_4;

					const auto keysym = xcb_key_press_lookup_keysym(con.symbols(), e, 0);

					debugf("KEY_PRESS: keysym=%d [%d:%d:%d:%d]\n", keysym, shift_pressed, ctrl_pressed, alt_pressed, super_pressed);

					switch (keysym) {
						case XK_j:
							start_volume_hold(-1, e->detail);
							break;
						case XK_k:
							start_volume_hold(1, e->detail);
							break;
						case XK_m:
							muted = !muted;
							pulsecl.SetMute(*device, muted);
							request_draw();
							break;
						case XK_s:
							// Silence is an explicit terminal action, unlike the toggle.
							volume_key_hold = {};
							muted = true;
							pulsecl.SetMute(*device, true);
							show_exit_feedback();
							goto exit;
							break;
						case XK_l:
							// Loud always establishes a known unmuted, full-volume state.
							volume_key_hold = {};
							muted = false;
							vol = MAX_VOL;
							pulsecl.SetMute(*device, false);
							pulsecl.SetVolume(*device, MAX_VOL);
							show_exit_feedback();
							goto exit;
							break;
						case XK_q:
						case XK_Escape:  // Escape
							goto exit;
							break;
						case XK_c:
						case XK_d:
							if (ctrl_pressed) {
								goto exit;
							}
							break;
					}
					break;
				}
			case XCB_KEY_RELEASE:
				logEvent = "";
				stop_volume_hold_if_released(conhandle, ((xcb_key_release_event_t *)ev)->detail);
				break;
			case XCB_BUTTON_PRESS:
				vol += 1;
				request_draw();
				break;
			case XCB_MAP_NOTIFY:
				debugf("XCB_MAP_NOTIFY received (window mapped)\n");
				break;

			default:
				logEvent = "";
				debugf("unhandled(");
				debugf("%d:", ev->response_type & ~0x80);
				debugf("%d;", ev->response_type);
				if (auto resptypeStr = xcb_event_get_label(ev->response_type); resptypeStr != NULL)
					debugf(std::string(resptypeStr));
				else
					debugf("UNKNOWN-EVENT");
				debugf(")\n");
				debugf("Unhandled XCB event: type=0x%02x\n", ev->response_type & ~0x80);
				break;
		}

		if (logEvent != "") {
			debugf("Unhandled event: " + logEvent);
		}
		if (ev != NULL) {
			free(ev);
		}
		advance_volume_hold();
		do_best_effort_work();
	}

exit:
	debugf("Exiting main loop\n");
	return;
}

int main(int argc, char **argv)
{
	try {
		init(argc, argv);
		exit(0);
	} catch (std::exception const &ex) {
		debugf("[EXCEPTION]\n");
		debugf(std::string(ex.what()) + "\n");
		debugf("Exception: %s\n", ex.what());
	}
}

// vim: set et ts=2 sw=2:
