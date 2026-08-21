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

extern int h264_enter_rate;    /* -h264_enter, hundredths of a screen/s */
extern int h264_exit_rate;     /* -h264_exit */
extern int h264_force;                  /* -h264_force */
extern char *h264_testfile_path;        /* -h264_testfile */

/*
 * Non-zero while H.264 is the sole path to every connected client.  scan.c
 * consults this to skip marking damage entirely: telling libvncserver about
 * regions the H.264 stream already repaints costs both a redundant Tight
 * encode and the bandwidth to ship it.
 */
extern int h264_owns_output(void);

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
