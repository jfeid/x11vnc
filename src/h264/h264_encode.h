/*
 * h264_encode.h - NVENC H.264 encoder for x11vnc, via libavcodec.
 *
 * Route A of docs/NVENC-H264-PLAN.md §4: libavcodec's h264_nvenc rather than
 * the NVENC SDK directly.  Needs no new dependencies on this machine and lets
 * FFmpeg own session lifecycle and driver quirks.
 */

#ifndef _X11VNC_H264_ENCODE_H
#define _X11VNC_H264_ENCODE_H

extern int h264_enable;                 /* -h264: encode live instead of a test file */
extern int h264_bitrate_kbps;           /* -h264_bitrate */
extern int h264_fps;                    /* -h264_fps */

/*
 * One encoder per tile.  The served region is split into horizontal bands
 * because a single full-screen rect is undecodable on the real client - see
 * h264_stream.h and the plan, §22 - and each band carries its own independent
 * H.264 stream, its own reference chain and its own IDRs.
 */
typedef struct h264_enc h264_enc_t;

/* Open an encoder for one tile.  Returns NULL on failure. */
extern h264_enc_t *h264_enc_open(int w, int h);
/* Close and NULL the handle.  Safe on an already-NULL handle. */
extern void h264_enc_close(h264_enc_t **ep);

/*
 * Encode one BGRA frame.  Returns 1 and points *au at an Annex-B access unit
 * when a packet is produced, 0 otherwise.  The buffer is owned by the encoder
 * and valid until the next call.
 *
 * The returned unit always begins with the SPS: the encoder is opened with
 * AV_CODEC_FLAG_GLOBAL_HEADER so parameter sets live in extradata and are
 * prepended here, rather than appearing only on keyframes.  TigerVNC's Windows
 * decoder requires SPS-first on every buffer - see the plan, §1.
 */
extern int h264_enc_frame(h264_enc_t *e, const unsigned char *bgra, int stride,
    int force_idr, const unsigned char **au, unsigned int *len);

#endif
