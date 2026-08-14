/*
 * nvfbc_capture.c - NVIDIA Frame Buffer Capture (NVFBC) implementation for x11vnc
 *
 * This module provides high-performance screen capture using NVIDIA's NVFBC API.
 * Supports consumer GeForce GPUs using Sunshine's patch-free method.
 *
 * Based on reference implementation from:
 * - Sunshine (LizardByte) - https://github.com/LizardByte/Sunshine
 * - keylase/nvidia-patch - https://github.com/keylase/nvidia-patch
 *
 * Copyright (C) 2026 x11vnc-nvfbc contributors
 * License: GPL-2.0+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "NvFBC.h"
#include "nvfbc_capture.h"

/*
 * Magic private data to enable NVFBC on consumer GeForce GPUs.
 * This bypasses the need for driver patching.
 *
 * Source: https://github.com/keylase/nvidia-patch/blob/master/win/nvfbcwrp/nvfbcwrp_main.cpp
 * Also used by Sunshine: https://github.com/LizardByte/Sunshine
 */
static const unsigned int NVFBC_MAGIC_PRIVATE_DATA[4] = {
    0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA
};

/* Global state */
static struct {
    void *lib_handle;                       /* dlopen handle for libnvidia-fbc.so */
    NVFBC_API_FUNCTION_LIST api;            /* NVFBC function pointers */
    NVFBC_SESSION_HANDLE session;           /* Current session handle */
    int initialized;                        /* 1 if nvfbc_init() succeeded */
    int session_active;                     /* 1 if capture session is active */
    int capture_setup;                      /* 1 if ToSys setup was called */

    /* Capture buffers (managed by NVFBC) */
    void *frame_buffer;                     /* Frame data buffer */
    void *diff_map_buffer;                  /* Differential map buffer */
    NVFBC_SIZE diff_map_size;               /* Diff map dimensions */

    /* Configuration */
    nvfbc_config_t config;

    /* Last error message */
    char last_error[512];
} nvfbc_state = {0};

/* Helper to set last error */
static void set_error(const char *msg) {
    if (msg) {
        strncpy(nvfbc_state.last_error, msg, sizeof(nvfbc_state.last_error) - 1);
        nvfbc_state.last_error[sizeof(nvfbc_state.last_error) - 1] = '\0';
    } else {
        nvfbc_state.last_error[0] = '\0';
    }
}

/* Load NVFBC library and get function pointers */
static nvfbc_cap_status_t load_nvfbc_library(void) {
    PNVFBCCREATEINSTANCE NvFBCCreateInstance_fn;
    NVFBCSTATUS status;

    /* Try to load the library */
    nvfbc_state.lib_handle = dlopen("libnvidia-fbc.so.1", RTLD_NOW);
    if (!nvfbc_state.lib_handle) {
        nvfbc_state.lib_handle = dlopen("libnvidia-fbc.so", RTLD_NOW);
    }

    if (!nvfbc_state.lib_handle) {
        set_error("Failed to load libnvidia-fbc.so.1 or libnvidia-fbc.so");
        return NVFBC_CAP_ERR_NO_LIBRARY;
    }

    /* Get NvFBCCreateInstance function */
    NvFBCCreateInstance_fn = (PNVFBCCREATEINSTANCE)dlsym(nvfbc_state.lib_handle, "NvFBCCreateInstance");
    if (!NvFBCCreateInstance_fn) {
        set_error("Failed to find NvFBCCreateInstance symbol");
        dlclose(nvfbc_state.lib_handle);
        nvfbc_state.lib_handle = NULL;
        return NVFBC_CAP_ERR_NO_LIBRARY;
    }

    /* Initialize function table */
    memset(&nvfbc_state.api, 0, sizeof(nvfbc_state.api));
    nvfbc_state.api.dwVersion = NVFBC_VERSION;

    status = NvFBCCreateInstance_fn(&nvfbc_state.api);
    if (status != NVFBC_SUCCESS) {
        set_error("NvFBCCreateInstance failed");
        dlclose(nvfbc_state.lib_handle);
        nvfbc_state.lib_handle = NULL;
        return NVFBC_CAP_ERR_INIT;
    }

    return NVFBC_CAP_OK;
}

/* Create NVFBC session handle */
static nvfbc_cap_status_t create_session_handle(void) {
    NVFBC_CREATE_HANDLE_PARAMS params = {0};
    NVFBCSTATUS status;

    params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    /*
     * Set magic private data to enable NVFBC on consumer GeForce GPUs.
     * This is the "patch-free" method discovered by keylase and
     * implemented by Sunshine.
     */
    params.privateData = NVFBC_MAGIC_PRIVATE_DATA;
    params.privateDataSize = sizeof(NVFBC_MAGIC_PRIVATE_DATA);

    status = nvfbc_state.api.nvFBCCreateHandle(&nvfbc_state.session, &params);
    if (status != NVFBC_SUCCESS) {
        const char *err = nvfbc_state.api.nvFBCGetLastErrorStr(nvfbc_state.session);
        set_error(err ? err : "nvFBCCreateHandle failed");

        if (status == NVFBC_ERR_UNSUPPORTED) {
            return NVFBC_CAP_ERR_UNSUPPORTED;
        }
        return NVFBC_CAP_ERR_INIT;
    }

    return NVFBC_CAP_OK;
}

nvfbc_cap_status_t nvfbc_init(void) {
    nvfbc_cap_status_t ret;

    if (nvfbc_state.initialized) {
        return NVFBC_CAP_OK;  /* Already initialized */
    }

    memset(&nvfbc_state, 0, sizeof(nvfbc_state));

    /* Load library */
    ret = load_nvfbc_library();
    if (ret != NVFBC_CAP_OK) {
        return ret;
    }

    /* Create session handle */
    ret = create_session_handle();
    if (ret != NVFBC_CAP_OK) {
        dlclose(nvfbc_state.lib_handle);
        nvfbc_state.lib_handle = NULL;
        return ret;
    }

    nvfbc_state.initialized = 1;
    return NVFBC_CAP_OK;
}

int nvfbc_is_available(void) {
    nvfbc_cap_status_t status;

    if (nvfbc_state.initialized) {
        return 1;
    }

    status = nvfbc_init();
    if (status == NVFBC_CAP_OK) {
        return 1;
    }

    return 0;
}

nvfbc_cap_status_t nvfbc_get_status(nvfbc_status_t *status) {
    NVFBC_GET_STATUS_PARAMS params = {0};
    NVFBCSTATUS ret;
    uint32_t i;

    if (!nvfbc_state.initialized) {
        set_error("NVFBC not initialized");
        return NVFBC_CAP_ERR_INIT;
    }

    if (!status) {
        set_error("NULL status pointer");
        return NVFBC_CAP_ERR_INIT;
    }

    params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    ret = nvfbc_state.api.nvFBCGetStatus(nvfbc_state.session, &params);
    if (ret != NVFBC_SUCCESS) {
        const char *err = nvfbc_state.api.nvFBCGetLastErrorStr(nvfbc_state.session);
        set_error(err ? err : "nvFBCGetStatus failed");
        return NVFBC_CAP_ERR_INIT;
    }

    memset(status, 0, sizeof(*status));
    status->is_capture_possible = (params.bIsCapturePossible == NVFBC_TRUE) ? 1 : 0;
    status->screen_width = params.screenSize.w;
    status->screen_height = params.screenSize.h;
    status->xrandr_available = (params.bXRandRAvailable == NVFBC_TRUE) ? 1 : 0;
    status->num_outputs = params.dwOutputNum;

    /* Copy output information */
    for (i = 0; i < params.dwOutputNum && i < 5; i++) {
        status->outputs[i].id = params.outputs[i].dwId;
        strncpy(status->outputs[i].name, params.outputs[i].name, sizeof(status->outputs[i].name) - 1);
        status->outputs[i].x = params.outputs[i].trackedBox.x;
        status->outputs[i].y = params.outputs[i].trackedBox.y;
        status->outputs[i].width = params.outputs[i].trackedBox.w;
        status->outputs[i].height = params.outputs[i].trackedBox.h;
    }

    return NVFBC_CAP_OK;
}

int nvfbc_find_output_by_box(const nvfbc_status_t *status,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t *out_id, const char **out_name) {
    uint32_t i;

    if (!status || w == 0 || h == 0) {
        return 0;
    }

    for (i = 0; i < status->num_outputs && i < 5; i++) {
        if (status->outputs[i].x == x && status->outputs[i].y == y &&
            status->outputs[i].width == w && status->outputs[i].height == h) {
            if (out_id) {
                *out_id = status->outputs[i].id;
            }
            if (out_name) {
                *out_name = status->outputs[i].name;
            }
            return 1;
        }
    }
    return 0;
}

