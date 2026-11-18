/*
 * illumosfb output: scanout to the console framebuffer.
 *
 * The contract (verified by harness/fbtest.c, 2026-11-17):
 *   open /dev/fb -> FBIOGATTR fills struct fbgattr; fbtype carries
 *   width/height/depth and sattr.dev_specific carries struct gfxfb_info
 *   (pitch + RGB mask layout) -> KDSETMODE(KD_GRAPHICS) stops the terminal
 *   emulator from painting -> mmap(len = pitch * height) is the live
 *   scanout surface. KDSETMODE(KD_TEXT) hands the console back.
 *
 * Copyright 2026 NoctOS Project. MIT license, as the rest of wlroots.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fbio.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>
#include "backend/illumosfb.h"
#include "types/wlr_output.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_ENABLED;

static struct wlr_illumosfb_output *illumosfb_output_from_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output_is_illumosfb(wlr_output));
	struct wlr_illumosfb_output *output =
		wl_container_of(wlr_output, output, wlr_output);
	return output;
}

static uint32_t format_from_masks(const struct gfxfb_info *info) {
	if (info->red_mask_size == 8 && info->green_mask_size == 8 &&
			info->blue_mask_size == 8 &&
			info->green_field_position == 8) {
		if (info->red_field_position == 16 &&
				info->blue_field_position == 0) {
			return DRM_FORMAT_XRGB8888;
		}
		if (info->red_field_position == 0 &&
				info->blue_field_position == 16) {
			return DRM_FORMAT_XBGR8888;
		}
	}
	wlr_log(WLR_ERROR,
		"Unrecognized framebuffer mask layout r%u@%u g%u@%u b%u@%u, "
		"assuming XRGB8888",
		info->red_mask_size, info->red_field_position,
		info->green_mask_size, info->green_field_position,
		info->blue_mask_size, info->blue_field_position);
	return DRM_FORMAT_XRGB8888;
}

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}
	return true;
}

static void output_blit(struct wlr_illumosfb_output *output,
		struct wlr_buffer *buffer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "Failed to access buffer data for scanout");
		return;
	}

	if (format != output->drm_format && !output->format_warned) {
		wlr_log(WLR_ERROR, "Buffer format 0x%"PRIx32" does not match "
			"framebuffer format 0x%"PRIx32"; colors may be wrong",
			format, output->drm_format);
		output->format_warned = true;
	}

	uint32_t height = (uint32_t)buffer->height < output->height ?
		(uint32_t)buffer->height : output->height;
	uint32_t width = (uint32_t)buffer->width < output->width ?
		(uint32_t)buffer->width : output->width;
	size_t row_bytes = (size_t)width * 4;
	if (row_bytes > stride) {
		row_bytes = stride;
	}
	if (row_bytes > output->pitch) {
		row_bytes = output->pitch;
	}

	const uint8_t *src = data;
	uint8_t *dst = output->mem;
	for (uint32_t y = 0; y < height; y++) {
		memcpy(dst, src, row_bytes);
		src += stride;
		dst += output->pitch;
	}

	wlr_buffer_end_data_ptr_access(buffer);
}

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_illumosfb_output *output =
		illumosfb_output_from_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	if (output_pending_enabled(wlr_output, state)) {
		if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
				output->mem != NULL) {
			output_blit(output, state->buffer);
		}

		struct wlr_output_event_present present_event = {
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = true,
		};
		output_defer_present(wlr_output, present_event);

		wl_event_source_timer_update(output->frame_timer,
			output->frame_delay);
	}

	return true;
}

static void output_restore_console(struct wlr_illumosfb_output *output) {
	if (output->mem != NULL) {
		munmap(output->mem, output->mem_len);
		output->mem = NULL;
	}
	if (output->kd_graphics) {
		if (ioctl(output->fd, KDSETMODE, KD_TEXT) != 0) {
			wlr_log_errno(WLR_ERROR, "KDSETMODE(KD_TEXT) failed");
		}
		output->kd_graphics = false;
	}
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_illumosfb_output *output =
		illumosfb_output_from_output(wlr_output);

	wlr_output_finish(wlr_output);

	if (output->frame_timer != NULL) {
		wl_event_source_remove(output->frame_timer);
	}
	output_restore_console(output);
	close(output->fd);
	output->backend->output = NULL;
	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
};

bool wlr_output_is_illumosfb(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(void *data) {
	struct wlr_illumosfb_output *output = data;
	wlr_output_send_frame(&output->wlr_output);
	return 0;
}

struct wlr_illumosfb_output *illumosfb_output_create(
		struct wlr_illumosfb_backend *backend) {
	int fd = open("/dev/fb", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to open /dev/fb");
		return NULL;
	}

	struct fbgattr attr;
	if (ioctl(fd, FBIOGATTR, &attr) != 0) {
		wlr_log_errno(WLR_ERROR, "FBIOGATTR failed on /dev/fb");
		close(fd);
		return NULL;
	}

	const struct gfxfb_info *info =
		(const struct gfxfb_info *)&attr.sattr.dev_specific;

	if (attr.fbtype.fb_depth != 32) {
		wlr_log(WLR_ERROR, "Unsupported framebuffer depth %d "
			"(only 32 bpp is supported)", attr.fbtype.fb_depth);
		close(fd);
		return NULL;
	}

	struct wlr_illumosfb_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		close(fd);
		return NULL;
	}
	output->backend = backend;
	output->fd = fd;
	output->width = attr.fbtype.fb_width;
	output->height = attr.fbtype.fb_height;
	output->pitch = info->pitch != 0 ?
		info->pitch : output->width * 4;
	output->mem_len = (size_t)output->pitch * output->height;
	output->drm_format = format_from_masks(info);
	output->frame_delay = 1000000 / ILLUMOSFB_DEFAULT_REFRESH; // ms

	struct wlr_output *wlr_output = &output->wlr_output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state,
		output->width, output->height, ILLUMOSFB_DEFAULT_REFRESH);

	wlr_output_init(wlr_output, &backend->backend, &output_impl,
		backend->event_loop, &state);
	wlr_output_state_finish(&state);

	wlr_output_set_name(wlr_output, "ILLUMOSFB-1");

	char description[128];
	snprintf(description, sizeof(description),
		"illumos console framebuffer (%ux%u, pitch %u)",
		output->width, output->height, output->pitch);
	wlr_output_set_description(wlr_output, description);

	output->frame_timer = wl_event_loop_add_timer(backend->event_loop,
		signal_frame, output);

	wlr_log(WLR_INFO, "illumosfb output: %ux%u depth 32 pitch %u "
		"format 0x%"PRIx32, output->width, output->height,
		output->pitch, output->drm_format);

	return output;
}

bool illumosfb_output_start(struct wlr_illumosfb_output *output) {
	if (ioctl(output->fd, KDSETMODE, KD_GRAPHICS) != 0) {
		wlr_log_errno(WLR_ERROR, "KDSETMODE(KD_GRAPHICS) failed");
		return false;
	}
	output->kd_graphics = true;

	output->mem = mmap(NULL, output->mem_len, PROT_READ | PROT_WRITE,
		MAP_SHARED, output->fd, 0);
	if (output->mem == MAP_FAILED) {
		wlr_log_errno(WLR_ERROR, "Failed to mmap /dev/fb");
		output->mem = NULL;
		output_restore_console(output);
		return false;
	}

	return true;
}
