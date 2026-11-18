/*
 * illumosfb: a wlroots backend for the illumos console framebuffer.
 *
 * Output: the boot-time GOP/VGA linear framebuffer exposed by gfxp_bitmap
 * through /dev/fb (geometry via FBIOGATTR's gfxfb_info, console handoff via
 * KDSETMODE, pixels via mmap). Input: VUID firm events from /dev/kbd
 * (TR_UNTRANS_EVENT; key stations are USB HID usages because conskbd forces
 * KB_USB into every linked keyboard driver) and /dev/mouse (VUID_FIRM_EVENT).
 *
 * Copyright 2026 NoctOS Project. MIT license, as the rest of wlroots.
 */
#include <assert.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>
#include "backend/illumosfb.h"

struct wlr_illumosfb_backend *illumosfb_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_illumosfb(wlr_backend));
	struct wlr_illumosfb_backend *backend =
		wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_illumosfb_backend *backend =
		illumosfb_backend_from_backend(wlr_backend);
	wlr_log(WLR_INFO, "Starting illumosfb backend");

	if (!illumosfb_output_start(backend->output)) {
		return false;
	}
	wl_signal_emit_mutable(&backend->backend.events.new_output,
		&backend->output->wlr_output);

	// Input failure is not fatal: pixels without a mouse still prove a lot.
	backend->keyboard = illumosfb_keyboard_create(backend);
	if (backend->keyboard != NULL) {
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&backend->keyboard->wlr_keyboard.base);
	}
	backend->pointer = illumosfb_pointer_create(backend);
	if (backend->pointer != NULL) {
		wl_signal_emit_mutable(&backend->backend.events.new_input,
			&backend->pointer->wlr_pointer.base);
	}

	backend->started = true;
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_illumosfb_backend *backend =
		illumosfb_backend_from_backend(wlr_backend);
	if (!wlr_backend) {
		return;
	}

	wlr_backend_finish(wlr_backend);

	if (backend->keyboard != NULL) {
		illumosfb_keyboard_destroy(backend->keyboard);
	}
	if (backend->pointer != NULL) {
		illumosfb_pointer_destroy(backend->pointer);
	}
	if (backend->output != NULL) {
		wlr_output_destroy(&backend->output->wlr_output);
	}

	wl_list_remove(&backend->event_loop_destroy.link);

	free(backend);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct wlr_illumosfb_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_illumosfb_backend_create(struct wl_event_loop *loop) {
	wlr_log(WLR_INFO, "Creating illumosfb backend");

	struct wlr_illumosfb_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_illumosfb_backend");
		return NULL;
	}

	wlr_backend_init(&backend->backend, &backend_impl);

	backend->backend.buffer_caps =
		WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM;
	backend->event_loop = loop;

	// Probe /dev/fb now so autocreate can fall through cleanly when there
	// is no console framebuffer (e.g. serial-only machines).
	backend->output = illumosfb_output_create(backend);
	if (backend->output == NULL) {
		free(backend);
		return NULL;
	}

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;
}

bool wlr_backend_is_illumosfb(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
