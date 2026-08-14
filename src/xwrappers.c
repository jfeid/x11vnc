/*
   Copyright (C) 2002-2010 Karl J. Runge <runge@karlrunge.com> 
   All rights reserved.

This file is part of x11vnc.

x11vnc is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or (at
your option) any later version.

x11vnc is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with x11vnc; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA
or see <http://www.gnu.org/licenses/>.

In addition, as a special exception, Karl J. Runge
gives permission to link the code of its release of x11vnc with the
OpenSSL project's "OpenSSL" library (or with modified versions of it
that use the same license as the "OpenSSL" library), and distribute
the linked executables.  You must obey the GNU General Public License
in all respects for all of the code used other than "OpenSSL".  If you
modify this file, you may extend this exception to your version of the
file, but you are not obligated to do so.  If you do not wish to do
so, delete this exception statement from your version.
*/

/* -- xwrappers.c -- */

#include "x11vnc.h"
#include "xrecord.h"
#include "keyboard.h"
#include "xevents.h"
#include "connections.h"
#include "cleanup.h"
#include "macosx.h"
#include "xi2_devices.h"
#include "xwrappers.h"

int xshm_present = 0;
int xshm_opcode = 0;
int xtest_present = 0;
int xtrap_present = 0;
int xrecord_present = 0;
int xkb_present = 0;
int xinerama_present = 0;

int keycode_state[256];
int rootshift = 0;
int clipshift = 0;

#if HAVE_NVFBC
#include <sys/time.h>

/* NVFBC capture state */
static int nvfbc_initialized = 0;
static int nvfbc_capture_active = 0;
static uint8_t *nvfbc_frame_buffer = NULL;
static nvfbc_frame_info_t nvfbc_last_frame;

/*
 * Offset from x11vnc's served coordinate space into the captured frame.
 *
 * copy_image() callers work in clipped coordinates; the X paths translate via
 * ADJUST_ROOTSHIFT.  When the capture is cropped to exactly the served region
 * these are zero, but they must still be honoured when NVFBC hands back a
 * larger frame than we serve.
 */
static int nvfbc_src_dx = 0, nvfbc_src_dy = 0;

/*
 * One grab per scan cycle.
 *
 * copy_image() is x11vnc's per-scanline and per-tile read primitive, so
 * grabbing inside it issued a fresh full-frame capture hundreds of times per
 * displayed frame.  Worse, the NVFBC buffer is a single buffer the driver
 * overwrites on the next grab, so a scan could assemble main_fb out of
 * several different frames and tear.
 *
 * scan_for_updates() now bumps the epoch once per cycle and grabs eagerly
 * (outside X_LOCK); every read in that cycle is served from that one frame.
 */
static unsigned long nvfbc_epoch = 0;
static unsigned long nvfbc_grabbed_epoch = ~0UL;
static int nvfbc_frame_ok = 0;
static int nvfbc_in_scan_cycle = 0;

/* Chosen once at init; reused verbatim when a modeset forces a restart. */
static nvfbc_config_t nvfbc_cfg;

/* NVFBC performance tracking */
static unsigned long nvfbc_frame_count = 0;
static unsigned long nvfbc_new_frame_count = 0;
static unsigned long nvfbc_direct_count = 0;
static struct timeval nvfbc_stats_start;
static int nvfbc_stats_initialized = 0;

/*
 * Start (or restart) capture from nvfbc_cfg.  Returns 1 on success.
 * Keeping this in one place stops the modeset-recovery path from drifting
 * away from the configuration chosen at init.
 */
static int nvfbc_start_configured_capture(void) {
	nvfbc_cap_status_t status = nvfbc_start_capture(&nvfbc_cfg);

	if (status != NVFBC_CAP_OK) {
		rfbLog("NVFBC: Failed to start capture: %s\n", nvfbc_status_string(status));
		rfbLog("NVFBC: %s\n", nvfbc_get_last_error());
		return 0;
	}
	return 1;
}

/*
 * Initialize NVFBC capture if enabled.
 * Returns 1 on success, 0 on failure.
 */
int nvfbc_capture_init(void) {
	nvfbc_cap_status_t status;
	nvfbc_status_t nvfbc_status;
	nvfbc_config_t config;
	int reg_x, reg_y, reg_w, reg_h, cap_x, cap_y;
	uint32_t out_id = 0;
	const char *out_name = NULL;

	if (!use_nvfbc) {
		return 0;
	}

	if (nvfbc_initialized) {
		return 1;
	}

	/*
	 * NVFBC can only hand back screen or per-output contents.  With -id/-sid
	 * and no -rootshift the X paths read the window drawable directly, which
	 * NVFBC cannot reproduce, so capture would silently show the wrong
	 * pixels.  Refuse rather than serve garbage.
	 */
	if (subwin && !rootshift) {
		rfbLog("NVFBC: -id/-sid without -rootshift captures a window, which "
		    "NVFBC cannot do; disabling NVFBC.\n");
		use_nvfbc = 0;
		return 0;
	}

	rfbLog("NVFBC: Initializing NVIDIA Frame Buffer Capture...\n");

	status = nvfbc_init();
	if (status != NVFBC_CAP_OK) {
		rfbLog("NVFBC: Initialization failed: %s\n", nvfbc_status_string(status));
		rfbLog("NVFBC: %s\n", nvfbc_get_last_error());
		use_nvfbc = 0;
		return 0;
	}

	/* Get display status */
	status = nvfbc_get_status(&nvfbc_status);
	if (status != NVFBC_CAP_OK) {
		rfbLog("NVFBC: Failed to get status: %s\n", nvfbc_status_string(status));
		nvfbc_cleanup();
		use_nvfbc = 0;
		return 0;
	}

	if (!nvfbc_status.is_capture_possible) {
		rfbLog("NVFBC: Capture not possible on this display\n");
		nvfbc_cleanup();
		use_nvfbc = 0;
		return 0;
	}

	rfbLog("NVFBC: Screen size: %ux%u\n", nvfbc_status.screen_width, nvfbc_status.screen_height);
	rfbLog("NVFBC: Outputs: %u\n", nvfbc_status.num_outputs);

	/*
	 * Work out the region x11vnc actually serves, in root coordinates.
	 * This mirrors what ADJUST_ROOTSHIFT does for the X read paths.
	 */
	reg_x = clipshift ? coff_x : 0;
	reg_y = clipshift ? coff_y : 0;
	reg_w = dpy_x;
	reg_h = dpy_y;
	if (subwin && rootshift) {
		reg_x += off_x;
		reg_y += off_y;
	}

	/* Configure capture */
	memset(&config, 0, sizeof(config));
	config.with_cursor = nvfbc_with_cursor;
	config.with_diff_map = nvfbc_with_diffmap;
	/*
	 * One diffmap cell per x11vnc tile, so the map can drive tile_has_diff[]
	 * directly.  Both grids are ceil(size/32), so they line up exactly.
	 */
	config.diff_map_scale = tile_x;
	config.sampling_rate_ms = 16;  /* ~60 Hz; ignored when push_model is on */
	config.track_output = -1;
	config.push_model = nvfbc_push_model;
	config.allow_direct_capture = nvfbc_direct_capture;

	/*
	 * Capture only what we serve.  Tracking a single output is cheapest, so
	 * prefer it when the served region is exactly one monitor; otherwise
	 * crop the screen.  Either way the captured frame's origin lines up with
	 * our coordinate space, which is also what makes -clip correct here.
	 */
	if (nvfbc_find_output_by_box(&nvfbc_status, (uint32_t)reg_x, (uint32_t)reg_y,
	    (uint32_t)reg_w, (uint32_t)reg_h, &out_id, &out_name)) {
		config.track_output = (int)out_id;
		cap_x = reg_x;
		cap_y = reg_y;
		rfbLog("NVFBC: tracking output %u (%s) %dx%d+%d+%d\n",
		    out_id, out_name ? out_name : "?", reg_w, reg_h, reg_x, reg_y);
	} else if (reg_w < (int)nvfbc_status.screen_width ||
	           reg_h < (int)nvfbc_status.screen_height) {
		config.box_x = (uint32_t)reg_x;
		config.box_y = (uint32_t)reg_y;
		config.box_w = (uint32_t)reg_w;
		config.box_h = (uint32_t)reg_h;
		cap_x = reg_x;
		cap_y = reg_y;
		rfbLog("NVFBC: cropping screen to %dx%d+%d+%d\n",
		    reg_w, reg_h, reg_x, reg_y);
	} else {
		cap_x = 0;
		cap_y = 0;
		rfbLog("NVFBC: capturing full screen %ux%u\n",
		    nvfbc_status.screen_width, nvfbc_status.screen_height);
	}

	nvfbc_src_dx = reg_x - cap_x;
	nvfbc_src_dy = reg_y - cap_y;

	memcpy(&nvfbc_cfg, &config, sizeof(nvfbc_cfg));

	if (!nvfbc_start_configured_capture()) {
		nvfbc_cleanup();
		use_nvfbc = 0;
		return 0;
	}

	nvfbc_initialized = 1;
	nvfbc_capture_active = 1;
	rfbLog("NVFBC: Capture initialized successfully (cursor=%d, diffmap=%d)\n",
	       nvfbc_with_cursor, nvfbc_with_diffmap);
	return 1;
}

