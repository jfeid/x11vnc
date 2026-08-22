/*
 * h264_stream.h - RFB "open H.264" encoding (50) for x11vnc.
 *
 * See docs/NVENC-H264-PLAN.md.  libvncserver has no H.264 encoder and its
 * rfbEncodingH264 constant (0x48323634) is a different, incompatible number
 * with nothing behind it.  This emits encoding 50 rects itself, using
 * libvncserver's own output primitives, so no fork of the library is needed.
 */

#ifndef _X11VNC_H264_STREAM_H
#define _X11VNC_H264_STREAM_H

#include <rfb/rfb.h>

/* The open H.264 encoding, as implemented by TigerVNC.  NOT the same as
 * libvncserver's rfbEncodingH264. */
#define RFB_ENCODING_H264 50

/* Rect flags, per TigerVNC common/rfb/H264Decoder.cxx */
#define H264_RESET_CONTEXT      0x1
#define H264_RESET_ALL_CONTEXTS 0x2

/*
 * RFB fence flow control (plan §17 fix).  The full-frame H.264 push has no
 * backpressure a stock libvncserver 0.9.15 can see: it has neither fences nor
 * continuous updates, sshd hides the socket queue, and TigerVNC pipelines its
 * update requests one deep - so the server stays a frame ahead and keeps the
 * viewer's single-threaded, output-corked socket loop from ever returning to
 * its event pump.  A fence round-trip is the one true acknowledgement: send one
 * after each frame and withhold the next until the client echoes it.  The
 * client drains, its socket runs dry, the loop yields, input and redraw breathe.
 *
 * Numbers are RFB's own (TigerVNC common/rfb/{msgTypes,encodings,fenceTypes}.h).
 * The pseudo-encoding is how the client advertises support in SetEncodings; the
 * message (248, both directions) carries the fence itself.
 */
#define RFB_ENCODING_FENCE      (-312)
#define RFB_MSG_FENCE           248
#define FENCE_FLAG_BLOCK_BEFORE 0x00000001u
#define FENCE_FLAG_BLOCK_AFTER  0x00000002u
#define FENCE_FLAG_REQUEST      0x80000000u

extern int h264_enter_rate;    /* -h264_enter, hundredths of a screen/s */
extern int h264_exit_rate;     /* -h264_exit */
extern int h264_force;                  /* -h264_force */
extern char *h264_testfile_path;        /* -h264_testfile */
extern int h264_fence;                  /* -h264_nofence disables; on by default */
extern int h264_fence_timeout_ms;       /* send anyway if no echo within this */

/*
 * Largest H.264 rect the client can actually display, in pixels.
 *
 * THIS IS NOT A TUNING KNOB - it is a hard client limit, measured (plan §22).
 * TigerVNC's H264WinDecoderContext allocates `decoded_buffer` once at
 * construction, from an output type that does not yet know the stream's
 * resolution, and never resizes it when MF_E_TRANSFORM_STREAM_CHANGE tells it
 * the real size.  A frame whose NV12 image exceeds that buffer is never
 * produced, `decoded` stays false, imageRect() is never called, and the viewer
 * shows a black or stale rect FOREVER, with no error anywhere.
 *
 * Measured against the real client by serving fixed streams of each size:
 *
 *      1280x720   0.92 Mpx  plays        2560x1080  2.76 Mpx  BLACK
 *      1600x1200  1.92 Mpx  plays        2048x1440  2.95 Mpx  BLACK
 *      1920x1080  2.07 Mpx  plays        2560x1440  3.69 Mpx  BLACK
 *      2048x1152  2.36 Mpx  plays                   (both NVENC and libx264)
 *
 * Every result fits one rule: NV12 size <= 2048*1152*3/2 = 3,538,944 bytes.
 * Not width - 2048x1152 plays and 2048x1440 does not.  So the served region is
 * split into as many horizontal bands as it takes to keep each under this.
 */
#define H264_MAX_TILE_PIXELS 2359296    /* 2048x1152 */
#define H264_MAX_TILES 8

extern int h264_tile_pixels;            /* -h264_tile_pixels, 0 = no tiling */

/* Drop every tile encoder so the next tick reopens them.  Bitrate and frame
 * rate are fixed at encoder open, so the remote-control knobs call this to
 * make a new value take effect without restarting the server. */
extern void h264_encoders_reset(void);

/*
 * Non-zero while H.264 is the sole path to every connected client.  scan.c
 * consults this to skip marking damage entirely: telling libvncserver about
 * regions the H.264 stream already repaints costs both a redundant Tight
 * encode and the bandwidth to ship it.
 */
extern int h264_owns_output(void);

/*
 * Phase 3' (plan §24): while H.264 owns every client, main_fb has no reader -
 * the encoder takes its pixels straight from NVFBC's capture buffer and
 * libvncserver is told about no damage at all.  Filling it is then two copies
 * per dirty tile of memory traffic nobody consumes, so scan.c asks here
 * whether it may skip them, and reports back when it has, so the exit path
 * knows main_fb must be refilled before Tight repaints from it.
 */
extern int h264_fb_copy_skippable(void);
extern void h264_fb_copy_skipped(void);

/* Called once per watch_loop cycle; drives the test source for now. */
extern void h264_frame_tick(int tile_diffs);

/* Register the protocol extension so SetEncodings(50) is noticed.
 * Call once, before clients connect. */
extern void h264_stream_init(void);

/* Non-zero if this client negotiated encoding 50 (or -h264_force is set). */
extern int h264_client_active(rfbClientPtr cl);

/* Emit one encoding-50 rect as a complete FramebufferUpdate.
 * `data` is an Annex-B access unit and MUST begin with an SPS NAL - see
 * the plan, §1: TigerVNC's Windows decoder rejects anything else silently.
 * Returns 1 on success, 0 if the write failed.
 *
 * CALLER MUST HOLD the watch_loop send ban (LOCK(cl->sendMutex) for every
 * client) or be single-threaded.  This function does not lock: watch_loop
 * already holds that mutex across its scan section, and re-taking a
 * non-recursive pthread mutex on the same thread deadlocks. */
extern int h264_send_rect(rfbClientPtr cl, int x, int y, int w, int h,
    const unsigned char *data, unsigned int len, unsigned int flags);

/* Test source container: "X11VNCAU" + u32 width + u32 height, then
 * [u32 len][bytes] repeated. Self-describing so the rect geometry cannot
 * silently disagree with the stream - a mismatch there renders black.
 * Phase 1 uses this to prove the transport without involving an encoder,
 * because a malformed stream renders black with no diagnostic anywhere.
 * Returns 1 if loaded. */
extern int h264_testfile_load(const char *path);
extern int h264_testfile_loaded(void);

/* Next access unit from the test source, cycling.  *flags gets
 * H264_RESET_ALL_CONTEXTS on the unit that restarts the loop. */
extern const unsigned char *h264_testfile_next(unsigned int *len,
    unsigned int *flags);

#endif
