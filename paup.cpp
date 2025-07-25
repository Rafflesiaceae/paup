#define XK_LATIN1
#define XK_MISCELLANY

// [RUN] make && ./paup

#include "pulse.h"

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>
#include <xcb/xcb_util.h>
#include <X11/keysymdef.h>

#include <initializer_list>
#include <iostream>
#include <map>
#include <typeinfo>
#include <vector>
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <thread>

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
static bool used_fallback = false; // new global

int vol = 0;
bool muted = false;
const int MAX_VOL = 100;
Device *device;
ServerInfo defaults;
const char *opt_device;
uint32_t col01;

PulseClient pulsecl("paup");

// Fetch current window geometry (width, height)
bool get_window_size(xcb_connection_t *conn, xcb_window_t win, uint16_t &w, uint16_t &h)
{
	xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, win);
	xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, geom_cookie, NULL);
	if (geom) {
		w = geom->width;
		h = geom->height;
		free(geom);
		return true;
	} else {
		w = 40;
		h = 130;
		return false;
	}
}

void draw()
{
	const auto conhandle = con.handle();
	uint16_t win_width = 40, win_height = 130;
	get_window_size(conhandle, subwin, win_width, win_height);

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

// --- New code for deferring the initial draw only if fallback is used ---
void wait_for_valid_window_size_and_draw()
{
	const auto conhandle = con.handle();
	if (used_fallback) {
		const int max_attempts = 40; // wait up to ~200ms total (40 x 5ms)
		int attempts = 0;
		uint16_t w = 0, h = 0;
		while (attempts < max_attempts) {
			bool ok = get_window_size(conhandle, subwin, w, h);
			if (ok && w > 1 && h > 1 && !(w == 40 && h == 130)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			xcb_flush(conhandle);
			attempts++;
		}
		debugf("Window size detected after %d attempts: %ux%u\n", attempts, w, h);
	}
	draw();
}
// --- End new code ---

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

	pulsecl.Populate();

	auto getwin = xcb_get_input_focus(conhandle);
	auto rep = xcb_get_input_focus_reply(conhandle, getwin, NULL);
	if (!rep) {
		debugf("xcb_get_input_focus_reply failed\n");
		throw std::runtime_error("Failed to get input focus");
	}
	auto parent = rep->focus;
	free(rep);

	uint32_t windowmask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE;

	xcb_window_t overlay_parent = parent;
	xcb_window_t window_id = xcb_generate_id(conhandle);

	xcb_generic_error_t *err = xcb_request_check(conhandle, xcb_create_window_checked(conhandle, (uint8_t)XCB_COPY_FROM_PARENT, window_id, overlay_parent, (int16_t)20, (int16_t)20, 40, 130, (uint16_t)0, (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_EVENT_MASK, &windowmask));
	used_fallback = false;
	if (err) {
		debugf("Window creation failed with parent (focus): error_code=%d (falling back to root)\n", err->error_code);
		free(err);

		overlay_parent = screen->root;
		window_id = xcb_generate_id(conhandle);
		xcb_create_window(conhandle, (uint8_t)XCB_COPY_FROM_PARENT, window_id, overlay_parent, (int16_t)20, (int16_t)20, 40, 130, (uint16_t)0, (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_EVENT_MASK, &windowmask);
		used_fallback = true;
	}
	subwin = window_id;

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

	xcb_set_input_focus(conhandle, XCB_INPUT_FOCUS_POINTER_ROOT, subwin, XCB_CURRENT_TIME);

	const auto olo = (uint32_t)XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes_checked(conhandle, screen->root, XCB_CW_EVENT_MASK, &olo);

	con.grabKey(0, XK_j);
	con.grabKey(0, XK_k);
	con.grabKey(0, XK_q);
	con.grabKey(0, XK_m);
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

	// Replace original draw() with wait_for_valid_window_size_and_draw()
	wait_for_valid_window_size_and_draw();

	xcb_generic_event_t *ev;
	std::string logEvent = "";
	while ((ev = xcb_wait_for_event(conhandle))) {

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
						case 106:  // j or J
							if (vol > 0) {
								vol -= 1;
								pulsecl.SetVolume(*device, vol);
								draw();
							}
							break;
						case 107:  // k or K
							if (vol < MAX_VOL) {
								vol += 1;
								pulsecl.SetVolume(*device, vol);
								draw();
							}
							break;
						case 109:  // m or M
							muted = !muted;
							pulsecl.SetMute(*device, muted);
							draw();
							break;
						case 113:        // q
						case XK_Escape:  // Escape
							goto exit;
							break;
						case 99:   // c or C
						case 100:  // d or D
							if (ctrl_pressed) {
								goto exit;
							}
							break;
					}
					break;
				}
			case XCB_KEY_RELEASE:
				logEvent = "";
				break;
			case XCB_BUTTON_PRESS:
				vol += 1;
				draw();
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
