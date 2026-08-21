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

#if defined(HAVE_FFMPEG)

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

static AVCodecContext *ctx = NULL;
static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;
static int enc_w = 0, enc_h = 0;
static int64_t pts = 0;

/* extradata (SPS+PPS) + packet payload, rebuilt per frame */
static unsigned char *au_buf = NULL;
static unsigned int au_cap = 0;

int h264_enc_is_open(void) {
	return ctx != NULL;
}

void h264_enc_close(void) {
	if (frame) { av_frame_free(&frame); }
	if (pkt)   { av_packet_free(&pkt); }
	if (ctx)   { avcodec_free_context(&ctx); }
	free(au_buf); au_buf = NULL; au_cap = 0;
	enc_w = enc_h = 0;
	pts = 0;
}

int h264_enc_open(int w, int h) {
	const AVCodec *codec;
	int rc;
	double t_open;

	if (ctx != NULL && w == enc_w && h == enc_h) {
		return 1;
	}
	h264_enc_close();

	if (w <= 0 || h <= 0) {
		return 0;
	}
	/* H.264 needs even dimensions */
	if ((w & 1) || (h & 1)) {
		rfbLog("h264: %dx%d is not even, cannot encode\n", w, h);
		return 0;
	}

	codec = avcodec_find_encoder_by_name("h264_nvenc");
	if (codec == NULL) {
		rfbLog("h264: h264_nvenc not available in this libavcodec\n");
		return 0;
	}
	ctx = avcodec_alloc_context3(codec);
	if (ctx == NULL) {
		return 0;
	}

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
	ctx->gop_size = (h264_fps > 0 ? h264_fps : 30) * 10;

	/*
	 * Parameter sets into extradata rather than only on keyframes, so
	 * h264_enc_frame() can put an SPS at the head of every access unit.
	 */
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	av_opt_set(ctx->priv_data, "preset", "p4", 0);
	av_opt_set(ctx->priv_data, "tune", "ll", 0);
	av_opt_set(ctx->priv_data, "rc", "cbr", 0);
	av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
	av_opt_set(ctx->priv_data, "delay", "0", 0);

	t_open = enc_now();
	rc = avcodec_open2(ctx, codec, NULL);
	t_open = enc_now() - t_open;
	if (rc < 0) {
		rfbLog("h264: avcodec_open2 failed (%d)\n", rc);
		avcodec_free_context(&ctx);
		return 0;
	}

	frame = av_frame_alloc();
	pkt = av_packet_alloc();
	if (frame == NULL || pkt == NULL) {
		h264_enc_close();
		return 0;
	}
	/*
	 * No av_frame_get_buffer(): the frame points straight at x11vnc's
	 * framebuffer each tick, so there is no allocation and no copy on our
	 * side.  Safe because h264_frame_tick() runs inside watch_loop's send
	 * ban, after scan_for_updates() has finished writing the framebuffer.
	 */
	frame->format = AV_PIX_FMT_BGR0;
	frame->width = w;
	frame->height = h;

	enc_w = w;
	enc_h = h;
	pts = 0;
	rfbLog("h264: encoder open, %dx%d @%dfps, %d kbps, extradata %d bytes, "
	    "avcodec_open2 took %.0f ms\n", w, h, ctx->framerate.num,
	    h264_bitrate_kbps, ctx->extradata_size, t_open * 1000.0);
	return 1;
}

int h264_enc_frame(const unsigned char *bgra, int stride, int force_idr,
    const unsigned char **au, unsigned int *len) {
	unsigned int need;
	int rc;

	if (ctx == NULL || bgra == NULL) {
		return 0;
	}

	frame->data[0] = (uint8_t *) bgra;
	frame->linesize[0] = stride;
	frame->data[1] = frame->data[2] = frame->data[3] = NULL;
	frame->linesize[1] = frame->linesize[2] = frame->linesize[3] = 0;
	frame->pts = pts++;
	if (force_idr) {
		frame->pict_type = AV_PICTURE_TYPE_I;
		frame->flags |= AV_FRAME_FLAG_KEY;
	} else {
		frame->pict_type = AV_PICTURE_TYPE_NONE;
		frame->flags &= ~AV_FRAME_FLAG_KEY;
	}

	if (avcodec_send_frame(ctx, frame) < 0) {
		return 0;
	}
	rc = avcodec_receive_packet(ctx, pkt);
	if (rc < 0) {
		return 0;               /* EAGAIN: nothing ready yet */
	}

	/* SPS+PPS in front of every access unit - see the header comment. */
	need = (unsigned int) ctx->extradata_size + (unsigned int) pkt->size;
	if (need > au_cap) {
		unsigned char *nb = (unsigned char *) realloc(au_buf, need);
		if (nb == NULL) {
			av_packet_unref(pkt);
			return 0;
		}
		au_buf = nb;
		au_cap = need;
	}
	memcpy(au_buf, ctx->extradata, (size_t) ctx->extradata_size);
	memcpy(au_buf + ctx->extradata_size, pkt->data, (size_t) pkt->size);
	*au = au_buf;
	*len = need;
	av_packet_unref(pkt);
	return 1;
}

#else  /* !HAVE_FFMPEG */

int h264_enc_is_open(void) { return 0; }
void h264_enc_close(void) { }
int h264_enc_open(int w, int h) {
	(void) w; (void) h;
	rfbLog("h264: built without ffmpeg; -h264 unavailable\n");
	return 0;
}
int h264_enc_frame(const unsigned char *bgra, int stride, int force_idr,
    const unsigned char **au, unsigned int *len) {
	(void) bgra; (void) stride; (void) force_idr; (void) au; (void) len;
	return 0;
}

#endif
