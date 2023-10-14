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


using namespace std;

namespace xcl {

class Window;
class Connection;

class Window {
 public:
  uint16_t width, height;

  Window() =delete;
  Window(Connection& con);

  // Accessors
  xcb_window_t handle() const { return handle_;}

 protected:
  xcb_window_t handle_;
  Connection& con_;
};

class Connection {
 public:
  /* explicit operator xcb_connection_t*() const {return handle_;} */
  vector<Window> windows;

  Connection(map<string, xcb_atom_t> atoms);
  Connection(initializer_list<string> atoms);

  xcb_atom_t readAtom(string atom);
  void grabKey(uint32_t cmodifier, uint32_t ckey);

  // Accessors
  xcb_connection_t* handle() const { return handle_;}
  xcb_key_symbols_t* symbols() const { return symbols_;}
  xcb_screen_t* screen() const { return screen_;}

 protected:
  xcb_connection_t* handle_;
  map<string, xcb_atom_t> atoms_;
  xcb_key_symbols_t* symbols_;
  xcb_screen_t* screen_;
};

xcb_atom_t Connection::readAtom(std::string atomId) {
  /* const xcb_generic_event_t* ev; */
  auto const internAtom = xcb_intern_atom(this->handle(), 0, atomId.size(), atomId.c_str());
  auto const reply = xcb_intern_atom_reply(this->handle(), internAtom, NULL);
  // @TODO check reply
  // auto const reply = xcb_request_check(this->handle(), cookie)
  // if (reply == nullptr) {
  //    throw std::runtime_error("Failed to read atom: "+atomId);
  // }

  auto result = reply->atom;
  free(reply);
  return result;

  /* let internAtom = xcb_intern_atom(con, 0, atom.len.uint16, $atom) */
  /* let reply = xcb_intern_atom_reply(con, internAtom, nil) # no error testing */
  /* if reply == nil: */
  /*   raise newException(IOError, "Error getting Atom " & $atom) */

  /* result = reply.atom */
  /* cfree(reply) */
}

void Connection::grabKey(uint32_t cmodifier, uint32_t ckey) {
  auto key = xcb_key_symbols_get_keycode(this->symbols(), ckey);
  auto const reply = xcb_grab_key(this->handle(), 0, this->screen()->root, 0, *key, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  // @TODO error check reply ↑
}

Connection::Connection(initializer_list<string> initialAtomsId) {
  this->handle_ = xcb_connect(NULL, NULL);
  this->screen_ = xcb_setup_roots_iterator(xcb_get_setup(this->handle())).data;
  this->symbols_ = xcb_key_symbols_alloc(this->handle());

  // initialize atoms
  this->atoms_ = map<string, xcb_atom_t>();
  for (auto atomId : initialAtomsId) {
    atoms_.insert({atomId, readAtom(atomId)});
  }
}

/* xcb_atom_t Connection::readAtom(string atom) { */
/*   const auto internAtom = xcb_intern_atom(this->getHandle(), 0, atom.size(), atom.c_str()); */
/*   /1* const auto reply = xcb_intern_atom_reply(this->getHandle(), ) *1/ */

/*   /1* let reply = xcb_intern_atom_reply(con, internAtom, nil) # no error testing *1/ */
/*   /1* if reply == nil: *1/ */
/*   /1*   raise newException(IOError, "Error getting Atom " & $atom) *1/ */

/*   /1* result = reply.atom *1/ */
/*   /1* cfree(reply) *1/ */
/* } */


Window::Window(Connection& con) : con_(con) {
  this->handle_ = xcb_generate_id(con.handle());
}

inline Window newXCBWindow(Connection& con, uint16_t width, uint16_t height, uint32_t eventmask, xcb_window_t parent) {
  auto w = Window(con);
  w.width = width;
  w.height = height;
  const auto mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  const uint32_t values[2] = {1, eventmask};
  xcb_create_window(con.handle(),
                    (uint8_t)XCB_COPY_FROM_PARENT,
                    w.handle(),
                    parent,
                    (int16_t)20, (int16_t)20,
                    width, height,
                    (uint16_t)0,
                    (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    con.screen()->root_visual,
                    (uint32_t)mask, values);

  return w;
}

inline Window newXCBSubWindow(Connection& con, uint16_t width, uint16_t height, uint32_t eventmask, xcb_window_t parent) {
  auto w = Window(con);
  w.width = width;
  w.height = height;
  const auto mask = XCB_CW_OVERRIDE_REDIRECT |  XCB_CW_EVENT_MASK;
  const uint32_t values[2] = {1, eventmask};
  xcb_create_window(con.handle(),
                    (uint8_t)XCB_COPY_FROM_PARENT,
                    w.handle(),
                    parent,
                    (int16_t)20, (int16_t)20,
                    width, height,
                    (uint16_t)0,
                    (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    con.screen()->root_visual,
                    (uint32_t)mask, values);

  return w;
}

/* Connection::Connection() { */
/*   this->handle = xcb_connect(NULL, NULL); */
/*   const auto conhandle = this->getHandle(); */
/*   /1* this->screen = xcb_setup_roots_iterator(xcb_get_setup((xcb_connection_t*)(this->handle))).data; *1/ */
/*   /1* this->symbols = (void*)(xcb_key_symbols_alloc((xcb_connection_t*)(this->handle))); *1/ */
/*   this->screen = xcb_setup_roots_iterator(xcb_get_setup(conhandle)).data; */
/*   this->symbols = (void*)(xcb_key_symbols_alloc(conhandle)); */

/* } */

/* inline xcb_connection_t* Connection::getHandle() { */
/*   return (xcb_connection_t*)this->handle; */
/* } */

uint32_t newGC(Connection& con, uint32_t mask, uint32_t values[2]) {
  auto result = xcb_generate_id(con.handle());
  xcb_create_gc(con.handle(), result, con.screen()->root, mask, &values[0]);
  return result;
}

} // namespace xcb_hl

using namespace xcl;

/* auto atoms = map<string, xcb_atom_t>(); */
/* const map<string, xcb_atom_t> atoms { */
/*   {"asd", 0} */
/* }; */

/* auto con = Connection(atoms); */
auto con = Connection({"WM_STATE", "WM_NAME", "_NET_ACTIVE_WINDOW"});
uint32_t background, foreground, foreground_muted, buffer;
xcb_window_t subwin;

int vol = 0;
bool muted = false;
const int MAX_VOL = 100;
Device* device;
ServerInfo defaults;
const char* opt_device;
uint32_t col01;

PulseClient pulsecl("paup");

void draw() {
  const auto conhandle = con.handle();
  const uint16_t wheight = 130;
  uint16_t pme = (int)( ( (float)wheight / 100 ) * (float)vol );
  xcb_rectangle_t fg_rects = {0, (int16_t)(wheight - pme), 100, (uint16_t)(pme)};
  xcb_rectangle_t bg_rects = {0, 0, 1024, 1024};

  xcb_poly_fill_rectangle(conhandle, buffer, background, 1, &bg_rects);
  if (muted) {
    xcb_poly_fill_rectangle(conhandle, buffer, foreground_muted, 1, &fg_rects);
  } else {
    xcb_poly_fill_rectangle(conhandle, buffer, foreground, 1, &fg_rects);
  }
  xcb_flush(conhandle);
  xcb_copy_area(conhandle, buffer, subwin, foreground, 0, 0, 0, 0, 200, wheight);
  xcb_flush(conhandle);
}

uint32_t get_colorpixel(uint16_t r,uint16_t g,uint16_t b) {
    #define RGB_8_TO_16(i) (65535 * ((i)&0xFF) / 255)
    int r16 = RGB_8_TO_16(r);
    int g16 = RGB_8_TO_16(g);
    int b16 = RGB_8_TO_16(b);

    xcb_alloc_color_reply_t *reply;

    reply = xcb_alloc_color_reply(con.handle(), xcb_alloc_color(con.handle(), con.screen()->default_colormap,
                                                        r16, g16, b16),
                                  NULL);

    uint32_t pixel = reply->pixel;
    free(reply);

    return pixel;
}

inline void dout(std::string in) {
  std::cout << in;
  std::cout.flush();
}

void init() {
  auto screen = con.screen();
  auto conhandle = con.handle();

  pulsecl.Populate();

  // Get current active window
  auto getwin = xcb_get_input_focus(conhandle);
  auto rep = xcb_get_input_focus_reply(conhandle, getwin, NULL);
  auto parent = rep->focus;

  // Create popup window

  uint32_t windowmask = XCB_EVENT_MASK_EXPOSURE |
                        XCB_EVENT_MASK_KEY_PRESS |
                        XCB_EVENT_MASK_KEY_RELEASE |
                        XCB_EVENT_MASK_BUTTON_PRESS |
                        XCB_EVENT_MASK_FOCUS_CHANGE |
                        XCB_EVENT_MASK_PROPERTY_CHANGE |
                        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                        XCB_EVENT_MASK_LEAVE_WINDOW |
                        XCB_EVENT_MASK_ENTER_WINDOW |
                        XCB_EVENT_MASK_PROPERTY_CHANGE;

  auto window = newXCBSubWindow(con, 40, 130, windowmask, parent);
  subwin = window.handle();

  // Allocate colors
  uint32_t values[2];
  values[1] = 0;

  /* values[0] = get_colorpixel(0x49, 0x48, 0x3E); */
  values[0] = get_colorpixel(0xA6, 0xE2, 0x2E);
  foreground = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

  /* values[0] = get_colorpixel(0x38, 0x38, 0x30); */
  values[0] = get_colorpixel(0xFF, 0x45, 0x35);
  foreground_muted = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

  /* values[0] = get_colorpixel(0x27, 0x28, 0x22); */
  values[0] = get_colorpixel(0x38, 0x38, 0x30);
  background = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

  // Create Pixmap
  buffer = xcb_generate_id(conhandle);
  xcb_create_pixmap_checked(conhandle,
                            screen->root_depth,
                            buffer,
                            window.handle(),
                            1024,
                            1024);

  xcb_map_window(conhandle, window.handle());

  const auto olo = (uint32_t)XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes_checked(conhandle,
                                       screen->root,
                                       XCB_CW_EVENT_MASK,
                                       &olo);

  con.grabKey(0, XK_j);
  con.grabKey(0, XK_k);
  con.grabKey(0, XK_q);
  con.grabKey(0, XK_m);
  con.grabKey(0, XK_Escape);

  xcb_flush(conhandle);

  // Grabkeys
  /* cout << "There is little" << endl; */

  // Init pulseaudio
  defaults = pulsecl.GetDefaults();
  opt_device = defaults.GetDefault(DeviceType::SINK).c_str();
  device = pulsecl.GetDevice(opt_device, DeviceType::SINK);
  vol = device->Volume();
  muted = device->Muted();

  /* cout << device->Volume() << endl; */

  /* pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PONYMIX_VERSION); */
  /* auto device = string_to_device_or_die(ponymix, opt_device, opt_devtype); */
  /* printf("%d\n", device->Volume()); */

  draw();

  // Ev handling
  xcb_generic_event_t* ev;
  std::string logEvent = "";
  while ( (ev = xcb_wait_for_event(conhandle)) ) {

    { // log event
      logEvent = "[";
      if ( auto evName = xcb_event_get_label(ev->response_type); evName != NULL )
        logEvent += std::string(evName);
      else
        logEvent += "UNKNOWN-EVENT";
      logEvent += "]";
    }

    switch(ev->response_type & ~0x80) {
    case XCB_EXPOSE: {
      auto e = (xcb_expose_event_t*)(ev);
      xcb_copy_area(conhandle, buffer, window.handle(), foreground,
                    e->x, e->y, e->x, e->y, e->width, e->height);
      xcb_flush(conhandle);
      break;
    }
    case XCB_FOCUS_IN:
      break;
    case XCB_FOCUS_OUT:
      break;
    case XCB_PROPERTY_NOTIFY: {
      auto e = (xcb_property_notify_event_t*)(ev);
      if (e->atom == con.readAtom("_NET_ACTIVE_WINDOW")) {
        dout("(ITWASME)");
        goto exit;
      }

      break;
    }
    case XCB_KEY_PRESS: {
      logEvent = "";
      dout(".");
      auto e = (xcb_key_press_event_t*)(ev);

      const int col = (e->state && XCB_MOD_MASK_SHIFT);
      const auto sym = xcb_key_press_lookup_keysym(con.symbols(), e, col);

      // cout << sym << "|" << vol << endl;

      switch(sym) {
        case 107:
        case 75:
          if (vol < MAX_VOL) {
            vol += 1;
            pulsecl.SetVolume(*device, vol);
            draw();
          }
          break;
        case 106:
        case 74:
          if (vol > 0) {
            vol -= 1;
            pulsecl.SetVolume(*device, vol);
            draw();
          }
          break;
        case 109:
        case 77:
          muted = !muted;
          pulsecl.SetMute(*device, muted);
          draw();
          break;

        case 113:
          goto exit;
          /* if (vol > 0) { */
          /*   vol -= 1; */
          /*   pulsecl.SetVolume(*device, vol); */
          /*   draw(); */
          /* } */
          break;

      }
      /* let col:cint = (e.state.cint and XCB_MOD_MASK_SHIFT.cint); */
      /* let sym = xcb_key_press_lookup_keysym(con.symbols, e, col) */

      break;
    }
    case XCB_KEY_RELEASE:
      logEvent = "";
      break;
    case XCB_BUTTON_PRESS:
      vol += 1;
      draw();
      // dout("-");
      break;
    default:
      logEvent = "";
      dout("unhandled(");
      if (auto resptypeStr = xcb_event_get_label(ev->response_type); resptypeStr != NULL)
        dout(std::string(resptypeStr));
      else
        dout("UNKNOWN-EVENT");
      dout(")");
      break;
    }

    if (logEvent != "") {
      dout(logEvent);
    }
    if (ev != NULL) {
      free(ev);
    }
  }

exit:
  // dout("BYE");
  return;
}

int main() {
  try {
    init();
    exit(0);
  } catch(std::exception const & ex) {
    dout("[EXCEPTION]");
    dout(std::string(ex.what()));
  }
}

// vim: set et ts=2 sw=2:
