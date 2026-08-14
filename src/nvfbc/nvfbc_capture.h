/*
 * nvfbc_capture.h - NVIDIA Frame Buffer Capture (NVFBC) interface for x11vnc
 *
 * This module provides high-performance screen capture using NVIDIA's NVFBC API.
 * It includes support for consumer GeForce GPUs using Sunshine's patch-free method.
 *
 * Copyright (C) 2026 x11vnc-nvfbc contributors
 * License: GPL-2.0+
 */

#ifndef NVFBC_CAPTURE_H
#define NVFBC_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVFBC capture status codes */
typedef enum {
    NVFBC_CAP_OK = 0,           /* Success */
    NVFBC_CAP_ERR_INIT,         /* Failed to initialize NVFBC */
    NVFBC_CAP_ERR_NO_LIBRARY,   /* libnvidia-fbc.so not found */
    NVFBC_CAP_ERR_NO_DISPLAY,   /* No display available */
    NVFBC_CAP_ERR_SESSION,      /* Failed to create capture session */
    NVFBC_CAP_ERR_SETUP,        /* Failed to setup capture */
    NVFBC_CAP_ERR_GRAB,         /* Failed to grab frame */
    NVFBC_CAP_ERR_UNSUPPORTED,  /* NVFBC not supported (wrong GPU/driver) */
    NVFBC_CAP_ERR_RECREATE,     /* Session needs to be recreated (modeset) */
} nvfbc_cap_status_t;

/* Frame information returned after capture */
typedef struct {
    uint32_t width;             /* Frame width in pixels */
    uint32_t height;            /* Frame height in pixels */
    uint32_t byte_size;         /* Total frame size in bytes */
    uint32_t frame_id;          /* Incremental frame counter */
    int is_new_frame;           /* 1 if this is a new frame, 0 if duplicate */
    int is_direct_capture;      /* 1 if NVFBC bypassed X and took it straight
                                 * from a fullscreen app (see
                                 * allow_direct_capture) */
    uint64_t timestamp_us;      /* Capture timestamp in microseconds */
} nvfbc_frame_info_t;

/* Display/output information */
typedef struct {
    uint32_t id;                /* Output ID */
    char name[128];             /* Output name (e.g., "DP-0") */
    uint32_t x;                 /* X offset */
    uint32_t y;                 /* Y offset */
    uint32_t width;             /* Width */
    uint32_t height;            /* Height */
} nvfbc_output_info_t;

/* Screen status information */
typedef struct {
    int is_capture_possible;    /* 1 if capture is possible */
    uint32_t screen_width;      /* Total screen width */
    uint32_t screen_height;     /* Total screen height */
    int xrandr_available;       /* 1 if XRandR is available */
    uint32_t num_outputs;       /* Number of connected outputs */
    nvfbc_output_info_t outputs[5];  /* Output information (max 5) */
} nvfbc_status_t;

/* Capture configuration */
typedef struct {
    int with_cursor;            /* 1 to capture mouse cursor */
    int with_diff_map;          /* 1 to generate differential map */
    uint32_t diff_map_scale;    /* Diff map scaling factor (default: 16) */
    int track_output;           /* Output ID to track, or -1 for entire screen */
    uint32_t sampling_rate_ms;  /* Capture rate in ms (default: 16 = ~60Hz) */
    int push_model;             /* 1 for push model (capture on damage) */
    int allow_direct_capture;   /* 1 to let NVFBC bypass X for fullscreen apps.
                                 * Requires push_model and !with_cursor. */

    /*
     * Optional crop of the tracked screen, used when track_output < 0.
     * Only honoured when box_w and box_h are both non-zero.
     *
     * NOTE: the driver silently ignores captureBox unless frameSize is set
     * as well, so both are always programmed together.
     */
    uint32_t box_x, box_y, box_w, box_h;
} nvfbc_config_t;

/*
 * Initialize NVFBC capture subsystem.
 * Must be called before any other nvfbc_* functions.
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_init(void);

/*
 * Check if NVFBC is available on this system.
 *
 * Returns: 1 if available, 0 otherwise.
 */
int nvfbc_is_available(void);

/*
 * Get current NVFBC status and display information.
 *
 * status: Pointer to status structure to fill.
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_get_status(nvfbc_status_t *status);

/*
 * Find a connected output whose tracked box exactly matches the given region.
 *
 * Tracking a single output is cheaper than tracking the whole X screen and
 * cropping it, so this is preferred when the region x11vnc serves happens to
 * be exactly one monitor.
 *
 * status:   Filled in by nvfbc_get_status().
 * out_id:   Receives the matching output ID.
 * out_name: Receives a pointer to the output name (owned by status).
 *
 * Returns: 1 on match, 0 otherwise.
 */
int nvfbc_find_output_by_box(const nvfbc_status_t *status,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t *out_id, const char **out_name);

/*
 * Start a capture session with the given configuration.
 *
 * config: Pointer to configuration structure.
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_start_capture(const nvfbc_config_t *config);

/*
 * Capture a single frame.
 *
 * buffer:     Pointer to receive the frame buffer pointer.
 *             Buffer is managed by NVFBC, do not free.
 * frame_info: Pointer to receive frame information.
 * timeout_ms: Timeout in milliseconds (0 = no wait, -1 = block forever).
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_grab_frame(uint8_t **buffer,
                                     nvfbc_frame_info_t *frame_info,
                                     int timeout_ms);

/*
 * Get differential map from last capture.
 * Only valid if with_diff_map was enabled in config.
 *
 * diff_map:   Pointer to receive diff map buffer pointer.
 * width:      Pointer to receive diff map width.
 * height:     Pointer to receive diff map height.
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_get_diff_map(uint8_t **diff_map,
                                       uint32_t *width,
                                       uint32_t *height);

/*
 * Stop the current capture session.
 *
 * Returns: NVFBC_CAP_OK on success, error code otherwise.
 */
nvfbc_cap_status_t nvfbc_stop_capture(void);

/*
 * Shutdown NVFBC capture subsystem and release all resources.
 */
void nvfbc_cleanup(void);

/*
 * Get human-readable error string for status code.
 *
 * status: Status code to describe.
 *
 * Returns: Static string describing the error.
 */
const char *nvfbc_status_string(nvfbc_cap_status_t status);

/*
 * Get the last NVFBC library error message.
 *
 * Returns: Error message from NVFBC library, or empty string.
 */
const char *nvfbc_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* NVFBC_CAPTURE_H */