/*
 * Cleanup NVFBC capture.
 */
void nvfbc_capture_cleanup(void) {
	if (nvfbc_initialized) {
		rfbLog("NVFBC: Cleaning up...\n");
		nvfbc_stop_capture();
		nvfbc_cleanup();
		nvfbc_initialized = 0;
		nvfbc_capture_active = 0;
		nvfbc_frame_buffer = NULL;
	}
}

/*
 * Check if NVFBC capture is active.
 */
int nvfbc_capture_is_active(void) {
	return nvfbc_capture_active;
}

/*
 * Capture a frame using NVFBC into the destination XImage.
 * The XImage should be in BGRA format for best performance.
 * Returns 1 on success, 0 on failure.
 */
static int nvfbc_grab_current(void) {
	nvfbc_cap_status_t status;
	struct timeval now;
	double elapsed;

	if (!nvfbc_capture_active) {
		return 0;
	}

	/* Already have this cycle's frame: reuse it, no driver call. */
	if (nvfbc_grabbed_epoch == nvfbc_epoch) {
		return nvfbc_frame_ok;
	}
	nvfbc_grabbed_epoch = nvfbc_epoch;
	nvfbc_frame_ok = 0;

	/* Non-blocking: a blocking grab here would stall the scan and, with it,
	 * anything waiting on X_LOCK. */
	status = nvfbc_grab_frame(&nvfbc_frame_buffer, &nvfbc_last_frame, 0);
	if (status == NVFBC_CAP_ERR_RECREATE) {
		rfbLog("NVFBC: Display mode changed, reinitializing...\n");
		nvfbc_stop_capture();
		nvfbc_capture_active = 0;

		if (!nvfbc_start_configured_capture()) {
			return 0;
		}
		nvfbc_capture_active = 1;
		status = nvfbc_grab_frame(&nvfbc_frame_buffer, &nvfbc_last_frame, 0);
	}

	if (status != NVFBC_CAP_OK) {
		rfbLog("NVFBC: Frame grab failed: %s\n", nvfbc_status_string(status));
		return 0;
	}
	if (!nvfbc_frame_buffer) {
		return 0;
	}

	/* Performance tracking.  One grab per cycle now, so this is per-cycle
	 * rather than per-scanline; the log format is unchanged so existing
	 * tooling keeps parsing it. */
	gettimeofday(&now, NULL);
	if (!nvfbc_stats_initialized) {
		nvfbc_stats_start = now;
		nvfbc_stats_initialized = 1;
		nvfbc_frame_count = 0;
		nvfbc_new_frame_count = 0;
	}
	nvfbc_frame_count++;
	if (nvfbc_last_frame.is_new_frame) {
		nvfbc_new_frame_count++;
		if (nvfbc_last_frame.is_direct_capture) {
			nvfbc_direct_count++;
		}
	}
	elapsed = (now.tv_sec - nvfbc_stats_start.tv_sec) +
	          (now.tv_usec - nvfbc_stats_start.tv_usec) / 1000000.0;
	if (elapsed >= 10.0) {
		/* appended, not inserted: existing tooling parses the prefix */
		rfbLog("NVFBC stats: %.1f new fps, %.0f grabs/sec, %lu new frames / %lu total grabs%s\n",
		       nvfbc_new_frame_count / elapsed, nvfbc_frame_count / elapsed,
		       nvfbc_new_frame_count, nvfbc_frame_count,
		       nvfbc_direct_count ? " [direct capture active]" : "");
		nvfbc_stats_start = now;
		nvfbc_frame_count = 0;
		nvfbc_new_frame_count = 0;
		nvfbc_direct_count = 0;
	}

	nvfbc_frame_ok = 1;
	return 1;
}

/*
 * Begin a capture cycle: take one coherent frame for the whole scan.
 *
 * Called from scan_for_updates() before it takes X_LOCK.  The grab itself
 * touches no X state, and a new-frame grab costs milliseconds, so doing it
 * under X_LOCK stalled XTest input for the duration.
 */
void nvfbc_begin_frame(void) {
	if (!use_nvfbc || !nvfbc_capture_active) {
		return;
	}
	NVFBC_LOCK;
	nvfbc_epoch++;
	nvfbc_grab_current();
	NVFBC_UNLOCK;
}

/*
 * Force the next read to re-grab rather than reuse this cycle's frame.
 *
 * Only for callers that run outside a scan cycle and genuinely need "now";
 * calling it inside a cycle throws away the frame the cycle was built on and
 * buys a redundant full-frame DMA.
 */
void nvfbc_invalidate_frame(void) {
	NVFBC_LOCK;
	nvfbc_grabbed_epoch = ~0UL;
	NVFBC_UNLOCK;
}

/* Set around the whole-screen copy that happens inside a scan cycle. */
void nvfbc_set_in_scan_cycle(int v) {
	nvfbc_in_scan_cycle = v;
}

/*
 * Freshness rule for copy_screen(), which is reached from both sides:
 *
 *  - inside a scan cycle (the fs_frac whole-screen path) the cycle's frame is
 *    exactly what the diff map was computed against, and re-grabbing costs a
 *    redundant full-frame DMA (~3ms, measured as a 12% frame-rate hit);
 *  - outside one (client connect, resize, startup) there may be no next cycle
 *    soon - a napping server can sit idle for a long time - so a stale frame
 *    would stay on screen.  Grab.
 */
void nvfbc_invalidate_if_out_of_cycle(void) {
	if (!nvfbc_in_scan_cycle) {
		nvfbc_invalidate_frame();
	}
}

/*
 * Did this cycle's grab yield a frame the display had not produced before?
 * If not, no pixel can have changed and the whole scan can be skipped.
 * Returns -1 when the answer is not known (no usable frame).
 */
int nvfbc_frame_is_new(void) {
	int ret;

	NVFBC_LOCK;
	if (!use_nvfbc || !nvfbc_capture_active || !nvfbc_frame_ok) {
		ret = -1;
	} else {
		ret = nvfbc_last_frame.is_new_frame ? 1 : 0;
	}
	NVFBC_UNLOCK;
	return ret;
}

/*
 * Capture a frame using NVFBC into the destination XImage.
 * The XImage should be in BGRA format for best performance.
 * Returns 1 on success, 0 on failure.
 */
static int nvfbc_copy_to_ximage(XImage *dest, int x, int y, unsigned int w, unsigned int h) {
	uint8_t *src_row, *dst_row;
	unsigned int line;
	int src_stride, dst_stride;
	int pixelsize;
	int sx, sy;
	int avail_w, avail_h;
	int ok;

	NVFBC_LOCK;
	ok = nvfbc_grab_current();
	if (!ok) {
		NVFBC_UNLOCK;
		return 0;
	}

	pixelsize = dest->bits_per_pixel / 8;
	if (pixelsize != 4 && pixelsize != 3) {
		NVFBC_UNLOCK;
		return 0;	/* unsupported depth: let the X paths handle it */
	}

	src_stride = (int)nvfbc_last_frame.width * 4;	/* NVFBC frame is BGRA */
	dst_stride = dest->bytes_per_line;

	/* Translate from x11vnc's served coordinates into the captured frame. */
	sx = x + nvfbc_src_dx;
	sy = y + nvfbc_src_dy;
	if (sx < 0 || sy < 0) {
		NVFBC_UNLOCK;
		return 0;
	}

	/*
	 * Clamp in signed arithmetic.  The old form compared "x + w" against a
	 * uint32_t, which promoted x and turned any overhang into a huge width.
	 */
	avail_w = (int)nvfbc_last_frame.width - sx;
	avail_h = (int)nvfbc_last_frame.height - sy;
	if (avail_w <= 0 || avail_h <= 0) {
		NVFBC_UNLOCK;
		return 0;
	}
	if ((int)w > avail_w) {
		w = (unsigned int)avail_w;
	}
	if ((int)h > avail_h) {
		h = (unsigned int)avail_h;
	}
	if ((int)h > dest->height) {
		h = (unsigned int)dest->height;
	}

	for (line = 0; line < h; line++) {
		src_row = nvfbc_frame_buffer + (size_t)(sy + line) * src_stride + (size_t)sx * 4;
		dst_row = (uint8_t *)dest->data + (size_t)line * dst_stride;

		if (pixelsize == 4) {
			memcpy(dst_row, src_row, (size_t)w * 4);
		} else {
			/* Convert BGRA to BGR for 24bpp */
			unsigned int px;
			for (px = 0; px < w; px++) {
				dst_row[px * 3 + 0] = src_row[px * 4 + 0];  /* B */
				dst_row[px * 3 + 1] = src_row[px * 4 + 1];  /* G */
				dst_row[px * 3 + 2] = src_row[px * 4 + 2];  /* R */
			}
		}
	}

	NVFBC_UNLOCK;
	return 1;
}

