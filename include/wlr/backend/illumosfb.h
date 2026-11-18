/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_ILLUMOSFB_H
#define WLR_BACKEND_ILLUMOSFB_H

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

/**
 * Creates an illumos framebuffer backend. The backend scans out to the
 * system console framebuffer (/dev/fb, the boot-time GOP/VGA mode exposed
 * by gfxp_bitmap) and reads input as VUID firm events from /dev/kbd and
 * /dev/mouse. One output, fixed mode; software rendering only
 * (WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM).
 *
 * Returns NULL if /dev/fb cannot be opened or probed.
 */
struct wlr_backend *wlr_illumosfb_backend_create(struct wl_event_loop *loop);

bool wlr_backend_is_illumosfb(struct wlr_backend *backend);
bool wlr_output_is_illumosfb(struct wlr_output *output);

#endif