nvfbc_cap_status_t nvfbc_start_capture(const nvfbc_config_t *config) {
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS session_params = {0};
    NVFBC_TOSYS_SETUP_PARAMS setup_params = {0};
    NVFBCSTATUS ret;

    if (!nvfbc_state.initialized) {
        set_error("NVFBC not initialized");
        return NVFBC_CAP_ERR_INIT;
    }

    if (nvfbc_state.session_active) {
        /* Stop existing session first */
        nvfbc_stop_capture();
    }

    /* Save configuration */
    if (config) {
        memcpy(&nvfbc_state.config, config, sizeof(nvfbc_state.config));
    } else {
        /* Default configuration */
        memset(&nvfbc_state.config, 0, sizeof(nvfbc_state.config));
        nvfbc_state.config.with_cursor = 1;
        nvfbc_state.config.sampling_rate_ms = 16;  /* ~60 Hz */
        nvfbc_state.config.track_output = -1;      /* Entire screen */
        nvfbc_state.config.diff_map_scale = 16;
    }

    /* Create capture session */
    session_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    session_params.eCaptureType = NVFBC_CAPTURE_TO_SYS;
    session_params.bWithCursor = nvfbc_state.config.with_cursor ? NVFBC_TRUE : NVFBC_FALSE;
    session_params.dwSamplingRateMs = nvfbc_state.config.sampling_rate_ms;
    session_params.bPushModel = nvfbc_state.config.push_model ? NVFBC_TRUE : NVFBC_FALSE;
    session_params.bDisableAutoModesetRecovery = NVFBC_FALSE;

    /*
     * Direct capture lets NVFBC attach straight to a fullscreen unoccluded
     * application and bypass the X server.  The driver only honours it when
     * push model is on and the cursor is not composited.
     */
    session_params.bAllowDirectCapture =
        (nvfbc_state.config.allow_direct_capture &&
         nvfbc_state.config.push_model &&
         !nvfbc_state.config.with_cursor) ? NVFBC_TRUE : NVFBC_FALSE;

    /* Configure tracking */
    if (nvfbc_state.config.track_output >= 0) {
        session_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
        session_params.dwOutputId = (uint32_t)nvfbc_state.config.track_output;
    } else {
        session_params.eTrackingType = NVFBC_TRACKING_SCREEN;

        /*
         * Crop the tracked screen down to the region we actually serve.
         * captureBox on its own is ignored by the driver - frameSize must be
         * programmed to the same dimensions or the full screen comes back.
         */
        if (nvfbc_state.config.box_w > 0 && nvfbc_state.config.box_h > 0) {
            session_params.captureBox.x = nvfbc_state.config.box_x;
            session_params.captureBox.y = nvfbc_state.config.box_y;
            session_params.captureBox.w = nvfbc_state.config.box_w;
            session_params.captureBox.h = nvfbc_state.config.box_h;
            session_params.frameSize.w  = nvfbc_state.config.box_w;
            session_params.frameSize.h  = nvfbc_state.config.box_h;
            /* RGB formats have no rounding constraints; keep the exact size */
            session_params.bRoundFrameSize = NVFBC_FALSE;
        }
    }

    ret = nvfbc_state.api.nvFBCCreateCaptureSession(nvfbc_state.session, &session_params);
    if (ret != NVFBC_SUCCESS) {
        const char *err = nvfbc_state.api.nvFBCGetLastErrorStr(nvfbc_state.session);
        set_error(err ? err : "nvFBCCreateCaptureSession failed");
        return NVFBC_CAP_ERR_SESSION;
    }

    nvfbc_state.session_active = 1;

    /* Setup ToSys capture */
    setup_params.dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER;
    setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;  /* Native format, no conversion */
    setup_params.ppBuffer = &nvfbc_state.frame_buffer;
    setup_params.bWithDiffMap = nvfbc_state.config.with_diff_map ? NVFBC_TRUE : NVFBC_FALSE;
    setup_params.ppDiffMap = &nvfbc_state.diff_map_buffer;
    setup_params.dwDiffMapScalingFactor = nvfbc_state.config.diff_map_scale > 0 ?
                                          nvfbc_state.config.diff_map_scale : 16;

    ret = nvfbc_state.api.nvFBCToSysSetUp(nvfbc_state.session, &setup_params);
    if (ret != NVFBC_SUCCESS) {
        const char *err = nvfbc_state.api.nvFBCGetLastErrorStr(nvfbc_state.session);
        set_error(err ? err : "nvFBCToSysSetUp failed");
        nvfbc_stop_capture();
        return NVFBC_CAP_ERR_SETUP;
    }

    nvfbc_state.capture_setup = 1;
    nvfbc_state.diff_map_size = setup_params.diffMapSize;

    return NVFBC_CAP_OK;
}