/*
 * Mark changed tiles straight from NVFBC's differential map.
 *
 * The map is generated on the GPU as a side effect of capture and, measured
 * against an independent per-tile memcmp, never misses a change (it does
 * over-report, which only costs a redundant tile copy).  It is a delta
 * against the last frame we *captured*, so skipping generated frames is safe.
 *
 * With dwDiffMapScalingFactor == tile_x both grids are ceil(size/32), so a
 * cell maps onto a tile 1:1 when the capture is cropped to the served region;
 * the general form below also handles an uncropped capture.
 *
 * Returns the number of tiles marked, or -1 if the map is unusable.
 */
int nvfbc_mark_tiles_from_diffmap(void) {
	uint8_t *map = NULL;
	uint32_t mw = 0, mh = 0;
	uint32_t scale = nvfbc_cfg.diff_map_scale;
	int tx, ty, count = 0;

	NVFBC_LOCK;
	if (!use_nvfbc || !nvfbc_capture_active || !nvfbc_frame_ok ||
	    !nvfbc_cfg.with_diff_map || scale == 0 ||
	    tile_has_diff == NULL || ntiles_x <= 0 || ntiles_y <= 0 ||
	    nvfbc_get_diff_map(&map, &mw, &mh) != NVFBC_CAP_OK || !map) {
		NVFBC_UNLOCK;
		return -1;
	}

	for (ty = 0; ty < ntiles_y; ty++) {
		for (tx = 0; tx < ntiles_x; tx++) {
			/* tile rect in served coords -> frame coords -> map cells */
			int fx0 = tx * tile_x + nvfbc_src_dx;
			int fy0 = ty * tile_y + nvfbc_src_dy;
			int fx1 = fx0 + tile_x - 1;
			int fy1 = fy0 + tile_y - 1;
			int cx0, cx1, cy0, cy1, cx, cy, dirty = 0;

			if (fx0 < 0) fx0 = 0;
			if (fy0 < 0) fy0 = 0;
			if (fx1 > (int)nvfbc_last_frame.width - 1)
				fx1 = (int)nvfbc_last_frame.width - 1;
			if (fy1 > (int)nvfbc_last_frame.height - 1)
				fy1 = (int)nvfbc_last_frame.height - 1;
			if (fx1 < fx0 || fy1 < fy0) {
				continue;
			}

			cx0 = fx0 / (int)scale;  cx1 = fx1 / (int)scale;
			cy0 = fy0 / (int)scale;  cy1 = fy1 / (int)scale;
			if (cx1 > (int)mw - 1) cx1 = (int)mw - 1;
			if (cy1 > (int)mh - 1) cy1 = (int)mh - 1;

			for (cy = cy0; cy <= cy1 && !dirty; cy++) {
				for (cx = cx0; cx <= cx1; cx++) {
					if (map[(size_t)cy * mw + cx]) {
						dirty = 1;
						break;
					}
				}
			}
			if (dirty) {
				tile_has_diff[tx + ty * ntiles_x] = 1;
				count++;
			}
		}
	}
	NVFBC_UNLOCK;
	return count;
}
#endif /* HAVE_NVFBC */

int guess_bits_per_color(int bits_per_pixel);

int XFlush_wr(Display *disp);

Status XShmGetImage_wr(Display *disp, Drawable d, XImage *image, int x, int y,
    unsigned long mask);
XImage *XShmCreateImage_wr(Display* disp, Visual* vis, unsigned int depth,
    int format, char* data, XShmSegmentInfo* shminfo, unsigned int width,
    unsigned int height);
Status XShmAttach_wr(Display *disp, XShmSegmentInfo *shminfo);
Status XShmDetach_wr(Display *disp, XShmSegmentInfo *shminfo);
Bool XShmQueryExtension_wr(Display *disp);
int XShmGetEventBase_wr(Display *disp);

XImage *xreadscreen(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, Bool show_cursor);
XImage *XGetSubImage_wr(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, unsigned long plane_mask,
    int format, XImage *dest_image, int dest_x, int dest_y);
XImage *XGetImage_wr(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, unsigned long plane_mask,
    int format);
XImage *XCreateImage_wr(Display *disp, Visual *visual, unsigned int depth,
    int format, int offset, char *data, unsigned int width,
    unsigned int height, int bitmap_pad, int bytes_per_line);
void copy_image(XImage *dest, int x, int y, unsigned int w, unsigned int h);
void init_track_keycode_state(void);

void XTRAP_FakeKeyEvent_wr(Display* dpy, KeyCode key, Bool down,
    unsigned long delay);
void XTestFakeKeyEvent_wr(Display* dpy, int dev_id, KeyCode key, Bool down,
    unsigned long delay);
void XTRAP_FakeButtonEvent_wr(Display* dpy, unsigned int button, Bool is_press,
    unsigned long delay);
void XTestFakeButtonEvent_wr(Display* dpy, int dev_id, unsigned int button, Bool is_press,
    unsigned long delay);
void XTRAP_FakeMotionEvent_wr(Display* dpy, int screen, int x, int y,
    unsigned long delay);
void XTestFakeMotionEvent_wr(Display* dpy, int dev_id, int screen, int x, int y,
    unsigned long delay);

Bool XTestCompareCurrentCursorWithWindow_wr(Display* dpy, Window w);
Bool XTestCompareCursorWithWindow_wr(Display* dpy, Window w, Cursor cursor);
Bool XTestQueryExtension_wr(Display *dpy, int *ev, int *er, int *maj,
    int *min);
void XTestDiscard_wr(Display *dpy);
Bool XETrapQueryExtension_wr(Display *dpy, int *ev, int *er, int *op);
int XTestGrabControl_wr(Display *dpy, Bool impervious);
int XTRAP_GrabControl_wr(Display *dpy, Bool impervious);
void disable_grabserver(Display *in_dpy, int change);

Bool XRecordQueryVersion_wr(Display *dpy, int *maj, int *min);

int xauth_raw(int on);
Display *XOpenDisplay_wr(char *display_name);
int XCloseDisplay_wr(Display *display);

Bool XQueryPointer_wr(Display *display, Window w, Window *root_return,
    Window *child_return, int *root_x_return, int *root_y_return,
    int *win_x_return, int *win_y_return, unsigned int *mask_return);

Status XQueryTree_wr(Display *display, Window w, Window *root_return,
    Window *parent_return, Window **children_return,
    unsigned int *nchildren_return);

int XFree_wr(void *data);
int XSelectInput_wr(Display *display, Window w, long event_mask);

void copy_raw_fb(XImage *dest, int x, int y, unsigned int w, unsigned int h);
static void upup_downdown_warning(KeyCode key, Bool down);

/* 
 * used in rfbGetScreen and rfbNewFramebuffer: and estimate to the number
 * of bits per color, of course for some visuals, e.g. 565, the number
 * is not the same for each color.  This is just a sane default.
 */
int guess_bits_per_color(int bits_per_pixel) {
	int bits_per_color;
	
	/* first guess, spread them "evenly" over R, G, and B */
	bits_per_color = bits_per_pixel/3;
	if (bits_per_color < 1) {
		bits_per_color = 1;	/* 1bpp, 2bpp... */
	}

	/* choose safe values for usual cases: */
	if (bits_per_pixel == 8) {
		bits_per_color = 2;
	} else if (bits_per_pixel == 15 || bits_per_pixel == 16) {
		bits_per_color = 5;
	} else if (bits_per_pixel == 24 || bits_per_pixel == 32) {
		bits_per_color = 8;
	}
	return bits_per_color;
}

int XFlush_wr(Display *disp) {
#if NO_X11
	if (!disp) {}
	return 1;
#else
	if (disp) {
		return XFlush(disp);
	} else {
		return 1;
	}
#endif	/* NO_X11 */
}

/*
 * Kludge to interpose image gets and limit to a subset rectangle of
 * the rootwin.  This is the -sid option trying to work around invisible
 * saveUnders menu, etc, windows.  Also -clip option.
 */

#define ADJUST_ROOTSHIFT \
	if (rootshift && subwin) { \
		d = rootwin; \
		x += off_x; \
		y += off_y; \
	} \
	if (clipshift) { \
		x += coff_x; \
		y += coff_y; \
	}

/*
 * Wrappers for Image related X calls
 */
Status XShmGetImage_wr(Display *disp, Drawable d, XImage *image, int x, int y,
    unsigned long mask) {

	ADJUST_ROOTSHIFT

	/* Note: the Solaris overlay stuff is all non-shm (using_shm = 0) */

#if HAVE_XSHM
#if HAVE_LIBXCOMPOSITE
	if(use_xcomposite && subwin && !rootshift) {
	  XErrorHandler old_handler = XSetErrorHandler(trap_xerror);
	  Pixmap pixmap = XCompositeNameWindowPixmap(disp, d);
	  Status s = XShmGetImage(disp, pixmap, image, x, y, mask);
	  XFreePixmap(dpy, pixmap);
	  XSetErrorHandler(old_handler);
	  if(!trapped_xerror)
	    return s;
	}
#endif
	  return XShmGetImage(disp, d, image, x, y, mask); 
#else
	if (!disp || !d || !image || !x || !y || !mask) {}
	return (Status) 0;
#endif
}

