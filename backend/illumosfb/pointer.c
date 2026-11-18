/*
 * illumosfb pointer: VUID firm events from /dev/mouse.
 *
 * VUIDSFORMAT(VUID_FIRM_EVENT) switches the consms stream to Firm_event
 * format: buttons arrive as MS_LEFT/MS_MIDDLE/MS_RIGHT (value 1/0),
 * relative motion as LOC_X_DELTA/LOC_Y_DELTA, and wheels as events in the
 * WHEEL_DEVID segment with a signed delta packed in the low byte of value.
 *
 * Sign conventions (vuidps2 passes raw PS/2 deltas through): positive X =
 * right, positive Y = up, so Y is negated for screen coordinates. Positive
 * wheel delta is taken as scroll-down; if testing shows the wheel inverted,
 * flip WHEEL_SIGN below.
 *
 * Copyright 2026 NoctOS Project. MIT license, as the rest of wlroots.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/msio.h>
#include <sys/vuid_event.h>
#include <sys/vuid_wheel.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/util/log.h>
#include "backend/illumosfb.h"
#include "util/time.h"

#define WHEEL_SIGN (+1.0)
#define WHEEL_SCROLL_UNITS 15.0 // one wheel detent, libinput convention

static const struct wlr_pointer_impl pointer_impl = {
	.name = "illumosfb-pointer",
};

static void emit_button(struct wlr_illumosfb_pointer *pointer,
		int64_t time_msec, uint32_t button, bool pressed) {
	struct wlr_pointer_button_event event = {
		.pointer = &pointer->wlr_pointer,
		.time_msec = time_msec,
		.button = button,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED :
			WL_POINTER_BUTTON_STATE_RELEASED,
	};
	wlr_pointer_notify_button(&pointer->wlr_pointer, &event);
}

static void emit_axis(struct wlr_illumosfb_pointer *pointer,
		int64_t time_msec, enum wl_pointer_axis orientation,
		double detents) {
	struct wlr_pointer_axis_event event = {
		.pointer = &pointer->wlr_pointer,
		.time_msec = time_msec,
		.source = WL_POINTER_AXIS_SOURCE_WHEEL,
		.orientation = orientation,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
		.delta = detents * WHEEL_SCROLL_UNITS,
		.delta_discrete = (int32_t)(detents * WLR_POINTER_AXIS_DISCRETE_STEP),
	};
	wl_signal_emit_mutable(&pointer->wlr_pointer.events.axis, &event);
}

static int handle_pointer_readable(int fd, uint32_t mask, void *data) {
	struct wlr_illumosfb_pointer *pointer = data;

	bool activity = false;
	Firm_event events[64];
	for (;;) {
		ssize_t n = read(fd, events, sizeof(events));
		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
					errno != EINTR) {
				wlr_log_errno(WLR_ERROR, "/dev/mouse read failed");
			}
			break;
		}

		size_t count = (size_t)n / sizeof(events[0]);
		int64_t time_msec = get_current_time_msec();
		double dx = 0, dy = 0;
		bool abs_moved = false;

		for (size_t i = 0; i < count; i++) {
			unsigned int id = events[i].id;
			int value = events[i].value;

			switch (id) {
			case LOC_X_DELTA:
				dx += value;
				continue;
			case LOC_Y_DELTA:
				dy -= value; // VUID positive Y = up
				continue;
			case LOC_X_ABSOLUTE:
				pointer->abs_x = value;
				abs_moved = true;
				continue;
			case LOC_Y_ABSOLUTE:
				pointer->abs_y = value;
				abs_moved = true;
				continue;
			default:
				break;
			}

			if (id == MS_LEFT || id == MS_MIDDLE || id == MS_RIGHT) {
				uint32_t button =
					id == MS_LEFT ? BTN_LEFT :
					id == MS_MIDDLE ? BTN_MIDDLE : BTN_RIGHT;
				emit_button(pointer, time_msec, button,
					value != 0);
				activity = true;
				continue;
			}

			if (vuid_id_addr(id) == vuid_first(WHEEL_DEVID)) {
				double detents = WHEEL_SIGN *
					(double)VUID_WHEEL_GETDELTA(value);
				enum wl_pointer_axis orientation =
					vuid_id_offset(id) == 0 ?
					WL_POINTER_AXIS_VERTICAL_SCROLL :
					WL_POINTER_AXIS_HORIZONTAL_SCROLL;
				emit_axis(pointer, time_msec, orientation,
					detents);
				activity = true;
				continue;
			}
		}

		if (dx != 0 || dy != 0) {
			struct wlr_pointer_motion_event event = {
				.pointer = &pointer->wlr_pointer,
				.time_msec = time_msec,
				.delta_x = dx,
				.delta_y = dy,
				.unaccel_dx = dx,
				.unaccel_dy = dy,
			};
			wl_signal_emit_mutable(
				&pointer->wlr_pointer.events.motion, &event);
			activity = true;
		}

		if (abs_moved) {
			struct wlr_illumosfb_output *output =
				pointer->backend->output;
			double w = output != NULL ? output->width : 1;
			double h = output != NULL ? output->height : 1;
			double x = pointer->abs_x / w;
			double y = pointer->abs_y / h;
			struct wlr_pointer_motion_absolute_event event = {
				.pointer = &pointer->wlr_pointer,
				.time_msec = time_msec,
				.x = x < 0 ? 0 : x > 1 ? 1 : x,
				.y = y < 0 ? 0 : y > 1 ? 1 : y,
			};
			wl_signal_emit_mutable(
				&pointer->wlr_pointer.events.motion_absolute,
				&event);
			activity = true;
		}
	}

	if (activity) {
		wl_signal_emit_mutable(&pointer->wlr_pointer.events.frame, NULL);
	}
	return 0;
}

struct wlr_illumosfb_pointer *illumosfb_pointer_create(
		struct wlr_illumosfb_backend *backend) {
	int fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to open /dev/mouse");
		return NULL;
	}

	int format = VUID_FIRM_EVENT;
	if (ioctl(fd, VUIDSFORMAT, &format) != 0) {
		wlr_log_errno(WLR_ERROR, "VUIDSFORMAT(VUID_FIRM_EVENT) failed");
		close(fd);
		return NULL;
	}

	// Absolute devices (usbms: USB tablets, remote-KVM mice) scale their
	// coordinates to this resolution; until it is set they use a
	// 1024x768 default. Harmless for relative-only devices.
	if (backend->output != NULL) {
		Ms_screen_resolution res = {
			.height = (int)backend->output->height,
			.width = (int)backend->output->width,
		};
		if (ioctl(fd, MSIOSRESOLUTION, &res) != 0) {
			wlr_log_errno(WLR_ERROR, "MSIOSRESOLUTION failed; "
				"absolute coordinates may be mis-scaled");
		}
	}

	struct wlr_illumosfb_pointer *pointer = calloc(1, sizeof(*pointer));
	if (pointer == NULL) {
		close(fd);
		return NULL;
	}
	pointer->backend = backend;
	pointer->fd = fd;

	wlr_pointer_init(&pointer->wlr_pointer, &pointer_impl,
		"illumosfb-pointer");

	pointer->event_source = wl_event_loop_add_fd(backend->event_loop, fd,
		WL_EVENT_READABLE, handle_pointer_readable, pointer);
	if (pointer->event_source == NULL) {
		illumosfb_pointer_destroy(pointer);
		return NULL;
	}

	wlr_log(WLR_INFO, "illumosfb pointer: /dev/mouse in VUID_FIRM_EVENT");
	return pointer;
}

void illumosfb_pointer_destroy(struct wlr_illumosfb_pointer *pointer) {
	if (pointer->event_source != NULL) {
		wl_event_source_remove(pointer->event_source);
	}
	close(pointer->fd);
	wlr_pointer_finish(&pointer->wlr_pointer);
	pointer->backend->pointer = NULL;
	free(pointer);
}
