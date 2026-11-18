/*
 * illumosfb keyboard: VUID firm events from /dev/kbd.
 *
 * KIOCTRANS(TR_UNTRANS_EVENT) makes the console keyboard deliver one
 * Firm_event per key transition with id = the raw key station and
 * value = 1 (press) / 0 (release). conskbd forces every linked keyboard
 * driver into KB_USB mode (CONSSETKBDTYPE at link time), so key stations
 * are USB HID usage codes: kb8042 translates PS/2 scan codes through
 * keytab_pc2usb and usbkbm emits them natively. The HID usage -> evdev
 * keycode mapping is the canonical table vendored in hid2evdev.c.
 *
 * PS/2 typematic repeat arrives as repeated press events with no release;
 * they are dropped here (the `down` bitmap) and key repeat is left to the
 * compositor, as with every wlroots backend.
 *
 * Copyright 2026 NoctOS Project. MIT license, as the rest of wlroots.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kbio.h>
#include <sys/kbd.h>
#include <sys/vuid_event.h>
#include <unistd.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/util/log.h>
#include "backend/illumosfb.h"
#include "util/time.h"

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "illumosfb-keyboard",
};

static bool key_is_down(struct wlr_illumosfb_keyboard *kb, unsigned int id) {
	return kb->down[id / 8] & (1 << (id % 8));
}

static void key_set_down(struct wlr_illumosfb_keyboard *kb, unsigned int id,
		bool down) {
	if (down) {
		kb->down[id / 8] |= 1 << (id % 8);
	} else {
		kb->down[id / 8] &= ~(1 << (id % 8));
	}
}

static int handle_keyboard_readable(int fd, uint32_t mask, void *data) {
	struct wlr_illumosfb_keyboard *kb = data;

	Firm_event events[64];
	for (;;) {
		ssize_t n = read(fd, events, sizeof(events));
		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
					errno != EINTR) {
				wlr_log_errno(WLR_ERROR, "/dev/kbd read failed");
			}
			break;
		}

		size_t count = (size_t)n / sizeof(events[0]);
		int64_t time_msec = get_current_time_msec();
		for (size_t i = 0; i < count; i++) {
			unsigned int id = events[i].id;
			bool pressed = events[i].value != 0;
			if (id >= 256) {
				continue;
			}
			uint16_t keycode = illumosfb_hid2evdev[id];
			if (keycode == 0) { // KEY_RESERVED
				continue;
			}
			if (pressed == key_is_down(kb, id)) {
				continue; // typematic repeat or stale release
			}
			key_set_down(kb, id, pressed);

			struct wlr_keyboard_key_event event = {
				.time_msec = time_msec,
				.keycode = keycode,
				.update_state = true,
				.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
					WL_KEYBOARD_KEY_STATE_RELEASED,
			};
			wlr_keyboard_notify_key(&kb->wlr_keyboard, &event);
		}
	}

	return 0;
}

struct wlr_illumosfb_keyboard *illumosfb_keyboard_create(
		struct wlr_illumosfb_backend *backend) {
	int fd = open("/dev/kbd", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to open /dev/kbd");
		return NULL;
	}

	struct wlr_illumosfb_keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		close(fd);
		return NULL;
	}
	kb->backend = backend;
	kb->fd = fd;
	kb->saved_trans = TR_ASCII;

	if (ioctl(fd, KIOCGTRANS, &kb->saved_trans) != 0) {
		wlr_log_errno(WLR_ERROR, "KIOCGTRANS failed (will restore "
			"TR_ASCII on exit)");
	}

	int trans = TR_UNTRANS_EVENT;
	if (ioctl(fd, KIOCTRANS, &trans) != 0) {
		wlr_log_errno(WLR_ERROR, "KIOCTRANS(TR_UNTRANS_EVENT) failed");
		close(fd);
		free(kb);
		return NULL;
	}

	// Deliver events to this fd instead of the console stream.
	int direct = 1;
	if (ioctl(fd, KIOCSDIRECT, &direct) != 0) {
		wlr_log_errno(WLR_ERROR, "KIOCSDIRECT failed (continuing)");
	}

	wlr_keyboard_init(&kb->wlr_keyboard, &keyboard_impl,
		"illumosfb-keyboard");

	kb->event_source = wl_event_loop_add_fd(backend->event_loop, fd,
		WL_EVENT_READABLE, handle_keyboard_readable, kb);
	if (kb->event_source == NULL) {
		illumosfb_keyboard_destroy(kb);
		return NULL;
	}

	wlr_log(WLR_INFO, "illumosfb keyboard: /dev/kbd in TR_UNTRANS_EVENT "
		"(KB_USB key stations)");
	return kb;
}

void illumosfb_keyboard_destroy(struct wlr_illumosfb_keyboard *kb) {
	if (kb->event_source != NULL) {
		wl_event_source_remove(kb->event_source);
	}

	int direct = 0;
	(void) ioctl(kb->fd, KIOCSDIRECT, &direct);
	if (ioctl(kb->fd, KIOCTRANS, &kb->saved_trans) != 0) {
		wlr_log_errno(WLR_ERROR, "failed to restore keyboard "
			"translation mode");
	}
	close(kb->fd);

	wlr_keyboard_finish(&kb->wlr_keyboard);
	kb->backend->keyboard = NULL;
	free(kb);
}