XImage *XShmCreateImage_wr(Display* disp, Visual* vis, unsigned int depth,
    int format, char* data, XShmSegmentInfo* shminfo, unsigned int width,
    unsigned int height) {

#if HAVE_XSHM
	return XShmCreateImage(disp, vis, depth, format, data, shminfo,
	    width, height); 
#else
	if (!disp || !vis || !depth || !format || !data || !shminfo || !width || !height) {}
	return (XImage *) 0;
#endif
}

Status XShmAttach_wr(Display *disp, XShmSegmentInfo *shminfo) {
#if HAVE_XSHM
	return XShmAttach(disp, shminfo);
#else
	if (!disp || !shminfo) {}
	return (Status) 0;
#endif
}

Status XShmDetach_wr(Display *disp, XShmSegmentInfo *shminfo) {
#if HAVE_XSHM
	if (getenv("X11VNC_SHM_DEBUG")) fprintf(stderr, "XShmDetach_wr: %p disp: %p\n", (void *)shminfo, (void *)disp);
	return XShmDetach(disp, shminfo);
#else
	if (!disp || !shminfo) {}
	return (Status) 0;
#endif
}

Bool XShmQueryExtension_wr(Display *disp) {
#if HAVE_XSHM
	return XShmQueryExtension(disp);
#else
	if (!disp) {}
	return False;
#endif
}

int XShmGetEventBase_wr(Display *disp) {
#if HAVE_XSHM
	return XShmGetEventBase(disp);
#else
	if (!disp) {}
	return 0;
#endif
}

/* wrapper for overlay screen reading: */

XImage *xreadscreen(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, Bool show_cursor) {
#if NO_X11
	if (!disp || !d || !x || !y || !width || !height || !show_cursor) {}
	return NULL;
#else

#ifdef SOLARIS_OVERLAY
	return XReadScreen(disp, d, x, y, width, height,
	    show_cursor);
#else
#  ifdef IRIX_OVERLAY
	{	unsigned long hints = 0, hints_ret;
		if (show_cursor) hints |= XRD_READ_POINTER;
		return XReadDisplay(disp, d, x, y, width, height,
		    hints, &hints_ret);
	}
#  else
	/* unused vars warning: */
	if (disp || d || x || y || width || height || show_cursor) {}

	return NULL;
#  endif
#endif

#endif	/* NO_X11 */
}

XImage *XGetSubImage_wr(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, unsigned long plane_mask,
    int format, XImage *dest_image, int dest_x, int dest_y) {
#if NO_X11
	nox11_exit(1);
	if (!disp || !d || !x || !y || !width || !height || !plane_mask || !format || !dest_image || !dest_x || !dest_y) {}
	return NULL;
#else
	ADJUST_ROOTSHIFT

	if (overlay && dest_x == 0 && dest_y == 0) {
		size_t size = (size_t)dest_image->height * dest_image->bytes_per_line;
		XImage *xi;

		xi = xreadscreen(disp, d, x, y, width, height,
		    (Bool) overlay_cursor);

		if (! xi) return NULL;

		/*
		 * There is extra overhead from memcpy and free...
		 * this is not like the real XGetSubImage().  We hope
		 * this significant overhead is still small compared to
		 * the time to retrieve the fb data.
		 */
		memcpy(dest_image->data, xi->data, size);

		XDestroyImage(xi);
		return (dest_image);
	}
#if HAVE_LIBXCOMPOSITE
	if(use_xcomposite && subwin && !rootshift) {
	  XErrorHandler old_handler = XSetErrorHandler(trap_xerror);
	  Pixmap pixmap = XCompositeNameWindowPixmap(disp, d);
	  XImage* xi =  XGetSubImage(disp, pixmap, x, y, width, height, plane_mask,
			     format, dest_image, dest_x, dest_y);
	  XFreePixmap(dpy, pixmap);
	  XSetErrorHandler(old_handler);
	  if(!trapped_xerror)
	    return xi;
	}
#endif
	return XGetSubImage(disp, d, x, y, width, height, plane_mask,
	    format, dest_image, dest_x, dest_y);
#endif	/* NO_X11 */
}

XImage *XGetImage_wr(Display *disp, Drawable d, int x, int y,
    unsigned int width, unsigned int height, unsigned long plane_mask,
    int format) {
#if NO_X11
	if (!disp || !d || !x || !y || !width || !height || !plane_mask || !format) {}
	nox11_exit(1);
	return NULL;
#else

	ADJUST_ROOTSHIFT

	if (overlay) {
		return xreadscreen(disp, d, x, y, width, height,
		    (Bool) overlay_cursor);
	}
#if HAVE_LIBXCOMPOSITE
	if(use_xcomposite && subwin  && !rootshift) {
	  XErrorHandler old_handler = XSetErrorHandler(trap_xerror);
	  Pixmap pixmap = XCompositeNameWindowPixmap(disp, d);
	  XImage* xi = XGetImage(disp, pixmap, x, y, width, height, plane_mask, format);
	  XFreePixmap(dpy, pixmap);
	  XSetErrorHandler(old_handler);
	  if(!trapped_xerror)
	    return xi;
	}
#endif
	return XGetImage(disp, d, x, y, width, height, plane_mask, format);
#endif	/* NO_X11 */
}

XImage *XCreateImage_wr(Display *disp, Visual *visual, unsigned int depth,
    int format, int offset, char *data, unsigned int width,
    unsigned int height, int bitmap_pad, int bytes_per_line) {
	/*
	 * This is a kludge to get a created XImage to exactly match what
	 * XReadScreen returns: we noticed the rgb masks are different
	 * from XCreateImage with the high color visual (red mask <->
	 * blue mask).  Note we read from the root window(!) then free
	 * the data.
	 */

	if (raw_fb) {	/* raw_fb hack */
		XImage *xi;
		xi = (XImage *) malloc(sizeof(XImage));
		memset(xi, 0, sizeof(XImage));
		xi->depth = depth;
		if (depth >= 24) {
			xi->bits_per_pixel = 32;
		} else if (depth > 16) {
			xi->bits_per_pixel = 24;
		} else if (depth > 8) {
			xi->bits_per_pixel = 16;
		} else {
			xi->bits_per_pixel = 8;
		}
		xi->format = format;
		xi->xoffset = offset;
		xi->data = data;
		xi->width = width;
		xi->height = height;
		xi->bitmap_pad = bitmap_pad;
		xi->bytes_per_line = bytes_per_line ? bytes_per_line : 
		    xi->width * xi->bits_per_pixel / 8;
		xi->bitmap_unit = -1;	/* hint to not call XDestroyImage */
		return xi;
	}

#if NO_X11
	nox11_exit(1);
	if (!disp || !visual || !depth || !format || !offset || !data || !width
	    || !height || !width || !bitmap_pad || !bytes_per_line) {}
	return NULL;
#else
	if (overlay) {
		XImage *xi;
		xi = xreadscreen(disp, window, 0, 0, width, height, False);
		if (xi == NULL) {
			return xi;
		}
		if (xi->data != NULL) {
			free(xi->data);
		}
		xi->data = data;
		return xi;
	}

	return XCreateImage(disp, visual, depth, format, offset, data,
	    width, height, bitmap_pad, bytes_per_line);
#endif	/* NO_X11 */
}

