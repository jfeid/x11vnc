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

/* Open/close the encoder for a given served size.  Reopening with the same
 * geometry is a no-op. */
extern int h264_enc_open(int w, int h);
extern void h264_enc_close(void);
extern int h264_enc_is_open(void);

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
extern int h264_enc_frame(const unsigned char *bgra, int stride, int force_idr,
    const unsigned char **au, unsigned int *len);

#endif