nvfbc_cap_status_t nvfbc_grab_frame(uint8_t **buffer,
                                     nvfbc_frame_info_t *frame_info,
                                     int timeout_ms) {
    NVFBC_TOSYS_GRAB_FRAME_PARAMS grab_params = {0};
    NVFBC_FRAME_GRAB_INFO info = {0};
    NVFBCSTATUS ret;

    if (!nvfbc_state.initialized || !nvfbc_state.session_active || !nvfbc_state.capture_setup) {
        set_error("Capture not started");
        return NVFBC_CAP_ERR_INIT;
    }

    grab_params.dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;
    grab_params.pFrameGrabInfo = &info;

    if (timeout_ms == 0) {
        grab_params.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT;
        grab_params.dwTimeoutMs = 0;
    } else if (timeout_ms < 0) {
        grab_params.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS;
        grab_params.dwTimeoutMs = 0;  /* Block forever */
    } else {
        grab_params.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOFLAGS;
        grab_params.dwTimeoutMs = (uint32_t)timeout_ms;
    }

    ret = nvfbc_state.api.nvFBCToSysGrabFrame(nvfbc_state.session, &grab_params);
    if (ret == NVFBC_ERR_MUST_RECREATE) {
        set_error("Session needs to be recreated (modeset occurred)");
        return NVFBC_CAP_ERR_RECREATE;
    }
    if (ret != NVFBC_SUCCESS) {
        const char *err = nvfbc_state.api.nvFBCGetLastErrorStr(nvfbc_state.session);
        set_error(err ? err : "nvFBCToSysGrabFrame failed");
        return NVFBC_CAP_ERR_GRAB;
    }

    /* Return buffer pointer */
    if (buffer) {
        *buffer = (uint8_t *)nvfbc_state.frame_buffer;
    }

    /* Fill frame info */
    if (frame_info) {
        frame_info->width = info.dwWidth;
        frame_info->height = info.dwHeight;
        frame_info->byte_size = info.dwByteSize;
        frame_info->frame_id = info.dwCurrentFrame;
        frame_info->is_new_frame = (info.bIsNewFrame == NVFBC_TRUE) ? 1 : 0;
        frame_info->is_direct_capture = (info.bDirectCapture == NVFBC_TRUE) ? 1 : 0;
        frame_info->timestamp_us = info.ulTimestampUs;
    }

    return NVFBC_CAP_OK;
}

nvfbc_cap_status_t nvfbc_get_diff_map(uint8_t **diff_map,
                                       uint32_t *width,
                                       uint32_t *height) {
    if (!nvfbc_state.config.with_diff_map) {
        set_error("Diff map not enabled");
        return NVFBC_CAP_ERR_SETUP;
    }

    if (!nvfbc_state.diff_map_buffer) {
        set_error("No diff map available");
        return NVFBC_CAP_ERR_GRAB;
    }

    if (diff_map) {
        *diff_map = (uint8_t *)nvfbc_state.diff_map_buffer;
    }
    if (width) {
        *width = nvfbc_state.diff_map_size.w;
    }
    if (height) {
        *height = nvfbc_state.diff_map_size.h;
    }

    return NVFBC_CAP_OK;
}

nvfbc_cap_status_t nvfbc_stop_capture(void) {
    NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params = {0};

    if (!nvfbc_state.initialized) {
        return NVFBC_CAP_OK;
    }

    if (nvfbc_state.session_active) {
        params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        nvfbc_state.api.nvFBCDestroyCaptureSession(nvfbc_state.session, &params);
    }

    nvfbc_state.session_active = 0;
    nvfbc_state.capture_setup = 0;
    nvfbc_state.frame_buffer = NULL;
    nvfbc_state.diff_map_buffer = NULL;

    return NVFBC_CAP_OK;
}

void nvfbc_cleanup(void) {
    NVFBC_DESTROY_HANDLE_PARAMS params = {0};

    if (!nvfbc_state.initialized) {
        return;
    }

    nvfbc_stop_capture();

    if (nvfbc_state.session) {
        params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        nvfbc_state.api.nvFBCDestroyHandle(nvfbc_state.session, &params);
        nvfbc_state.session = 0;
    }

    if (nvfbc_state.lib_handle) {
        dlclose(nvfbc_state.lib_handle);
        nvfbc_state.lib_handle = NULL;
    }

    nvfbc_state.initialized = 0;
}

const char *nvfbc_status_string(nvfbc_cap_status_t status) {
    switch (status) {
        case NVFBC_CAP_OK:
            return "Success";
        case NVFBC_CAP_ERR_INIT:
            return "Initialization error";
        case NVFBC_CAP_ERR_NO_LIBRARY:
            return "NVFBC library not found (libnvidia-fbc.so)";
        case NVFBC_CAP_ERR_NO_DISPLAY:
            return "No display available";
        case NVFBC_CAP_ERR_SESSION:
            return "Failed to create capture session";
        case NVFBC_CAP_ERR_SETUP:
            return "Failed to setup capture";
        case NVFBC_CAP_ERR_GRAB:
            return "Failed to grab frame";
        case NVFBC_CAP_ERR_UNSUPPORTED:
            return "NVFBC not supported on this GPU/driver";
        case NVFBC_CAP_ERR_RECREATE:
            return "Session needs recreation (display mode changed)";
        default:
            return "Unknown error";
    }
}

const char *nvfbc_get_last_error(void) {
    return nvfbc_state.last_error;
}