static void copy_raw_fb_low_bpp(XImage *dest, int x, int y, unsigned int w,
    unsigned int h) {
	char *src, *dst;
	unsigned int line;
	static char *buf = NULL;
	static int buflen = -1;
	int bpl = wdpy_x * raw_fb_native_bpp / 8;
	int n, ix, len, del, sz = wdpy_x * raw_fb_expand_bytes;

	unsigned int rm_n = raw_fb_native_red_mask;
	unsigned int gm_n = raw_fb_native_green_mask;
	unsigned int bm_n = raw_fb_native_blue_mask;
	unsigned int rm_f = main_red_mask;
	unsigned int gm_f = main_green_mask;
	unsigned int bm_f = main_blue_mask;

	unsigned int rs_n = raw_fb_native_red_shift;
	unsigned int gs_n = raw_fb_native_green_shift;
	unsigned int bs_n = raw_fb_native_blue_shift;
	unsigned int rs_f = main_red_shift;
	unsigned int gs_f = main_green_shift;
	unsigned int bs_f = main_blue_shift;

	unsigned int rx_n = raw_fb_native_red_max;
	unsigned int gx_n = raw_fb_native_green_max;
	unsigned int bx_n = raw_fb_native_blue_max;
	unsigned int rx_f = main_red_max;
	unsigned int gx_f = main_green_max;
	unsigned int bx_f = main_blue_max;

	static unsigned int msk[8];
	static int last_bpp = -1;
	static int cga = -1;

	if (rm_f | gm_f | bm_f) {}

	if (cga < 0) {
		if (getenv("RAWFB_CGA")) {
			cga = 1;
		} else {
			cga = 0;
		}
	}

	if (sz > buflen || buf == NULL) {
		if (buf) {
			free(buf);
		}
		buflen = sz + 1000;
		buf = (char *) malloc(buflen);
	}

	if (clipshift && ! use_snapfb) {
		x += coff_x;
		y += coff_y;
	}

	if (last_bpp != raw_fb_native_bpp) {
		int br;
		for (br = 0; br < 8; br++) {
			unsigned int pbit, k, m = 0;
			
			for (k=0; k < (unsigned int) raw_fb_native_bpp; k++) {
				pbit = 1 << (br+k);
				m |= pbit;
			}
			msk[br] = m;
		}
		last_bpp = raw_fb_native_bpp;
	}

	dst = dest->data;
if (0) fprintf(stderr, "x=%d y=%d w=%d h=%d bpl=%d d_bpl=%d-%dx%dx%d/%d %p\n",
    x, y, w, h, bpl, dest->bytes_per_line, dest->width, dest->height, dest->bits_per_pixel, dest->depth, dst);

	for (line = 0; line < h; line++) {

		if (! raw_fb_seek) {
			/* mmap */
			src = raw_fb_addr + raw_fb_offset + bpl*(y+line);

			memcpy(buf, src, bpl);
		} else {
			/* lseek */
			off_t off;
			off = (off_t) (raw_fb_offset + bpl*(y+line));

			lseek(raw_fb_fd, off, SEEK_SET);

			len = bpl;
			del = 0;
			while (len > 0) {
				n = read(raw_fb_fd, buf + del, len);

				if (n > 0) {
					del += n;
					len -= n;
				} else if (n == 0) {
					break;
				} else if (errno != EINTR && errno != EAGAIN) {
					break;
				}
			}
		}
		for (ix = 0; ix < (int) w; ix++) {
			int bx = (x + ix) * raw_fb_native_bpp;
			int ib = bx / 8;
			int br = bx - ib * 8;
			unsigned char val;

			val = *((unsigned char*) (buf + ib));

			val = msk[br] & val;
			val = val >> br;

			if (cga) {
				/* this is expt for CGA */
				double r, g, b;
				int ir, ig, ib;
				r = (2./3)*(val & 4) + (1./3)*(val & 8);
				g = (2./3)*(val & 2) + (1./3)*(val & 8);
				b = (2./3)*(val & 1) + (1./3)*(val & 8);
				if (val == 6) {
					g = g/2.;
				}
				ir = rx_f * r;
				ig = gx_f * g;
				ib = bx_f * b;
				val = (ib << bs_f) | (ig << gs_f) | (ir << rs_f);
			} else {
				unsigned char rval, gval, bval;
				
				rval = (val & rm_n) >> rs_n;
				gval = (val & gm_n) >> gs_n;
				bval = (val & bm_n) >> bs_n;

				rval = (rx_f * rval) / rx_n;
				gval = (gx_f * gval) / gx_n;
				bval = (bx_f * bval) / bx_n;

				rval = rval << rs_f;
				gval = gval << gs_f;
				bval = bval << bs_f;

				val = rval | gval | bval;
			}

			*(dst+ix) = (char) val;
		}

		dst += dest->bytes_per_line;
	}
}

static void copy_raw_fb_24_to_32(XImage *dest, int x, int y, unsigned int w,
    unsigned int h) {
	/*
	 * kludge to read 1 byte at a time and dynamically transform
	 * 24bpp -> 32bpp by inserting a extra 0 byte into dst.
	 */
	char *src, *dst;
	unsigned int line;
	static char *buf = NULL;
	static int buflen = -1;
	int bpl = wdpy_x * 3;	/* pixelsize == 3 */
	int LE, n, stp, len, del, sz = w * 3;
	int insert_zeroes = 1;

#define INSERT_ZEROES  \
	len = sz; \
	del = 0; \
	stp = 0; \
	while (len > 0) { \
		if (insert_zeroes && (del - LE) % 4 == 0) { \
			*(dst + del) = 0; \
			del++; \
		} \
		*(dst + del) = *(buf + stp); \
		del++; \
		len--; \
		stp++; \
	}

	if (rfbEndianTest) {
		LE = 3;	/* little endian */
	} else {
		LE = 0; /* big endian */
	}

	if (sz > buflen || buf == NULL) {
		if (buf) {
			free(buf);
		}
		buf = (char *) malloc((size_t)4*(sz + 1000));
	}

	if (clipshift && ! use_snapfb) {
		x += coff_x;
		y += coff_y;
	}

	if (use_snapfb && dest != snap) {
		/* snapfb src */
		src = snap->data + snap->bytes_per_line*y + 3*x;
		dst = dest->data;
		for (line = 0; line < h; line++) {
			memcpy(buf, src, sz);

			INSERT_ZEROES

			src += snap->bytes_per_line;
			dst += dest->bytes_per_line;
		}

	} else if (! raw_fb_seek) {
		/* mmap */
		bpl = raw_fb_bytes_per_line;
		if (clipshift && wdpy_x != cdpy_x) {
			bpl = wdpy_x * 3;
		}
		src = raw_fb_addr + raw_fb_offset + bpl*y + 3*x;
		dst = dest->data;

		if (use_snapfb && dest == snap) {
			/*
			 * writing *to* snap_fb: need the x,y offset,
			 * and also do not do inserts.
			 */
			dst += bpl*y + 3*x;
			insert_zeroes = 0;
		}

		for (line = 0; line < h; line++) {
			memcpy(buf, src, sz);

			INSERT_ZEROES

			src += bpl;
			dst += dest->bytes_per_line;
		}

	} else {
		/* lseek */
		off_t off;
		bpl = raw_fb_bytes_per_line;
		if (clipshift && wdpy_x != cdpy_x) {
			bpl = wdpy_x * 3;
		}
		off = (off_t) (raw_fb_offset + bpl*y + 3*x);

		lseek(raw_fb_fd, off, SEEK_SET);
		dst = dest->data;

		if (use_snapfb && dest == snap) {
			/*
			 * writing *to* snap_fb: need the x,y offset,
			 * and also do not do inserts.
			 */
			dst += bpl*y + 3*x;
			insert_zeroes = 0;
		}

		for (line = 0; line < h; line++) {
			len = sz;
			del = 0;
			while (len > 0) {
				n = read(raw_fb_fd, buf + del, len);

				if (n > 0) {
					del += n;
					len -= n;
				} else if (n == 0) {
					break;
				} else if (errno != EINTR && errno != EAGAIN) {
					break;
				}
			}

			INSERT_ZEROES

			if (bpl > sz) {
				off = (off_t) (bpl - sz);
				lseek(raw_fb_fd, off, SEEK_CUR);
			}
			dst += dest->bytes_per_line;
		}
	}
}

#ifdef MACOSX
void macosx_copy_opengl(char *, int, int, unsigned int, unsigned int);
#endif

void copy_raw_fb(XImage *dest, int x, int y, unsigned int w, unsigned int h) {
	char *src, *dst;
	unsigned int line;
	int pixelsize = bpp/8;
	static int db = -1;

#ifdef MACOSX
	if (macosx_console && macosx_read_opengl) {
		macosx_copy_opengl(dest->data, x, y, w, h);
		return;
	}
#endif

	if (xform24to32) {
		copy_raw_fb_24_to_32(dest, x, y, w, h);
		return;
	}
	if (raw_fb_native_bpp < 8) {
		copy_raw_fb_low_bpp(dest, x, y, w, h);
		return;
	}
	if (db < 0) {
		if (getenv("DEBUG_COPY_RAW_FB")) {
			db = atoi(getenv("DEBUG_COPY_RAW_FB"));
		} else {
			db = 0;
		}
	}

	if (clipshift && ! use_snapfb) {
		x += coff_x;
		y += coff_y;
	}


	if (use_snapfb && dest != snap) {
		/* snapfb src */
		src = snap->data + snap->bytes_per_line*y + pixelsize*x;
		dst = dest->data;

if (db) fprintf(stderr, "snap->bytes_per_line: %d, dest->bytes_per_line: %d, w: %d h: %d dpy_x: %d wdpy_x: %d cdpy_x: %d bpp: %d pixelsize: %d\n", snap->bytes_per_line, dest->bytes_per_line, w, h, dpy_x, wdpy_x, cdpy_x, bpp, pixelsize);

		for (line = 0; line < h; line++) {
			memcpy(dst, src, (size_t)w * pixelsize);
			src += snap->bytes_per_line;
			dst += dest->bytes_per_line;
		}

	} else if (! raw_fb_seek) {
		/* mmap */
		int bpl = raw_fb_bytes_per_line;

		if (clipshift && wdpy_x != cdpy_x) {
			bpl = wdpy_x * pixelsize;
		}

		src = raw_fb_addr + raw_fb_offset + bpl*y + pixelsize*x;
		dst = dest->data;

if (db) fprintf(stderr, "bpl: %d, dest->bytes_per_line: %d, w: %d h: %d dpy_x: %d wdpy_x: %d cdpy_x: %d bpp: %d pixelsize: %d\n", bpl, dest->bytes_per_line, w, h, dpy_x, wdpy_x, cdpy_x, bpp, pixelsize);

		for (line = 0; line < h; line++) {
			memcpy(dst, src, (size_t)w * pixelsize);
			src += bpl;
			dst += dest->bytes_per_line;
		}

	} else {
		/* lseek */
		int n, len, del, sz = w * pixelsize;
		off_t off;
		int bpl = raw_fb_bytes_per_line;

		if (clipshift && wdpy_x != cdpy_x) {
			bpl = wdpy_x * pixelsize;
		}

		off = (off_t) (raw_fb_offset + bpl*y + pixelsize*x);

		lseek(raw_fb_fd, off, SEEK_SET);
		dst = dest->data;

if (db) fprintf(stderr, "lseek 0 ps: %d  sz: %d off: %d bpl: %d\n", pixelsize, sz, (int) off, bpl);

		for (line = 0; line < h; line++) {
			len = sz;
			del = 0;
			while (len > 0) {
				n = read(raw_fb_fd, dst + del, len);

				if (n > 0) {
					del += n;
					len -= n;
				} else if (n == 0) {
					break;
				} else if (errno != EINTR && errno != EAGAIN) {
					break;
				}
			}
			if (bpl > sz) {
				off = (off_t) (bpl - sz);
				lseek(raw_fb_fd, off, SEEK_CUR);
			}
			dst += dest->bytes_per_line;
		}
	}
}

