#include <iostream>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_keysyms.h>
#include <typeinfo>

using namespace std;

namespace xcl {
class Connection;
class Window;

class Window {
 public:
  xcb_window_t handle;
  Connection& con;
  uint16_t width, height;

  Window() =delete;
  Window(Connection& con);
};



class Connection {
 public:
  xcb_screen_t* screen;
  vector<Window> windows;
  vector<xcb_atom_t> atoms;
  void* symbols;

  Connection();
  xcb_connection_t* getHandle();

 protected:
  void* handle; // xcb_connection_t
};


Window::Window(Connection& con) : con(con) {
  auto conhandle = con.getHandle();
  this->handle = xcb_generate_id(conhandle);
}

inline Window newXCBSubWindow(Connection& con, uint16_t width, uint16_t height, uint32_t eventmask, xcb_window_t parent) {
  auto w = Window(con);
  const auto conhandle = con.getHandle();
  w.width = width;
  w.height = height;
  const auto mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  const uint32_t values[2] = {1, eventmask};
  xcb_create_window(conhandle,
                    (uint8_t)XCB_COPY_FROM_PARENT,
                    w.handle,
                    parent,
                    (int16_t)20, (int16_t)20,
                    width, height,
                    (uint16_t)5,
                    (uint16_t)XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    con.screen->root_visual,
                    (uint32_t)mask, values);

  return w;
}

Connection::Connection() {
  this->handle = xcb_connect(NULL, NULL);
  const auto conhandle = this->getHandle();
  /* this->screen = xcb_setup_roots_iterator(xcb_get_setup((xcb_connection_t*)(this->handle))).data; */
  /* this->symbols = (void*)(xcb_key_symbols_alloc((xcb_connection_t*)(this->handle))); */
  this->screen = xcb_setup_roots_iterator(xcb_get_setup(conhandle)).data;
  this->symbols = (void*)(xcb_key_symbols_alloc(conhandle));

}

inline xcb_connection_t* Connection::getHandle() {
  return (xcb_connection_t*)this->handle;
}

uint32_t newGC(Connection& con, uint32_t mask, uint32_t values[2]) {
  auto result = xcb_generate_id(con.getHandle());
  xcb_create_gc(con.getHandle(), result, con.screen->root, mask, &values[0]);
  return result;
}

} // namespace xcb_hl

using namespace xcl;

auto con = Connection();
uint32_t background, foreground, buffer;
xcb_window_t subwin;

int vol = 20;

void draw() {
  const auto conhandle = con.getHandle();
  const uint16_t wheight = 130;
  uint16_t pme = (int)( ( (float)wheight / 100 ) * (float)vol );
  xcb_rectangle_t fg_rects = {0, (int16_t)(wheight - pme), 100, (uint16_t)(pme)};
  xcb_rectangle_t bg_rects = {0, 0, 1024, 1024};

  xcb_poly_fill_rectangle(conhandle, buffer, background, 1, &bg_rects);
  xcb_poly_fill_rectangle(conhandle, buffer, foreground, 1, &fg_rects);
  xcb_flush(conhandle);
  xcb_copy_area(conhandle, buffer, subwin, foreground, 0, 0, 0, 0, 200, wheight);
  xcb_flush(conhandle);
}

void init() {
  auto screen = con.screen;
  auto conhandle = con.getHandle();

  // Get current active window
  auto getwin = xcb_get_input_focus(conhandle);
  auto rep = xcb_get_input_focus_reply(conhandle, getwin, NULL);
  auto parent = rep->focus;

  // Create popup window
  uint32_t fg_values[2] = {screen->white_pixel, 0};
  foreground = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, fg_values);

  uint32_t bg_values[2] = {screen->black_pixel, 0};
  background = newGC(con, XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, bg_values);

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
  subwin = window.handle;

  // Create Pixmap
  buffer = xcb_generate_id(conhandle);
  xcb_create_pixmap_checked(conhandle,
                            screen->root_depth,
                            buffer,
                            window.handle,
                            1024,
                            1024);

  xcb_map_window(conhandle, window.handle);

  const auto olo = (uint32_t)XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes_checked(conhandle,
                                       screen->root,
                                       XCB_CW_EVENT_MASK,
                                       &olo);


  xcb_flush(conhandle);

  draw();

  // Grabkeys

  // Ev handling
  xcb_generic_event_t* ev;
  while ( (ev = xcb_wait_for_event(conhandle)) ) {
    switch(ev->response_type & ~0x80) {
      case XCB_EXPOSE: {
        auto e = (xcb_expose_event_t*)(ev);
        xcb_copy_area(conhandle, buffer, window.handle, foreground,
                      e->x, e->y, e->x, e->y, e->width, e->height);
        xcb_flush(conhandle);
        break;
      }
      case XCB_FOCUS_IN: {
        break;
      }
      case XCB_FOCUS_OUT: {
        break;
      }
      case XCB_PROPERTY_NOTIFY: {
        auto e = (xcb_property_notify_event_t*)(ev);
        /* if (e->atom == atom"_NET_ACTIVE_WINDOW") { */
        /* } */

        break;
      }
      default: {}
    }
    cfree(ev);
  }

}

int main() {
  init();
  exit(0);
}

// vim: set et ts=2 sw=2:
