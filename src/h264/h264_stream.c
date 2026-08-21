/*
 * h264_stream.c - emit RFB encoding 50 ("open H.264") from x11vnc.
 *
 * Why this exists rather than a libvncserver encoder: x11vnc links the system
 * libvncserver, which has no H.264 support at all.  But it exports everything
 * needed to write rects alongside its own encoders - rfbRegisterProtocolExtension
 * to notice the client asking for encoding 50, and cl->updateBuf / rfbSendUpdateBuf
 * / rfbWriteExact to emit one.  So the work stays in the fork.
 */

#include "x11vnc.h"
#include "cleanup.h"
#include "scan.h"
#include "h264/h264_stream.h"
#include "h264/h264_encode.h"
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int h264_force = 0;             /* -h264_force: assume every client wants it */
char *h264_testfile_path = NULL;
static int extension_registered = 0;

/* ------------------------------------------------------------------ *
 * Per-client state
 * ------------------------------------------------------------------ */

typedef struct {
	int enabled;                    /* client listed encoding 50 */
	int preferred;                  /* ...and listed it ahead of everything else */
} h264_client_t;

static rfbBool h264_new_client(rfbClientPtr cl, void **data) {
	h264_client_t *st = (h264_client_t *) calloc(1, sizeof(h264_client_t));
	(void) cl;
	if (st == NULL) {
		return FALSE;
	}
	*data = st;
	return TRUE;
}

/*
 * libvncserver hands us every encoding it does not recognise itself, which is
 * how encoding 50 reaches us at all.  encodingNumber == 0 is its "the client
 * re-sent SetEncodings, forget what you knew" signal, not encoding Raw.
 */
static rfbBool h264_enable_pseudo(rfbClientPtr cl, void **data, int encoding) {
	h264_client_t *st;

	if (*data == NULL && !h264_new_client(cl, data)) {
		return FALSE;
	}
	st = (h264_client_t *) *data;

	if (encoding == 0) {
		st->enabled = 0;
		st->preferred = 0;
		return FALSE;
	}
	if (encoding == RFB_ENCODING_H264) {
		st->enabled = 1;
		/*
		 * Honour the viewer's "Preferred encoding" setting.  TigerVNC
		 * advertises encoding 50 whether or not the user selected it -
		 * it sat at position 19 of 26 with Tight chosen - so merely
		 * being listed means "I can decode this", not "I want it".
		 *
		 * libvncserver walks SetEncodings in order and assigns
		 * cl->preferredEncoding on the first encoding it recognises,
		 * having reset it to -1 beforehand.  Ours is not one it knows,
		 * so if it is still -1 when we are called, encoding 50 arrived
		 * ahead of every real encoding: the user picked H.264.
		 */
		st->preferred = (cl->preferredEncoding == -1);
		rfbLog("h264: client %s offers encoding 50 (%s)\n",
		    cl->host ? cl->host : "?",
		    st->preferred ? "preferred - hybrid active"
		                  : "not preferred - staying on Tight");
		return TRUE;
	}
	return FALSE;
}

static void h264_close_client(rfbClientPtr cl, void *data) {
	(void) cl;
	free(data);
}

static int h264_pseudo_encodings[] = { RFB_ENCODING_H264, 0 };

static rfbProtocolExtension h264_extension = {
	h264_new_client,        /* newClient */
	NULL,                   /* init */
	h264_pseudo_encodings,  /* pseudoEncodings */
	h264_enable_pseudo,     /* enablePseudoEncoding */
	NULL,                   /* handleMessage */
	h264_close_client,      /* close */
	NULL,                   /* usage */
	NULL,                   /* processArgument */
	NULL                    /* next */
};

void h264_stream_init(void) {
	if (extension_registered) {
		return;
	}
	rfbRegisterProtocolExtension(&h264_extension);
	extension_registered = 1;
	rfbLog("h264: registered protocol extension for encoding %d\n",
	    RFB_ENCODING_H264);
}

int h264_client_active(rfbClientPtr cl) {
	h264_client_t *st;

	if (h264_force) {
		return 1;
	}
	if (cl == NULL) {
		return 0;
	}
	st = (h264_client_t *) rfbGetExtensionClientData(cl, &h264_extension);
	return st != NULL && st->enabled && st->preferred;
}

/* ------------------------------------------------------------------ *
 * Rect emission
 * ------------------------------------------------------------------ */

int h264_send_rect(rfbClientPtr cl, int x, int y, int w, int h,
    const unsigned char *data, unsigned int len, unsigned int flags) {
	rfbFramebufferUpdateMsg fu;
	rfbFramebufferUpdateRectHeader rect;
	unsigned char sub[8];
	int ok = 1;

	if (cl == NULL || data == NULL) {
		return 0;
	}

	/*
	 * NO LOCK HERE - deliberately.
	 *
	 * watch_loop() already holds LOCK(cl->sendMutex) for every client
	 * across its whole scan section (screen.c, the "send ban"), which is
	 * exactly the window in which libvncserver is guaranteed not to be
	 * writing.  Taking it again on this thread self-deadlocks: LOCK() is a
	 * real pthread_mutex_lock - LIBVNCSERVER_HAVE_LIBPTHREAD is defined in
	 * rfb/rfbconfig.h, not in x11vnc's own config.h, so it is easy to
	 * assume it compiles away - and the mutex is not recursive.  Doing this
	 * killed watch_loop on the first H.264 client and every session went
	 * blank with no error logged anywhere.
	 *
	 * Caller contract: hold the send ban, or be single-threaded.
	 */

	/*
	 * Anything libvncserver already buffered belongs to a different update;
	 * flush it so ours starts on a message boundary.
	 */
	if (cl->ublen > 0 && !rfbSendUpdateBuf(cl)) {
		return 0;
	}

	memset(&fu, 0, sizeof(fu));
	fu.type = rfbFramebufferUpdate;
	fu.nRects = Swap16IfLE(1);
	memcpy(&cl->updateBuf[cl->ublen], &fu, sz_rfbFramebufferUpdateMsg);
	cl->ublen += sz_rfbFramebufferUpdateMsg;

	rect.r.x = Swap16IfLE(x);
	rect.r.y = Swap16IfLE(y);
	rect.r.w = Swap16IfLE(w);
	rect.r.h = Swap16IfLE(h);
	rect.encoding = Swap32IfLE(RFB_ENCODING_H264);
	memcpy(&cl->updateBuf[cl->ublen], &rect,
	    sz_rfbFramebufferUpdateRectHeader);
	cl->ublen += sz_rfbFramebufferUpdateRectHeader;

	/* encoding-50 sub-header: U32 length, U32 flags, both big-endian */
	sub[0] = (unsigned char) (len >> 24);
	sub[1] = (unsigned char) (len >> 16);
	sub[2] = (unsigned char) (len >> 8);
	sub[3] = (unsigned char) (len);
	sub[4] = (unsigned char) (flags >> 24);
	sub[5] = (unsigned char) (flags >> 16);
	sub[6] = (unsigned char) (flags >> 8);
	sub[7] = (unsigned char) (flags);
	memcpy(&cl->updateBuf[cl->ublen], sub, 8);
	cl->ublen += 8;

	/* header out, then the payload straight to the socket - an access unit
	 * is far larger than UPDATE_BUF_SIZE and copying it twice is pointless */
	if (!rfbSendUpdateBuf(cl)) {
		ok = 0;
	} else if (len > 0 && rfbWriteExact(cl, (const char *) data,
	    (int) len) < 0) {
		ok = 0;
	}

	return ok;
}

/* ------------------------------------------------------------------ *
 * Test source - Phase 1
 * ------------------------------------------------------------------ */

static unsigned char *tf_blob = NULL;
static size_t tf_size = 0;
static unsigned int *tf_off = NULL;     /* offset of each access unit */
static unsigned int *tf_len = NULL;
static int tf_count = 0;
static int tf_pos = 0;
static unsigned int tf_w = 0, tf_h = 0;

int h264_testfile_loaded(void) {
	return tf_count > 0;
}

int h264_testfile_load(const char *path) {
	FILE *f;
	long size;
	size_t got, p;
	int n, cap;

	if (path == NULL) {
		return 0;
	}
	f = fopen(path, "rb");
	if (f == NULL) {
		rfbLog("h264: cannot open test file %s\n", path);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		rfbLog("h264: test file %s is empty\n", path);
		return 0;
	}
	tf_blob = (unsigned char *) malloc((size_t) size);
	if (tf_blob == NULL) {
		fclose(f);
		return 0;
	}
	got = fread(tf_blob, 1, (size_t) size, f);
	fclose(f);
	if (got != (size_t) size) {
		free(tf_blob);
		tf_blob = NULL;
		rfbLog("h264: short read on %s\n", path);
		return 0;
	}
	tf_size = got;

	/* header: "X11VNCAU" + u32 width + u32 height */
	if (tf_size < 16 || memcmp(tf_blob, "X11VNCAU", 8) != 0) {
		rfbLog("h264: %s is not an X11VNCAU container\n", path);
		free(tf_blob);
		tf_blob = NULL;
		return 0;
	}
	tf_w = ((unsigned int) tf_blob[8] << 24) | ((unsigned int) tf_blob[9] << 16) |
	       ((unsigned int) tf_blob[10] << 8) | ((unsigned int) tf_blob[11]);
	tf_h = ((unsigned int) tf_blob[12] << 24) | ((unsigned int) tf_blob[13] << 16) |
	       ((unsigned int) tf_blob[14] << 8) | ((unsigned int) tf_blob[15]);

	/* container is [u32 be length][payload] repeated */
	cap = 64;
	tf_off = (unsigned int *) malloc(sizeof(unsigned int) * cap);
	tf_len = (unsigned int *) malloc(sizeof(unsigned int) * cap);
	if (tf_off == NULL || tf_len == NULL) {
		return 0;
	}
	n = 0;
	p = 16;
	while (p + 4 <= tf_size) {
		unsigned int l = ((unsigned int) tf_blob[p] << 24) |
		    ((unsigned int) tf_blob[p+1] << 16) |
		    ((unsigned int) tf_blob[p+2] << 8) |
		    ((unsigned int) tf_blob[p+3]);
		p += 4;
		if (l == 0 || p + l > tf_size) {
			break;
		}
		if (n == cap) {
			cap *= 2;
			tf_off = (unsigned int *) realloc(tf_off,
			    sizeof(unsigned int) * cap);
			tf_len = (unsigned int *) realloc(tf_len,
			    sizeof(unsigned int) * cap);
			if (tf_off == NULL || tf_len == NULL) {
				return 0;
			}
		}
		tf_off[n] = (unsigned int) p;
		tf_len[n] = l;
		n++;
		p += l;
	}
	tf_count = n;
	tf_pos = 0;
	rfbLog("h264: loaded %d access units from %s, %ux%u (%ld bytes)\n",
	    tf_count, path, tf_w, tf_h, size);
	return tf_count > 0;
}

const unsigned char *h264_testfile_next(unsigned int *len,
    unsigned int *flags) {
	const unsigned char *p;

	if (tf_count <= 0) {
		return NULL;
	}
	if (tf_pos >= tf_count) {
		tf_pos = 0;
	}
	/* restarting the loop replays an IDR, so the client must drop the
	 * contexts built from the previous pass */
	*flags = (tf_pos == 0) ? H264_RESET_ALL_CONTEXTS : 0;
	*len = tf_len[tf_pos];
	p = tf_blob + tf_off[tf_pos];
	tf_pos++;
	return p;
}


/* ------------------------------------------------------------------ *
 * Per-cycle driver
 * ------------------------------------------------------------------ */

static double now_s(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

/*
 * True once Tight has delivered everything it owes every H.264 client.
 * Switching while a backlog is outstanding strands it: the remaining marks are
 * suppressed the moment H.264 claims the output, so a half-painted screen
 * freezes until fresh damage happens to cover it.
 */
static int h264_clients_drained(void) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int drained = 1;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		if (h264_client_active(cl) &&
		    !sraRgnEmpty(cl->modifiedRegion)) {
			drained = 0;
			break;
		}
	}
	rfbReleaseClientIterator(iter);
	return drained;
}

/* How many connected clients actually want encoding 50. */
static int h264_active_clients(void) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int n = 0;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		if (h264_client_active(cl)) {
			n++;
		}
	}
	rfbReleaseClientIterator(iter);
	return n;
}

/*
 * While H.264 owns the served region, Tight must not paint over it - on every
 * cycle, not only the ones that carry an encode. watch_loop runs far faster
 * than the encode rate, so leaving the intervening cycles to Tight sends the
 * whole damaged area twice and costs an order of magnitude in bandwidth.
 */
static void h264_suppress_tight(void) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		if (h264_client_active(cl)) {
			sraRgnMakeEmpty(cl->modifiedRegion);
		}
	}
	rfbReleaseClientIterator(iter);
}

/* Broadcast one access unit to every client that negotiated encoding 50.
 * Called inside watch_loop's send ban, so no locking here - see
 * h264_send_rect(). */
static int h264_broadcast(int w, int h, const unsigned char *au,
    unsigned int len, unsigned int flags, int suppress_tight) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int sent = 0;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		if (!h264_client_active(cl)) {
			continue;
		}
		if (!h264_send_rect(cl, 0, 0, w, h, au, len, flags)) {
			rfbLog("h264: send failed for %s\n",
			    cl->host ? cl->host : "?");
			continue;
		}
		/*
		 * The H.264 rect just repainted the whole served region, so
		 * anything libvncserver still has pending for this client is
		 * both redundant and would overwrite it with Tight.
		 */
		if (suppress_tight) {
			sraRgnMakeEmpty(cl->modifiedRegion);
		}
		sent++;
	}
	rfbReleaseClientIterator(iter);
	return sent;
}

/*
 * Hybrid gate (plan §11): Tight while the screen is static, H.264 once motion
 * is sustained.
 *
 * The metric is dirty area per second expressed in whole screens, which keeps
 * the thresholds independent of resolution and frame rate.  Entry is slow so a
 * transient does not cost text quality; exit is faster and asymmetric so a
 * settled screen goes back to crisp Tight promptly.
 */
int h264_enter_rate = 300;      /* hundredths of a screen/s: 3.00 */
int h264_exit_rate = 75;        /* 0.75 */

static double last = 0.0;
static int in_h264_mode = 0;
static double mode_since = 0.0;
static double above_since = 0.0, below_since = 0.0;
static double clients_since = 0.0;      /* when a client first wanted H.264 */
static int exclusive = 0;               /* every client is on H.264 right now */
static int initial_paint_done = 0;      /* connect-time Tight backlog has drained */

int h264_owns_output(void) {
	return exclusive;
}
static double dirty_ewma = 0.0;
static double last_tick = 0.0;

static void h264_update_gate(int tile_diffs, double t) {
	double dt, frac, rate, alpha;
	int ntiles_total = ntiles_x * ntiles_y;

	dt = (last_tick > 0.0) ? (t - last_tick) : 0.0;
	last_tick = t;
	if (dt <= 0.0 || dt > 1.0 || ntiles_total <= 0) {
		return;                 /* first tick, or a stall - no signal */
	}

	frac = (double) tile_diffs / (double) ntiles_total;
	rate = frac / dt;           /* screens per second */

	/* ~500 ms time constant, expressed so it is independent of tick rate */
	alpha = dt / (0.5 + dt);
	dirty_ewma += alpha * (rate - dirty_ewma);

	if (dirty_ewma * 100.0 >= h264_enter_rate) {
		if (above_since == 0.0) above_since = t;
		below_since = 0.0;
	} else if (dirty_ewma * 100.0 <= h264_exit_rate) {
		if (below_since == 0.0) below_since = t;
		above_since = 0.0;
	}

	/*
	 * Hold Tight briefly after a client arrives.  Connecting paints the
	 * whole screen at once, which reads as enormous motion and flips the
	 * gate immediately - the first image then dribbles in over several
	 * seconds of 30fps encoding instead of arriving in one fast Tight
	 * blast, which is what a viewer expects on connect.
	 */
	if (clients_since > 0.0 && t - clients_since < 2.0) {
		return;
	}

	if (!in_h264_mode) {
		/*
		 * Let the connect-time paint finish before ever claiming the
		 * output.  A full 2560x1440 Tight update takes well over the
		 * grace period on a remote link, and switching mid-paint
		 * strands the remainder - the suppression in scan.c drops the
		 * marks that would have completed it.
		 *
		 * One-shot, not a standing condition: under sustained motion
		 * modifiedRegion is never empty, so requiring it every time
		 * would mean the gate could never engage at all.
		 */
		if (!initial_paint_done) {
			/*
			 * Satisfied by a drain, or by giving up after a few
			 * seconds.  Waiting for the drain alone deadlocks the
			 * gate whenever motion starts before the connect paint
			 * finishes: modifiedRegion is then never empty, so the
			 * latch never sets and H.264 never engages at all.
			 */
			if (h264_clients_drained() ||
			    (clients_since > 0.0 && t - clients_since > 5.0)) {
				initial_paint_done = 1;
			} else {
				above_since = 0.0;
				return;
			}
		}
		if (above_since > 0.0 && t - above_since >= 0.150) {
			in_h264_mode = 1;
			mode_since = t;
			above_since = 0.0;
			rfbLog("h264: motion %.1f screens/s -> H.264\n", dirty_ewma);
		}
	} else {
		if (below_since > 0.0 && t - below_since >= 0.300 &&
		    t - mode_since >= 0.500) {
			in_h264_mode = 0;
			mode_since = t;
			below_since = 0.0;
			rfbLog("h264: quiet %.2f screens/s -> Tight\n", dirty_ewma);
			/*
			 * Drop the claim on the output BEFORE asking for the
			 * repaint.  exclusive is only recomputed after this
			 * function returns, so leaving it set means the guard
			 * in scan.c swallows the very mark we are making here -
			 * the screen then freezes on the last H.264 frame and
			 * only updates where fresh damage happens to land.
			 */
			exclusive = 0;

			/*
			 * The client holds a 4:2:0 decode of the whole region.
			 * Repaint it with Tight so settled text is crisp again.
			 */
			mark_rect_as_modified(0, 0, screen->width,
			    screen->height, 1);
		}
	}
}