void copy_image(XImage *dest, int x, int y, unsigned int w, unsigned int h) {
	/* default (w=0, h=0) is the fill the entire XImage */
	if (dest == NULL) {
		return;
	}
	if (w < 1)  {
		w = dest->width;
	}
	if (h < 1)  {
		h = dest->height;
	}

#if HAVE_NVFBC
	/* Try NVFBC capture first if enabled */
	if (use_nvfbc && nvfbc_capture_active) {
		if (nvfbc_copy_to_ximage(dest, x, y, w, h)) {
			return;  /* Success */
		}
		/* Fall through to other methods on failure */
	}
#endif

	if (raw_fb) {
		copy_raw_fb(dest, x, y, w, h);

	} else if (use_snapfb && snap_fb && dest != snaprect) {
		char *src, *dst;
		unsigned int line;
		int pixelsize = bpp/8;

		src = snap->data + snap->bytes_per_line*y + pixelsize*x;
		dst = dest->data;
		for (line = 0; line < h; line++) {
			memcpy(dst, src, (size_t)w * pixelsize);
			src += snap->bytes_per_line;
			dst += dest->bytes_per_line;
		}

	} else if ((using_shm && ! xform24to32) && (int) w == dest->width &&
	    (int) h == dest->height) {
		XShmGetImage_wr(dpy, window, dest, x, y, AllPlanes);

	} else {
		XGetSubImage_wr(dpy, window, x, y, w, h, AllPlanes,
		    ZPixmap, dest, 0, 0);
	}
}

#define DEBUG_SKIPPED_INPUT(dbg, str) \
	if (dbg) { \
		rfbLog("skipped input: %s\n", str); \
	}

void init_track_keycode_state(void) {
	int i;
	for (i=0; i<256; i++) {
		keycode_state[i] = 0;
	}
	get_keystate(keycode_state);
}

static void upup_downdown_warning(KeyCode key, Bool down) {
	RAWFB_RET_VOID
#if NO_X11
	if (!key || !down) {}
	return;
#else
	if ((down ? 1:0) == keycode_state[(int) key]) {
		char *str = XKeysymToString(XKeycodeToKeysym_wr(dpy, key, 0));
		rfbLog("XTestFakeKeyEvent: keycode=0x%x \"%s\" is *already* "
		    "%s\n", key, str ? str : "null", down ? "down":"up");
	}
#endif	/* NO_X11 */
}

/*
 * wrappers for XTestFakeKeyEvent, etc..
 * also for XTrap equivalents XESimulateXEventRequest
 */

void XTRAP_FakeKeyEvent_wr(Display* dpy, KeyCode key, Bool down,
    unsigned long delay) {

	RAWFB_RET_VOID
#if NO_X11
	nox11_exit(1);
	if (!dpy || !key || !down || !delay) {}
	return;
#else

	if (! xtrap_present) {
		DEBUG_SKIPPED_INPUT(debug_keyboard, "keyboard: no-XTRAP");
		return;
	}
	/* unused vars warning: */
	if (key || down || delay) {}

# if HAVE_LIBXTRAP
	XESimulateXEventRequest(trap_ctx, down ? KeyPress : KeyRelease,
	    key, 0, 0, 0);
	if (debug_keyboard) {
		upup_downdown_warning(key, down);
	}
	keycode_state[(int) key] = down ? 1 : 0;
# else
	DEBUG_SKIPPED_INPUT(debug_keyboard, "keyboard: no-XTRAP-build");
# endif

#endif	/* NO_X11 */
}

void XTestFakeKeyEvent_wr(Display* dpy, int dev_id, KeyCode key, Bool down,
    unsigned long delay) {
	static int first = 1;
	int regrab = 0;

	RAWFB_RET_VOID

#if NO_X11
	nox11_exit(1);
	if (!dpy || !key || !down || !delay || !first) {}
	return;
#else
	if (debug_keyboard) {
		char *str = XKeysymToString(XKeycodeToKeysym_wr(dpy, key, 0));
		rfbLog("XTestFakeKeyEvent(dpy, keycode=0x%x \"%s\", %s)\n",
		    key, str ? str : "null", down ? "down":"up");
	}
	if (first) { 
		init_track_keycode_state();
		first = 0;
	}
	if (down) {
		last_keyboard_keycode = -key;
	} else {
		last_keyboard_keycode = key;
	}

	if (grab_kbd) {
		XUngrabKeyboard(dpy, CurrentTime);
		regrab = 1;
	}
	if (grab_ptr && ungrab_both) {
		XUngrabPointer(dpy, CurrentTime);
		regrab = 1;
	}

	if (xtrap_input) {
		XTRAP_FakeKeyEvent_wr(dpy, key, down, delay);
		if (regrab) {
			adjust_grabs(1, 1);
		}
		return;
	}

	if (! xtest_present) {
		DEBUG_SKIPPED_INPUT(debug_keyboard, "keyboard: no-XTEST");
		return;
	}
	if (debug_keyboard) {
		rfbLog("calling XTestFakeKeyEvent(%d, %d)  %.4f\n",
		    key, down, dnowx());	
	}
#if HAVE_XTEST
#ifdef HAVE_XI2
    if(use_multipointer && dev_id >= 0)
      {
        XErrorHandler old_handler;
	XDevice xdev;
        xdev.device_id = dev_id;

        /* there can be a race condition where this is called when the XI2 device has not yet been created */
        old_handler = XSetErrorHandler(trap_xerror);
        trapped_xerror = 0;

	XTestFakeDeviceKeyEvent(dpy, &xdev, key, down, NULL, 0, delay);        

        XSetErrorHandler(old_handler);
        if (trapped_xerror) 
          trapped_xerror = 0;
      }
    else
#endif
        XTestFakeKeyEvent(dpy, key, down, delay);

	if (regrab) {
		adjust_grabs(1, 1);
	}
	if (debug_keyboard) {
		upup_downdown_warning(key, down);
	}
	keycode_state[(int) key] = down ? 1 : 0;
#endif

#endif	/* NO_X11 */
}

void XTRAP_FakeButtonEvent_wr(Display* dpy, unsigned int button, Bool is_press,
    unsigned long delay) {

	RAWFB_RET_VOID
#if NO_X11
	nox11_exit(1);
	if (!dpy || !button || !is_press || !delay) {}
	return;
#else

	if (! xtrap_present) {
		DEBUG_SKIPPED_INPUT(debug_keyboard, "button: no-XTRAP");
		return;
	}
	/* unused vars warning: */
	if (button || is_press || delay) {}

#if HAVE_LIBXTRAP
	XESimulateXEventRequest(trap_ctx,
	    is_press ? ButtonPress : ButtonRelease, button, 0, 0, 0);
#else
	DEBUG_SKIPPED_INPUT(debug_keyboard, "button: no-XTRAP-build");
#endif

#endif	/* NO_X11 */
}

void XTestFakeButtonEvent_wr(Display* dpy, int dev_id, unsigned int button, Bool is_press,
    unsigned long delay) {
	int regrab = 0;

	RAWFB_RET_VOID
#if NO_X11
	nox11_exit(1);
	if (!dpy || !button || !is_press || !delay) {}
	return;
#else

	if (grab_ptr) {
		XUngrabPointer(dpy, CurrentTime);
		regrab = 1;
	}
	if (grab_kbd && ungrab_both) {
		XUngrabKeyboard(dpy, CurrentTime);
		regrab = 1;
	}

	if (xtrap_input) {
		XTRAP_FakeButtonEvent_wr(dpy, button, is_press, delay);
		if (regrab) {
			adjust_grabs(1, 1);
		}
		return;
	}

	if (! xtest_present) {
		DEBUG_SKIPPED_INPUT(debug_keyboard, "button: no-XTEST");
		return;
	}
	if (debug_pointer) {
		rfbLog("calling XTestFakeButtonEvent(%d, %d)  %.4f\n",
		    button, is_press, dnowx());	
	}
#if HAVE_XTEST
#ifdef HAVE_XI2
    if(use_multipointer && dev_id >= 0)
      {
        XErrorHandler old_handler;
        XDevice xdev;
	xdev.device_id = dev_id;

        /* there can be a race condition where this is called when the XI2 device has not yet been created */
        old_handler = XSetErrorHandler(trap_xerror);
        trapped_xerror = 0;

        XTestFakeDeviceButtonEvent(dpy, &xdev, button, is_press, NULL, 0, delay);	  

        XSetErrorHandler(old_handler);
        if (trapped_xerror) 
          trapped_xerror = 0;
      }
    else
#endif
        XTestFakeButtonEvent(dpy, button, is_press, delay);
#endif
	if (regrab) {
		adjust_grabs(1, 1);
	}
#endif	/* NO_X11 */
}


