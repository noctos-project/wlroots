#ifndef BACKEND_ILLUMOSFB_H
#define BACKEND_ILLUMOSFB_H

#include <stdint.h>
#include <wlr/backend/illumosfb.h>
#include <wlr/backend/interface.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>

#define ILLUMOSFB_DEFAULT_REFRESH (60 * 1000) // mHz

struct wlr_illumosfb_backend {
	struct wlr_backend backend;
	struct wl_event_loop *event_loop;
	struct wl_listener event_loop_destroy;
	bool started;

	struct wlr_illumosfb_output *output;
	struct wlr_illumosfb_keyboard *keyboard;
	struct wlr_illumosfb_pointer *pointer;
};

struct wlr_illumosfb_output {
	struct wlr_output wlr_output;
	struct wlr_illumosfb_backend *backend;

	int fd;			// /dev/fb
	uint8_t *mem;		// scanout mapping, NULL until started
	size_t mem_len;
	uint32_t width, height;
	uint32_t pitch;		// bytes per scanline
	uint32_t drm_format;
	bool kd_graphics;	// console handed off to us

	struct wl_event_source *frame_timer;
	int frame_delay;	// ms
	bool format_warned;
};

struct wlr_illumosfb_keyboard {
	struct wlr_keyboard wlr_keyboard;
	struct wlr_illumosfb_backend *backend;

	int fd;			// /dev/kbd
	int saved_trans;	// pre-existing KIOCTRANS mode
	struct wl_event_source *event_source;
	uint8_t down[256 / 8];	// pressed HID usages, for typematic de-dup
};

struct wlr_illumosfb_pointer {
	struct wlr_pointer wlr_pointer;
	struct wlr_illumosfb_backend *backend;

	int fd;			// /dev/mouse
	struct wl_event_source *event_source;

	// Last absolute position in screen pixels (usbms scales absolute
	// devices to the MSIOSRESOLUTION we set at create time). usbms only
	// sends an axis when it changes, so both must be remembered.
	double abs_x, abs_y;
};

struct wlr_illumosfb_backend *illumosfb_backend_from_backend(
	struct wlr_backend *wlr_backend);

// output.c
struct wlr_illumosfb_output *illumosfb_output_create(
	struct wlr_illumosfb_backend *backend);
bool illumosfb_output_start(struct wlr_illumosfb_output *output);

// keyboard.c
struct wlr_illumosfb_keyboard *illumosfb_keyboard_create(
	struct wlr_illumosfb_backend *backend);
void illumosfb_keyboard_destroy(struct wlr_illumosfb_keyboard *keyboard);

// pointer.c
struct wlr_illumosfb_pointer *illumosfb_pointer_create(
	struct wlr_illumosfb_backend *backend);
void illumosfb_pointer_destroy(struct wlr_illumosfb_pointer *pointer);

// hid2evdev.c
extern const uint16_t illumosfb_hid2evdev[256];

#endif
