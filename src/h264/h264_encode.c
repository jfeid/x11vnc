/*
 * h264_encode.c - NVENC H.264 encoding for x11vnc via libavcodec.
 */

#include "x11vnc.h"
#include "h264/h264_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double enc_now(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

int h264_enable = 0;
int h264_bitrate_kbps = 20000;
int h264_fps = 30;
int h264_cq = 0;
char *h264_preset = NULL;       /* NULL means the built-in default below */
char *h264_tune = NULL;
char *h264_cuda_sched = NULL;   /* NULL means the default - see h264_cuda_device() */

#if defined(HAVE_FFMPEG)

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

/*
 * A minimal slice of the CUDA driver API, declared here rather than pulled in
 * from <cuda.h> so the build needs no CUDA toolkit, and resolved from
 * libcuda.so.1 at runtime - the same approach the NVFBC code takes to
 * libnvidia-fbc.  Defining CUDA_VERSION is what stops hwcontext_cuda.h
 * including <cuda.h> for these same three types.
 */
#define CUDA_VERSION 12000
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef int CUdevice;
#define CU_CTX_SCHED_SPIN          0x01
#define CU_CTX_SCHED_YIELD         0x02
#define CU_CTX_SCHED_BLOCKING_SYNC 0x04

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <dlfcn.h>

/*
 * ------------------------------------------------------------------
 * The CUDA context the tile encoders run on (plan §26)
 * ------------------------------------------------------------------
 *
 * Left to itself, libavcodec creates one CUDA context per encoder with default
 * scheduling flags.  Each context runs a driver thread that sits in a poll()
 * loop on the NVIDIA fd for as long as GPU work is outstanding - measured at
 * 42,000 wakeups/s and ~35% of a core EACH under heavy GPU load, almost all of
 * it system time.  With two tiles that is two thirds of the process.
 *
 * -h264_cuda_sched builds ONE context up front instead, shared by every tile:
 *
 *   blocking  (default)  CU_CTX_SCHED_BLOCKING_SYNC, shared
 *   spin                 CU_CTX_SCHED_SPIN, shared
 *   yield                CU_CTX_SCHED_YIELD, shared
 *   auto                 libavcodec's own context per encoder - what this
 *                        encoder did before, kept as the way back
 *
 * Measured, full-screen load, 2 tiles (plan §27):
 *
 *   auto      2 driver threads   105.3% total   15.7 fps delivered
 *   spin      1 driver thread     69.5%         16.1
 *   yield     1 driver thread     70.8%         15.7
 *   blocking  1 driver thread     69.9%         15.6
 *
 * **The scheduling flag itself is inert here** - all three shared modes poll at
 * the same ~45,000 wakeups/s, so CU_CTX_SCHED_BLOCKING_SYNC does not change
 * what the driver's event-handler thread does.  What saves the 36 points is
 * having one context rather than one per encoder.  The flag is still exposed
 * because it costs nothing and a different driver may not be so indifferent.
 *
 * The context lives for the life of the process, like the NVFBC session: it is
 * created on the first encoder open and reused by every one after it, across
 * any number of gate entries and tile counts.
 */
static AVBufferRef *cuda_dev = NULL;
static CUcontext cuda_own_ctx = NULL;
static void *cuda_lib = NULL;

static const char *cuda_sched_mode(void) {
	return h264_cuda_sched ? h264_cuda_sched : "blocking";
}

static unsigned int cuda_sched_flag(const char *mode) {
	if (!strcmp(mode, "blocking"))     return CU_CTX_SCHED_BLOCKING_SYNC;
	if (!strcmp(mode, "spin"))         return CU_CTX_SCHED_SPIN;
	if (!strcmp(mode, "yield"))        return CU_CTX_SCHED_YIELD;
	return 0;
}

static AVBufferRef *h264_cuda_device(void) {
	static int tried = 0;
	int (*p_cuInit)(unsigned int);
	int (*p_cuDeviceGet)(CUdevice *, int);
	int (*p_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
	const char *mode = cuda_sched_mode();
	unsigned int flags;
	CUdevice dev = 0;
	int rc;

	if (!strcmp(mode, "auto")) {
		return NULL;
	}
	if (tried) {
		return cuda_dev;        /* NULL if it failed; do not retry per tile */
	}
	tried = 1;

	flags = cuda_sched_flag(mode);
	if (flags == 0) {
		rfbLog("h264: unknown -h264_cuda_sched '%s' (want blocking, spin, "
		    "yield or auto); leaving it to libavcodec\n", mode);
		return NULL;
	}

	cuda_lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (cuda_lib == NULL) {
		rfbLog("h264: dlopen libcuda.so.1 failed (%s); leaving the CUDA "
		    "context to libavcodec\n", dlerror());
		return NULL;
	}
	p_cuInit      = dlsym(cuda_lib, "cuInit");
	p_cuDeviceGet = dlsym(cuda_lib, "cuDeviceGet");
	/* _v2 is the ABI everything since CUDA 4 actually binds to */
	p_cuCtxCreate = dlsym(cuda_lib, "cuCtxCreate_v2");
	if (p_cuInit == NULL || p_cuDeviceGet == NULL || p_cuCtxCreate == NULL) {
		rfbLog("h264: libcuda.so.1 is missing cuInit/cuDeviceGet/"
		    "cuCtxCreate_v2; leaving the CUDA context to libavcodec\n");
		return NULL;
	}
	if ((rc = p_cuInit(0)) != 0 || (rc = p_cuDeviceGet(&dev, 0)) != 0) {
		rfbLog("h264: CUDA init failed (%d); leaving the context to "
		    "libavcodec\n", rc);
		return NULL;
	}
	/*
	 * cuCtxCreate leaves the new context current on THIS thread, which is
	 * exactly what AV_CUDA_USE_CURRENT_CONTEXT adopts.  Both calls therefore
	 * have to happen on the same thread, and they do: the first encoder open
	 * is from h264_frame_tick(), on watch_loop's thread.
	 */
	if ((rc = p_cuCtxCreate(&cuda_own_ctx, flags, dev)) != 0) {
		rfbLog("h264: cuCtxCreate(sched=%s) failed (%d); leaving the "
		    "context to libavcodec\n", mode, rc);
		return NULL;
	}
	rc = av_hwdevice_ctx_create(&cuda_dev, AV_HWDEVICE_TYPE_CUDA, NULL, NULL,
	    AV_CUDA_USE_CURRENT_CONTEXT);
	if (rc < 0) {
		rfbLog("h264: av_hwdevice_ctx_create failed (%d); leaving the "
		    "context to libavcodec\n", rc);
		cuda_dev = NULL;
		return NULL;
	}
	rfbLog("h264: one shared CUDA context for every tile, sched=%s "
	    "(-h264_cuda_sched auto for libavcodec's own, one per encoder)\n",
	    mode);
	return cuda_dev;
}

struct h264_enc {
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	int w, h;
	int64_t pts;
	/* extradata (SPS+PPS) + packet payload, rebuilt per frame */
	unsigned char *au_buf;
	unsigned int au_cap;
};

void h264_enc_close(h264_enc_t **ep) {
	h264_enc_t *e;

	if (ep == NULL || *ep == NULL) {
		return;
	}
	e = *ep;
	if (e->frame) { av_frame_free(&e->frame); }
	if (e->pkt)   { av_packet_free(&e->pkt); }
	if (e->ctx)   { avcodec_free_context(&e->ctx); }
	free(e->au_buf);
	free(e);
	*ep = NULL;
}

h264_enc_t *h264_enc_open(int w, int h) {
	const AVCodec *codec;
	h264_enc_t *e;
	AVCodecContext *ctx;
	int rc;
	double t_open;

	if (w <= 0 || h <= 0) {
		return NULL;
	}
	/* H.264 needs even dimensions */
	if ((w & 1) || (h & 1)) {
		rfbLog("h264: %dx%d is not even, cannot encode\n", w, h);
		return NULL;
	}

	codec = avcodec_find_encoder_by_name("h264_nvenc");
	if (codec == NULL) {
		rfbLog("h264: h264_nvenc not available in this libavcodec\n");
		return NULL;
	}
	e = (h264_enc_t *) calloc(1, sizeof(*e));
	if (e == NULL) {
		return NULL;
	}
	ctx = avcodec_alloc_context3(codec);
	if (ctx == NULL) {
		free(e);
		return NULL;
	}
	e->ctx = ctx;

	ctx->width = w;
	ctx->height = h;
	/*
	 * Feed NVENC packed BGRA and let the GPU do the colour conversion.
	 * Converting to NV12 with libswscale first cost ~55% of a core at
	 * 2560x1440@30 - it was the single largest expense in the whole path,
	 * larger than the encode, and it made scrolling visibly laggy because
	 * the encode tick could not keep up.  NVENC takes RGB natively.
	 */
	ctx->pix_fmt = AV_PIX_FMT_BGR0;
	ctx->time_base.num = 1;
	ctx->time_base.den = h264_fps > 0 ? h264_fps : 30;
	ctx->framerate.num = h264_fps > 0 ? h264_fps : 30;
	ctx->framerate.den = 1;
	ctx->bit_rate = (int64_t) h264_bitrate_kbps * 1000;
	ctx->rc_max_rate = ctx->bit_rate;
	ctx->rc_buffer_size = (int) (ctx->bit_rate / 2);

	/* No B-frames: any reordering is latency we cannot spend (plan §5). */
	ctx->max_b_frames = 0;
	/*
	 * 2 s of GOP, not 10.  The IDR interval is the worst case for how long
	 * the client can show nothing if it ever fails to start on our keyframe
	 * - it recovers only at the next IDR.  Ten seconds of frozen desktop is
	 * indistinguishable from a hang; two is a blink.  Cheap insurance now
	 * that forced-idr makes the deliberate keyframes real IDRs anyway.
	 */
	ctx->gop_size = (h264_fps > 0 ? h264_fps : 30) * 2;

	/*
	 * Parameter sets into extradata rather than only on keyframes, so
	 * h264_enc_frame() can put an SPS at the head of every access unit.
	 */
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	av_opt_set(ctx->priv_data, "preset",
	    h264_preset ? h264_preset : "p4", 0);
	av_opt_set(ctx->priv_data, "tune",
	    h264_tune ? h264_tune : "ll", 0);
	/*
	 * VBR, not CBR.  Strict CBR makes NVENC pad every frame with filler
	 * NALs to hit the bitrate exactly whatever the content: measured on the
	 * wire, access units were a constant 500,051 bytes of which 96%, 39%
	 * and 50% was filler on three consecutive frames.  That is bandwidth
	 * spent transmitting padding, on a link this project exists to fit
	 * inside.  VBR with the same cap keeps the ceiling and lets a static or
	 * cheap frame actually be small.
	 */
	av_opt_set(ctx->priv_data, "rc", "vbr", 0);
	/*
	 * MANDATORY, and the cause of a silent freeze without it.  We ask for a
	 * keyframe by setting pict_type = AV_PICTURE_TYPE_I, but h264_nvenc
	 * defaults forced-idr to false, so that request produces a plain I
	 * slice rather than an IDR - confirmed on the wire: the very access
	 * unit carrying H264_RESET_CONTEXT decoded as SPS+PPS+I-slice(NON-IDR).
	 *
	 * The client destroys its decoder context on that flag and builds a new
	 * one, and a fresh H.264 decoder cannot start on a non-IDR picture.
	 * TigerVNC's Media Foundation path then gets NEED_MORE_INPUT forever,
	 * never sets `decoded`, and never calls pb->imageRect() - so the viewer
	 * silently paints nothing until the next GOP boundary, with no error
	 * anywhere (plan §1: encoding 50 has no diagnostics).
	 */
	av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
	/*
	 * Quality target.  Measured after the VBR switch: the encoder was
	 * spending only 1.6-3.2 Mbps of a 20 Mbps ceiling and the operator
	 * found text "a bit soft during motion".  cq spends that headroom on
	 * quality; it costs the GPU, not the CPU, and at these rates it barely
	 * moves the link.  Left at 0 (bitrate-driven) unless asked for, so this
	 * changes nothing until a value has actually been judged on real content.
	 */
	if (h264_cq > 0) {
		av_opt_set_int(ctx->priv_data, "cq", h264_cq, 0);
	}
	av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
	av_opt_set(ctx->priv_data, "delay", "0", 0);

	/*
	 * Hand libavcodec a context of our own when asked, so every tile shares
	 * one and its completion-wait mode is ours to choose.  Doing nothing
	 * here is the "auto" path: nvenc then makes its own, per encoder.
	 */
	{
		AVBufferRef *dev = h264_cuda_device();

		if (dev != NULL) {
			ctx->hw_device_ctx = av_buffer_ref(dev);
		}
	}

	t_open = enc_now();
	rc = avcodec_open2(ctx, codec, NULL);
	t_open = enc_now() - t_open;
	if (rc < 0) {
		rfbLog("h264: avcodec_open2 failed (%d)\n", rc);
		h264_enc_close(&e);
		return NULL;
	}

	e->frame = av_frame_alloc();
	e->pkt = av_packet_alloc();
	if (e->frame == NULL || e->pkt == NULL) {
		h264_enc_close(&e);
		return NULL;
	}
	/*
	 * No av_frame_get_buffer(): the frame points straight at x11vnc's
	 * framebuffer each tick, so there is no allocation and no copy on our
	 * side.  Safe because h264_frame_tick() runs inside watch_loop's send
	 * ban, after scan_for_updates() has finished writing the framebuffer.
	 */
	e->frame->format = AV_PIX_FMT_BGR0;
	e->frame->width = w;
	e->frame->height = h;

	e->w = w;
	e->h = h;
	e->pts = 0;
	rfbLog("h264: encoder open, %dx%d @%dfps, %d kbps, cq %d, preset %s, "
	    "tune %s, cuda %s, extradata %d bytes, avcodec_open2 took %.0f ms\n",
	    w, h, ctx->framerate.num, h264_bitrate_kbps, h264_cq,
	    h264_preset ? h264_preset : "p4", h264_tune ? h264_tune : "ll",
	    ctx->hw_device_ctx ? cuda_sched_mode() : "auto",
	    ctx->extradata_size, t_open * 1000.0);
	return e;
}

int h264_enc_frame(h264_enc_t *e, const unsigned char *bgra, int stride,
    int force_idr, const unsigned char **au, unsigned int *len) {
	unsigned int need;
	int rc;

	if (e == NULL || e->ctx == NULL || bgra == NULL) {
		return 0;
	}

	e->frame->data[0] = (uint8_t *) bgra;
	e->frame->linesize[0] = stride;
	e->frame->data[1] = e->frame->data[2] = e->frame->data[3] = NULL;
	e->frame->linesize[1] = e->frame->linesize[2] = e->frame->linesize[3] = 0;
	e->frame->pts = e->pts++;
	if (force_idr) {
		e->frame->pict_type = AV_PICTURE_TYPE_I;
		e->frame->flags |= AV_FRAME_FLAG_KEY;
	} else {
		e->frame->pict_type = AV_PICTURE_TYPE_NONE;
		e->frame->flags &= ~AV_FRAME_FLAG_KEY;
	}

	if (avcodec_send_frame(e->ctx, e->frame) < 0) {
		return 0;
	}
	rc = avcodec_receive_packet(e->ctx, e->pkt);
	if (rc < 0) {
		return 0;               /* EAGAIN: nothing ready yet */
	}

	/* SPS+PPS in front of every access unit - see the header comment. */
	need = (unsigned int) e->ctx->extradata_size + (unsigned int) e->pkt->size;
	if (need > e->au_cap) {
		unsigned char *nb = (unsigned char *) realloc(e->au_buf, need);
		if (nb == NULL) {
			av_packet_unref(e->pkt);
			return 0;
		}
		e->au_buf = nb;
		e->au_cap = need;
	}
	memcpy(e->au_buf, e->ctx->extradata, (size_t) e->ctx->extradata_size);
	memcpy(e->au_buf + e->ctx->extradata_size, e->pkt->data,
	    (size_t) e->pkt->size);
	*au = e->au_buf;
	*len = need;
	av_packet_unref(e->pkt);
	return 1;
}

#else  /* !HAVE_FFMPEG */

void h264_enc_close(h264_enc_t **ep) { (void) ep; }
h264_enc_t *h264_enc_open(int w, int h) {
	(void) w; (void) h;
	rfbLog("h264: built without ffmpeg; -h264 unavailable\n");
	return NULL;
}
int h264_enc_frame(h264_enc_t *e, const unsigned char *bgra, int stride,
    int force_idr, const unsigned char **au, unsigned int *len) {
	(void) e; (void) bgra; (void) stride; (void) force_idr;
	(void) au; (void) len;
	return 0;
}

#endif