void XTRAP_FakeMotionEvent_wr(Display* dpy, int screen, int x, int y,
    unsigned long delay) {

	RAWFB_RET_VOID

#if NO_X11
	nox11_exit(1);
	if (!dpy || !screen || !x || !y || !delay) {}
	return;
#else
	if (! xtrap_present) {
		DEBUG_SKIPPED_INPUT(debug_keyboard, "motion: no-XTRAP");
		return;
	}
	/* unused vars warning: */
	if (dpy || screen || x || y || delay) {}

#if HAVE_LIBXTRAP
	XESimulateXEventRequest(trap_ctx, MotionNotify, 0, x, y, 0);
#else
	DEBUG_SKIPPED_INPUT(debug_keyboard, "motion: no-XTRAP-build");
#endif

#endif	/* NO_X11 */
}

void XTestFakeMotionEvent_wr(Display* dpy, int dev_id, int screen, int x, int y,
    unsigned long delay) {
	int regrab = 0;

	RAWFB_RET_VOID
#if NO_X11
	nox11_exit(1);
	if (!dpy || !screen || !x || !y || !delay) {}
	return;
#else

	if (grab_ptr) {
		XUngrabPointer(dpy, CurrentTime);
		regrab = 1;
	}
	if (grab_kbd && ungrab_both) {
		XUngrabKeyboard(dpy, CurrentTime);
		regrab = 1;
	}

	if (xtrap_input) {
		XTRAP_FakeMotionEvent_wr(dpy, screen, x, y, delay);
		if (regrab) {
			adjust_grabs(1, 1);
		}
		return;
	}

	if (debug_pointer) {
		rfbLog("calling XTestFakeMotionEvent(%d, %d)  %.4f\n",
		    x, y, dnowx());	
	}
#if HAVE_XTEST
#ifdef HAVE_XI2
    if(use_multipointer && dev_id >= 0)
      {
        XErrorHandler old_handler;
        int axes[] = {x, y};
	XDevice xdev;
	xdev.device_id = dev_id;

        /* there can be a race condition where this is called when the XI2 device has not yet been created */
        old_handler = XSetErrorHandler(trap_xerror);
        trapped_xerror = 0;

        XTestFakeDeviceMotionEvent(dpy, &xdev, 0, 0, axes, 2, delay);

        XSetErrorHandler(old_handler);
        if (trapped_xerror) 
          trapped_xerror = 0;
      }
    else
#endif
          XTestFakeMotionEvent(dpy, screen, x, y, delay);
#endif
	if (regrab) {
		adjust_grabs(1, 1);
	}
#endif	/* NO_X11 */
}

Bool XTestCompareCurrentCursorWithWindow_wr(Display* dpy, Window w) {
	if (! xtest_present) {
		return False;
	}
	RAWFB_RET(False)

#if HAVE_XTEST
	return XTestCompareCurrentCursorWithWindow(dpy, w);
#else
	if (!w) {}
	return False;
#endif
}

Bool XTestCompareCursorWithWindow_wr(Display* dpy, Window w, Cursor cursor) {
	if (! xtest_present) {
		return False;
	}
	RAWFB_RET(False)
#if HAVE_XTEST
	return XTestCompareCursorWithWindow(dpy, w, cursor);
#else
	if (!dpy || !w || !cursor) {}
	return False;
#endif
}

Bool XTestQueryExtension_wr(Display *dpy, int *ev, int *er, int *maj,
    int *min) {
	RAWFB_RET(False)
#if HAVE_XTEST
	return XTestQueryExtension(dpy, ev, er, maj, min);
#else
	if (!dpy || !ev || !er || !maj || !min) {}
	return False;
#endif
}

void XTestDiscard_wr(Display *dpy) {
	if (! xtest_present) {
		return;
	}
	RAWFB_RET_VOID
#if HAVE_XTEST
	XTestDiscard(dpy);
#else
	if (!dpy) {}
#endif
}

Bool XETrapQueryExtension_wr(Display *dpy, int *ev, int *er, int *op) {
	RAWFB_RET(False)
#if HAVE_LIBXTRAP
	return XETrapQueryExtension(dpy, (INT32 *)ev, (INT32 *)er,
	    (INT32 *)op);
#else
	/* unused vars warning: */
	if (ev || er || op) {}
	return False;
#endif
}

int XTestGrabControl_wr(Display *dpy, Bool impervious) {
	if (! xtest_present) {
		return 0;
	}
	RAWFB_RET(0)
#if HAVE_XTEST && HAVE_XTESTGRABCONTROL
	XTestGrabControl(dpy, impervious);
	return 1;
#else
	if (!dpy || !impervious) {}
	return 0;
#endif
}

int XTRAP_GrabControl_wr(Display *dpy, Bool impervious) {
	if (! xtrap_present) {
		/* unused vars warning: */
		if (dpy || impervious) {}
		return 0;
	}
	RAWFB_RET(0)
#if HAVE_LIBXTRAP
	  else {
		ReqFlags requests;

		if (! impervious) {
			if (trap_ctx) {
				XEFreeTC(trap_ctx);
			}
			trap_ctx = NULL;
			return 1;
		}

		if (! trap_ctx) {
			trap_ctx = XECreateTC(dpy, 0, NULL);
			if (! trap_ctx) {
				rfbLog("DEC-XTRAP XECreateTC failed.  Watch "
				    "out for XGrabServer from wm's\n");
				return 0;
			}
			XEStartTrapRequest(trap_ctx);
			memset(requests, 0, sizeof(requests));
			BitTrue(requests, X_GrabServer);
			BitTrue(requests, X_UngrabServer);
			XETrapSetRequests(trap_ctx, True, requests);
			XETrapSetGrabServer(trap_ctx, True);
		}
		return 1;
	}
#endif
	return 0;
}

void disable_grabserver(Display *in_dpy, int change) {
	int ok = 0;
	static int didmsg = 0;

	if (debug_grabs) {
		fprintf(stderr, "disable_grabserver/%d %.5f\n",
			xserver_grabbed, dnowx());
		didmsg = 0;
	}

	if (! xtrap_input) {
		if (XTestGrabControl_wr(in_dpy, True)) {
			if (change) {
				XTRAP_GrabControl_wr(in_dpy, False);
			}
			if (! didmsg && ! raw_fb_str) {
				rfbLog("GrabServer control via XTEST.\n"); 
				didmsg = 1;
			}
			ok = 1;
		} else {
			if (XTRAP_GrabControl_wr(in_dpy, True)) {
				ok = 1;
				if (! didmsg && ! raw_fb_str) {
					rfbLog("Using DEC-XTRAP for protection"
					    " from XGrabServer.\n");
					didmsg = 1;
				}
			}
		}
	} else {
		if (XTRAP_GrabControl_wr(in_dpy, True)) {
			if (change) {
				XTestGrabControl_wr(in_dpy, False);
			}
			if (! didmsg && ! raw_fb_str) {
				rfbLog("GrabServer control via DEC-XTRAP.\n"); 
				didmsg = 1;
			}
			ok = 1;
		} else {
			if (XTestGrabControl_wr(in_dpy, True)) {
				ok = 1;
				if (! didmsg && ! raw_fb_str) {
					rfbLog("DEC-XTRAP XGrabServer "
					    "protection not available, "
					    "using XTEST.\n");
					didmsg = 1;
				}
			}
		}
	}
	if (! ok && ! didmsg) {
		rfbLog("*********************************************************\n");
		rfbLog("* No XTEST or DEC-XTRAP protection from XGrabServer !!! *\n");
		rfbLog("* DEADLOCK if your window manager calls XGrabServer !!! *\n");
		rfbLog("*********************************************************\n");
	}
	XFlush_wr(in_dpy);
}

Bool XRecordQueryVersion_wr(Display *dpy, int *maj, int *min) {
	RAWFB_RET(False)
#if HAVE_RECORD
	return XRecordQueryVersion(dpy, maj, min);
#else
	if (!dpy || !maj || !min) {}
	return False;
#endif
}


Bool XInputQueryVersion_wr(Display *dpy, int *maj, int *min) {
	RAWFB_RET(False)
#if NO_X11
	rfbLog("This x11vnc was built without X11 support (-rawfb only).\n");
	if (!dpy || !maj || !min) {}
	return NULL;
#else
	int ignore;
	if(! XQueryExtension (dpy, "XInputExtension", &ignore, &ignore, &ignore))
	  return False;
#ifdef HAVE_XI2
	if (XIQueryVersion(dpy, maj, min) != Success)
	  return False;
#endif

	return True;
#endif	/* NO_X11 */
}