/*
 * One tick per watch_loop cycle.  -h264 encodes the served framebuffer live
 * when the gate says the screen is moving; otherwise -h264_testfile replays a
 * known-good stream, which keeps transport problems separable from encoder
 * problems.
 */
void h264_frame_tick(int tile_diffs) {
	const unsigned char *au = NULL;
	unsigned int len = 0, flags = 0;

	if (screen == NULL) {
		return;
	}

	if (h264_enable) {
		static int need_idr = 1;
		double period = (h264_fps > 0) ? 1.0 / h264_fps : 1.0 / 30.0;
		double t = now_s();
		int w = screen->width, h = screen->height;

		/*
		 * Check for a consumer BEFORE encoding, not at broadcast time.
		 * Colour conversion plus encode is ~55% of a core at 2560x1440
		 * @30fps, and spending it on frames nobody has asked for
		 * starves the Tight path badly enough to be visible as jerky
		 * text - which reads as a codec problem and is not one.
		 */
		if (h264_active_clients() == 0) {
			clients_since = 0.0;
			exclusive = 0;
			initial_paint_done = 0;
			if (h264_enc_is_open()) {
				h264_enc_close();
				need_idr = 1;
				in_h264_mode = 0;
				rfbLog("h264: no clients want encoding 50, "
				    "encoder closed\n");
			}
			return;
		}

		if (clients_since == 0.0) {
			clients_since = t;
		}
		h264_update_gate(tile_diffs, t);

		/*
		 * Only claim the output when *every* client is on H.264 - the
		 * suppression in scan.c is global, so a single Tight viewer
		 * sharing the session would otherwise see a frozen screen.
		 */
		{
			int total = 0;
			rfbClientIteratorPtr it = rfbGetClientIterator(screen);
			while (rfbClientIteratorNext(it) != NULL) total++;
			rfbReleaseClientIterator(it);
			exclusive = in_h264_mode && total > 0 &&
			    h264_active_clients() == total;
		}

		if (!in_h264_mode) {
			/*
			 * Static: leave everything to Tight. Do not touch
			 * modifiedRegion - a keystroke must go out as a small
			 * rect immediately, not wait for the next encode tick.
			 */
			need_idr = 1;
			return;
		}

		/* H.264 owns the region for as long as the gate says so */
		h264_suppress_tight();

		if (t - last < period) {
			return;
		}
		if (!h264_enc_is_open()) {
			/*
			 * x11vnc does not poll the screen while no client is
			 * attached (see bench/README.md), so the framebuffer
			 * can be stale or only partly populated here. A
			 * full-frame rect would encode whatever happens to be
			 * in it, and because we clear modifiedRegion below,
			 * the update that would have repaired it is discarded
			 * too - the screen then fills in only where damage
			 * happens to land. Force a full copy first.
			 */
			copy_screen();
			need_idr = 1;
		}
		if (!h264_enc_open(w, h)) {
			return;
		}
		if (getenv("H264_DUMP_FB")) {
			/* one-shot: what does the encoder actually see? */
			static int dumped = 0;
			if (!dumped) {
				FILE *f = fopen("/tmp/h264-fb.ppm", "wb");
				dumped = 1;
				if (f) {
					const unsigned char *fb =
					    (const unsigned char *) screen->frameBuffer;
					int x, y;
					fprintf(f, "P6\n%d %d\n255\n", w, h);
					for (y = 0; y < h; y++) {
						const unsigned char *row = fb +
						    (size_t) y * screen->paddedWidthInBytes;
						for (x = 0; x < w; x++) {
							fputc(row[x*4+2], f);
							fputc(row[x*4+1], f);
							fputc(row[x*4+0], f);
						}
					}
					fclose(f);
					rfbLog("h264: dumped first encoded frame to "
					    "/tmp/h264-fb.ppm\n");
				}
			}
		}
		if (!h264_enc_frame((const unsigned char *) screen->frameBuffer,
		    screen->paddedWidthInBytes, need_idr, &au, &len)) {
			return;
		}
		last = t;
		flags = need_idr ? H264_RESET_CONTEXT : 0;
		need_idr = 0;
		h264_broadcast(w, h, au, len, flags, 1);
		return;
	}

	if (!h264_testfile_loaded()) {
		return;
	}
	au = h264_testfile_next(&len, &flags);
	if (au != NULL) {
		h264_broadcast((int) tf_w, (int) tf_h, au, len, flags, 0);
	}
}