int xauth_raw(int on) {
	char tmp[] = "/tmp/x11vnc-xauth.XXXXXX";
	int tmp_fd = -1;
	static char *old_xauthority = NULL;
	static char *old_tmp = NULL;
	int db = 0;

	if (on) {
		if (old_xauthority) {
			free(old_xauthority);
			old_xauthority = NULL;
		}
		if (old_tmp) {
			free(old_tmp);
			old_tmp = NULL;
		}
		if (xauth_raw_data) {
			tmp_fd = mkstemp(tmp);
			if (tmp_fd < 0) {
				rfbLog("could not create tmp xauth file: %s\n", tmp);	
				return 0;
			}
			if (db) fprintf(stderr, "XAUTHORITY tmp: %s\n", tmp);
			write(tmp_fd, xauth_raw_data, xauth_raw_len);
			close(tmp_fd);
			if (getenv("XAUTHORITY")) {
				old_xauthority = strdup(getenv("XAUTHORITY"));
			} else {
				old_xauthority = strdup("");
			}
			set_env("XAUTHORITY", tmp);
			old_tmp = strdup(tmp);
		}
		return 1;
	} else {
		if (old_xauthority) {
			if (!strcmp(old_xauthority, "")) {
				char *xauth = getenv("XAUTHORITY");
				if (xauth) {
					*(xauth-2) = '_';	/* yow */
				}
			} else {
				set_env("XAUTHORITY", old_xauthority);
			}
			free(old_xauthority);
			old_xauthority = NULL;
		}
		if (old_tmp) {
			unlink(old_tmp);
			free(old_tmp);
			old_tmp = NULL;
		}
		return 1;
	}
}

Display *XOpenDisplay_wr(char *display_name) {
	Display *d = NULL;
	int db = 0;

	if (! xauth_raw(1)) {
		return NULL;
	}
#if NO_X11
	rfbLog("This x11vnc was built without X11 support (-rawfb only).\n");
	if (!display_name || !d || !db) {}
	return NULL;
#else

	d = XOpenDisplay(display_name);
	if (db) fprintf(stderr, "XOpenDisplay_wr: %s  %p\n", display_name, (void *)d);

	if (d == NULL && !getenv("NO_XAUTHLOCALHOSTNAME")) {
		char *xalhn = getenv("XAUTHLOCALHOSTNAME");
		if (1 || !xalhn) {
			rfbLog("XOpenDisplay(\"%s\") failed.\n",
			    display_name ? display_name : "");
			rfbLog("Trying again with XAUTHLOCALHOSTNAME=localhost ...\n");
			set_env("XAUTHLOCALHOSTNAME", "localhost");
			d = XOpenDisplay(display_name);
			if (d == NULL && xalhn) {
				char *ptr = getenv("XAUTHLOCALHOSTNAME");
				if (ptr) {
					*(ptr-2) = '_';	/* yow */
					rfbLog("XOpenDisplay(\"%s\") failed.\n",
					    display_name ? display_name : "");
					rfbLog("Trying again with unset XAUTHLOCALHOSTNAME ...\n");
					d = XOpenDisplay(display_name);
				}
			}
		}
	}

	xauth_raw(0);

	return d;
#endif	/* NO_X11 */
}

int XCloseDisplay_wr(Display *display) {
	int db = 0;
	if (db) fprintf(stderr, "XCloseDisplay_wr: %p\n", (void *)display);
#if NO_X11
	return 0;
#else
	return XCloseDisplay(display);
#endif	/* NO_X11 */
}

static unsigned int Bmask = (Button1Mask|Button2Mask|Button3Mask|Button4Mask|Button5Mask);
static unsigned int Mmask = (ShiftMask|LockMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask);

static unsigned int last_local_button_mask = 0;
static unsigned int last_local_mod_mask = 0;
static int last_local_x = 0;
static int last_local_y = 0;

Bool XQueryPointer_wr(Display *display, Window w, Window *root_return,
    Window *child_return, int *root_x_return, int *root_y_return,
    int *win_x_return, int *win_y_return, unsigned int *mask_return) {
#if NO_X11
	if (!display || !w || !root_return || !child_return || !root_x_return
	    || !root_y_return || !win_x_return || !win_y_return || !mask_return) {}
	return False;
#else
	Bool rc;
	XErrorHandler old_handler;


	if (! display) {
		return False;
	}
	old_handler = XSetErrorHandler(trap_xerror);
	trapped_xerror = 0;

	rc = XQueryPointer(display, w, root_return, child_return,
	    root_x_return, root_y_return, win_x_return, win_y_return,
	    mask_return);

	XSetErrorHandler(old_handler);
	if (trapped_xerror) {
		rc = 0;
	}
	if (rc) {
		display_button_mask = (*mask_return) & Bmask;
		display_mod_mask    = (*mask_return) & Mmask;
		if (last_local_button_mask != display_button_mask) {
			got_local_pointer_input++;
		} else if (*root_x_return != last_local_x ||
		    *root_y_return != last_local_y) {
			got_local_pointer_input++;
		}
		last_local_button_mask = display_button_mask;
		last_local_mod_mask = display_mod_mask;
		last_local_x = *root_x_return;
		last_local_y = *root_y_return;
	}
	return rc;
#endif	/* NO_X11 */
}

#if HAVE_XI2
Bool XIQueryPointer_wr( Display *display,
                        int deviceid,
                        Window win,
                        Window *root_return,
                        Window *child_return,
                        double *root_x_return,
                        double *root_y_return,
                        double *win_x_return,
                        double *win_y_return,
                        XIButtonState *buttons_return,
                        XIModifierState *modifiers_return,
                        XIGroupState *group_return)
{
  Bool rc;
  XErrorHandler old_handler;

  if (! display) {
    return False;
  }

  /* there can be a race condition where this is called when the XI2 device has not yet been created */
  old_handler = XSetErrorHandler(trap_xerror);
  trapped_xerror = 0;

  rc = XIQueryPointer(display, deviceid, win, root_return, child_return,
                      root_x_return, root_y_return, win_x_return, win_y_return,
                      buttons_return, modifiers_return, group_return);

  XSetErrorHandler(old_handler);
  if (trapped_xerror) {
    trapped_xerror = 0;
    rc = 0;
  }

  return rc;
}


Bool XIWarpPointer_wr( Display *display,
                       int deviceid,
                       Window src_w,
                       Window dest_w,
                       double src_x,
                       double src_y,
                       int src_width,
                       int src_height,
                       double dest_x,
                       double dest_y)
{
  Bool rc;
  XErrorHandler old_handler;

  if (! display) {
    return False;
  }

  /* there can be a race condition where this is called when the XI2 device has not yet been created */
  old_handler = XSetErrorHandler(trap_xerror);
  trapped_xerror = 0;

  rc = XIWarpPointer(display, deviceid, src_w, dest_w, src_x, src_y, src_width, src_height, dest_x, dest_y);

  XSetErrorHandler(old_handler);
  if (trapped_xerror) {
    trapped_xerror = 0;
    rc = 0;
  }

  return rc;
}
#endif /* HAVE_XI2 */


Status XQueryTree_wr(Display *display, Window w, Window *root_return,
    Window *parent_return, Window **children_return,
    unsigned int *nchildren_return) {

#ifdef MACOSX
	if (macosx_console) {
		return macosx_xquerytree(w, root_return, parent_return,
		    children_return, nchildren_return);
	}
#endif
#if NO_X11
	if (!display || !w || !root_return || !parent_return
	    || !children_return || !nchildren_return) {}
	return (Status) 0;
#else
	if (! display) {
		return (Status) 0;
	}
	return XQueryTree(display, w, root_return, parent_return,
	    children_return, nchildren_return);
#endif	/* NO_X11 */
    	
}

int XFree_wr(void *data) {
	if (data == NULL) {
		return 1;
	}
	if (! dpy) {
		return 1;
	}
#if NO_X11
	return 1;
#else
	return XFree(data);
#endif
}

int XSelectInput_wr(Display *display, Window w, long event_mask) {
#if NO_X11
	if (!display || !w || !event_mask) {}
	return 0;
#else
	int rc;
	XErrorHandler old_handler;
	if (display == NULL || w == None) {
		return 0;
	}
	old_handler = XSetErrorHandler(trap_xerror);
	trapped_xerror = 0;
	rc = XSelectInput(display, w, event_mask);
	XSetErrorHandler(old_handler);
	if (trapped_xerror) {
		rc = 0;
	}
	return rc;
#endif
}

KeySym XKeycodeToKeysym_wr(Display *display, KeyCode keycode, int index) {
#if NO_X11
	return 0;
#else
#if !HAVE_XKEYBOARD || SKIP_XKB
	return XKeycodeToKeysym(display, keycode, index);
#else
	return XkbKeycodeToKeysym(display, keycode, 0, index);
#endif
#endif
}

void nox11_exit(int rc) {
#if NO_X11
	rfbLog("This x11vnc was not built with X11 support.\n");
	clean_up_exit(rc);
#else
	if (0) {rc = 0;}
#endif
}


#if NO_X11
#include "nox11_funcs.h"
#endif

