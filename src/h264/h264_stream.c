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
#include "xwrappers.h"
#include "h264/h264_stream.h"
#include "h264/h264_encode.h"
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One encoded band, ready to go on the wire. */
typedef struct {
	int x, y, w, h;
	const unsigned char *data;
	unsigned int len;
} h264_au_t;

static int h264_fits(rfbClientPtr cl, unsigned int len);
static rfbBool h264_handle_message(rfbClientPtr cl, void *data,
    const rfbClientToServerMsg *msg);
static int h264_last_refused = 0;       /* clients that could not take the last frame */

int h264_force = 0;             /* -h264_force: assume every client wants it */
char *h264_testfile_path = NULL;
int h264_fence = 1;             /* fence flow control on unless -h264_nofence */
int h264_tile_pixels = H264_MAX_TILE_PIXELS;
/*
 * Delivery counters.  Added because the first fence validation inferred the
 * client's frame rate from bytes/s divided by an ASSUMED access-unit size, and
 * inferred liveness from inbound bytes - both wrong.  TigerVNC's out-stream
 * flushes once 1 KB accumulates even while corked, so echoes arrive in batches
 * whether or not its socket loop ever returned to the event pump.  Count the
 * real events instead of deriving them.  echoes/timeouts are incremented from
 * the client input thread; a torn read of a stat is harmless.
 */
static unsigned long st_frames, st_bytes, st_echoes, st_timeouts, st_held, st_norequest;
/* frames encoded straight out of NVFBC's capture buffer rather than main_fb */
static unsigned long st_direct;
/* scan cycles that skipped filling main_fb (plan §24) - compare against the
 * grabs/sec in the NVFBC stats line: materially fewer means a copy survives */
static unsigned long st_skips;
static double st_since = 0.0;
int h264_fence_timeout_ms = 500;
static int extension_registered = 0;

/* ------------------------------------------------------------------ *
 * Per-client state
 * ------------------------------------------------------------------ */

typedef struct {
	int enabled;                    /* client listed encoding 50 */
	int preferred;                  /* ...and listed it ahead of everything else */
	int fence_ok;                   /* client listed pseudo-encoding -312 */
	/*
	 * Fence flow control.  outstanding is set by watch_loop when it sends a
	 * request-fence and cleared by the client's input thread when the echo
	 * arrives, so it is a hand-off between two threads: volatile keeps the
	 * gate loop from caching it.  A uint32 read/write is atomic on the
	 * targets x11vnc runs on, and there is no condition variable to miss -
	 * watch_loop polls the flag every tick - so no lock is needed for this
	 * one signal.  seq pairs a fence with its echo so a late echo from a
	 * previous (timed-out) fence cannot open the gate for the current one.
	 */
	volatile int fence_outstanding;
	uint32_t fence_seq;             /* last request-fence sequence sent */
	double fence_sent_at;           /* when, for the timeout fallback */
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
		st->fence_ok = 0;
		return FALSE;
	}
	if (encoding == RFB_ENCODING_FENCE) {
		/*
		 * The client can carry fences (msg 248).  This is what makes
		 * the flow-control gate safe to arm: without it we would be
		 * sending an unrecognised message and libvncserver would drop
		 * the client.  Claim it so we hear about it, then let the H.264
		 * gate decide when to actually fence.
		 */
		st->fence_ok = 1;
		return TRUE;
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

static int h264_pseudo_encodings[] = { RFB_ENCODING_H264, RFB_ENCODING_FENCE, 0 };

static rfbProtocolExtension h264_extension = {
	h264_new_client,        /* newClient */
	NULL,                   /* init */
	h264_pseudo_encodings,  /* pseudoEncodings */
	h264_enable_pseudo,     /* enablePseudoEncoding */
	h264_handle_message,    /* handleMessage: catches the fence echo (msg 248) */
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
	 * Refuse rather than block.  The caller forces an IDR after a refusal,
	 * because skipping an access unit breaks P-frame prediction.
	 */
	if (!h264_fits(cl, len)) {
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

/*
 * Emit one FramebufferUpdate carrying every tile as its own encoding-50 rect.
 *
 * Why several rects rather than one full-screen rect: a rect larger than the
 * client's decode buffer is silently never displayed (h264_stream.h). Each
 * tile is a FIXED geometry, so it maps to one stable decoder context - rects
 * are looked up by exact geometry (isEqualRect) and capped at 64, so a handful
 * of constant bands is nothing like the churn that made §3 reject
 * damage-driven rects.
 *
 * Same locking contract as h264_send_rect(): the caller holds the send ban.
 */
static int h264_send_tiles(rfbClientPtr cl, const h264_au_t *aus, int n,
    unsigned int flags) {
	rfbFramebufferUpdateMsg fu;
	int i;

	if (cl == NULL || n <= 0) {
		return 0;
	}
	{
		/*
		 * Check the WHOLE update against the send buffer, not each tile
		 * separately: they go out back to back with no chance to drain
		 * in between, so per-tile checks would each pass against a queue
		 * the previous tile is about to fill.
		 */
		unsigned int total = 0;
		for (i = 0; i < n; i++) {
			if (aus[i].data == NULL) {
				return 0;
			}
			total += aus[i].len + 20;       /* rect + sub-header */
		}
		if (!h264_fits(cl, total)) {
			return 0;
		}
	}

	/* start on a message boundary - anything buffered is a different update */
	if (cl->ublen > 0 && !rfbSendUpdateBuf(cl)) {
		return 0;
	}

	memset(&fu, 0, sizeof(fu));
	fu.type = rfbFramebufferUpdate;
	fu.nRects = Swap16IfLE((uint16_t) n);
	memcpy(&cl->updateBuf[cl->ublen], &fu, sz_rfbFramebufferUpdateMsg);
	cl->ublen += sz_rfbFramebufferUpdateMsg;

	for (i = 0; i < n; i++) {
		rfbFramebufferUpdateRectHeader rect;
		unsigned char sub[8];
		unsigned int len = aus[i].len;

		rect.r.x = Swap16IfLE(aus[i].x);
		rect.r.y = Swap16IfLE(aus[i].y);
		rect.r.w = Swap16IfLE(aus[i].w);
		rect.r.h = Swap16IfLE(aus[i].h);
		rect.encoding = Swap32IfLE(RFB_ENCODING_H264);
		memcpy(&cl->updateBuf[cl->ublen], &rect,
		    sz_rfbFramebufferUpdateRectHeader);
		cl->ublen += sz_rfbFramebufferUpdateRectHeader;

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

		/* headers out, then the payload straight to the socket */
		if (!rfbSendUpdateBuf(cl)) {
			return 0;
		}
		if (len > 0 && rfbWriteExact(cl, (const char *) aus[i].data,
		    (int) len) < 0) {
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * Fence flow control (plan §17)
 * ------------------------------------------------------------------ */

/*
 * Emit one RFB fence (message 248).  Same discipline as h264_send_rect: the
 * caller holds the send ban, so no lock here, and anything libvncserver already
 * buffered is flushed first so ours starts on a message boundary.  The whole
 * message is at most 12 + 64 bytes, well under UPDATE_BUF_SIZE.
 */
static int h264_send_fence(rfbClientPtr cl, uint32_t flags,
    const unsigned char *payload, int len) {
	unsigned char *b;

	if (cl == NULL || len < 0 || len > 64) {
		return 0;
	}
	if (cl->ublen > 0 && !rfbSendUpdateBuf(cl)) {
		return 0;
	}
	b = (unsigned char *) &cl->updateBuf[cl->ublen];
	b[0] = RFB_MSG_FENCE;
	b[1] = b[2] = b[3] = 0;                 /* padding */
	b[4] = (unsigned char) (flags >> 24);
	b[5] = (unsigned char) (flags >> 16);
	b[6] = (unsigned char) (flags >> 8);
	b[7] = (unsigned char) (flags);
	b[8] = (unsigned char) len;
	if (len > 0) {
		memcpy(b + 9, payload, (size_t) len);
	}
	cl->ublen += 9 + len;
	return rfbSendUpdateBuf(cl) ? 1 : 0;
}

/*
 * Client -> server message hook.  libvncserver hands us any message type it
 * does not recognise, having read only the one type byte; we must consume the
 * rest of the body or the input stream desyncs, and we must return TRUE or the
 * library closes the client (rfbserver.c, the default case).  This runs on the
 * client's input thread, not watch_loop.
 *
 * A fence with fenceFlagRequest is the peer asking us to echo (a viewer rarely
 * does this, but be correct); anything else is the echo of a fence we sent, and
 * clears the gate for that client if the sequence matches.
 */
static rfbBool h264_handle_message(rfbClientPtr cl, void *data,
    const rfbClientToServerMsg *msg) {
	h264_client_t *st = (h264_client_t *) data;
	unsigned char hdr[8];
	unsigned char payload[64];
	uint32_t flags;
	int len;

	if (msg->type != RFB_MSG_FENCE) {
		return FALSE;           /* not ours - let the library handle it */
	}

	/* body after the type byte: pad[3], flags(u32 be), len(u8), data[len] */
	if (rfbReadExact(cl, (char *) hdr, 8) <= 0) {
		return TRUE;            /* connection is gone; we still "handled" it */
	}
	flags = ((uint32_t) hdr[3] << 24) | ((uint32_t) hdr[4] << 16) |
	        ((uint32_t) hdr[5] << 8) | ((uint32_t) hdr[6]);
	len = hdr[7];
	if (len > 64) {
		return TRUE;            /* malformed; drop it rather than desync */
	}
	if (len > 0 && rfbReadExact(cl, (char *) payload, len) <= 0) {
		return TRUE;
	}

	if (flags & FENCE_FLAG_REQUEST) {
		/* Peer wants an echo.  Mirror it, minus the request bit, under
		 * the send ban we do not hold here - but a fence is tiny and
		 * this path is not the H.264 hot path, so take the client's
		 * send mutex explicitly, exactly as libvncserver's own writers
		 * do outside the scan section. */
		LOCK(cl->sendMutex);
		h264_send_fence(cl, flags & (FENCE_FLAG_BLOCK_BEFORE |
		    FENCE_FLAG_BLOCK_AFTER), payload, len);
		UNLOCK(cl->sendMutex);
		return TRUE;
	}

	/* An echo.  Match the sequence so a late echo from a timed-out fence
	 * cannot open the gate for the current one. */
	if (st != NULL && len >= 4) {
		uint32_t seq = ((uint32_t) payload[0] << 24) |
		    ((uint32_t) payload[1] << 16) |
		    ((uint32_t) payload[2] << 8) | ((uint32_t) payload[3]);
		if (seq == st->fence_seq) {
			st->fence_outstanding = 0;
			st_echoes++;
		}
	}
	return TRUE;
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
 * Unsent bytes still queued on a client's socket.
 *
 * h264_send_rect() writes with rfbWriteExact(), which loops until every byte
 * is gone, and it runs inside watch_loop's send ban holding every client's
 * sendMutex.  So a client that cannot drain the stream does not just fall
 * behind - it stalls the whole server, and because H.264 mode suppresses Tight
 * there is nothing else to paint with.  The session freezes with the
 * connection still up and recovers only when the backlog clears.
 *
 * Checking the queue before encoding turns that into dropped frames instead.
 */
static int h264_backlog(rfbClientPtr cl) {
	int q = 0;

	if (cl == NULL || cl->sock < 0) {
		return 0;
	}
	if (ioctl(cl->sock, SIOCOUTQ, &q) != 0) {
		return 0;               /* cannot tell; assume clear */
	}
	return q;
}

/* Largest backlog we will add another access unit on top of. */
#define H264_BACKLOG_LIMIT (512 * 1024)

/*
 * Will this whole access unit fit in the socket's remaining send buffer?
 *
 * Checking the backlog alone is not enough: rfbWriteExact() loops until every
 * byte is written, so starting a 100 KB write with only 20 KB of room blocks
 * watch_loop anyway.  That is not merely a stall - x11vnc processes signals
 * from the main loop, so a blocked write also means SIGTERM is never seen and
 * `systemctl restart` sits there until its stop timeout expires.
 *
 * Linux reports SO_SNDBUF as roughly twice the usable size, hence the halving.
 */
static int h264_fits(rfbClientPtr cl, unsigned int len) {
	int q = 0, sndbuf = 0;
	socklen_t sl = sizeof(sndbuf);

	if (cl == NULL || cl->sock < 0) {
		return 0;
	}
	if (ioctl(cl->sock, SIOCOUTQ, &q) != 0 ||
	    getsockopt(cl->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) != 0) {
		return 1;               /* cannot tell; let it through */
	}
	return (q + (int) len + 256) < (sndbuf / 2);
}

/* True if any H.264 client is too far behind to take another frame. */
static int h264_clients_backed_up(void) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int busy = 0;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		if (h264_client_active(cl) &&
		    h264_backlog(cl) > H264_BACKLOG_LIMIT) {
			busy = 1;
			break;
		}
	}
	rfbReleaseClientIterator(iter);
	return busy;
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

/*
 * True while every active client is still waiting to ack the previous frame
 * (fence outstanding, not yet timed out).  The tick uses this to HOLD before
 * encoding rather than encode-and-drop: a dropped frame would advance the
 * encoder past what the client has, so the next send would have to be an IDR,
 * and under sustained fencing that turns every delivered frame into a ~4x
 * larger IDR - the opposite of what the pacing is for.  Holding keeps the
 * stream as clean P-frames, one per ack.  A timed-out fence does not count as
 * holding, so a lost echo still resolves via the send-anyway path in broadcast.
 */
static int h264_fence_holding(double t) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int any_active = 0, all_held = 1;

	if (!h264_fence) {
		return 0;
	}
	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		h264_client_t *st;
		if (!h264_client_active(cl)) {
			continue;
		}
		any_active = 1;
		st = (h264_client_t *) rfbGetExtensionClientData(cl,
		    &h264_extension);
		if (st == NULL || !st->fence_ok) {
			all_held = 0;           /* an un-fenced client can take it */
			break;
		}
		if (!st->fence_outstanding ||
		    (t - st->fence_sent_at) * 1000.0 >= (double) h264_fence_timeout_ms) {
			all_held = 0;           /* this one is ready (or timed out) */
			break;
		}
	}
	rfbReleaseClientIterator(iter);
	return any_active && all_held;
}

/*
 * Clear every client's fence gate.  Called when H.264 hands back to Tight so a
 * later re-entry starts un-gated instead of waiting out the timeout on a fence
 * whose echo is no longer coming.
 */
static void h264_reset_fences(void) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		h264_client_t *st = (h264_client_t *)
		    rfbGetExtensionClientData(cl, &h264_extension);
		if (st != NULL) {
			st->fence_outstanding = 0;
		}
	}
	rfbReleaseClientIterator(iter);
}

/* Broadcast one access unit to every client that negotiated encoding 50.
 * Called inside watch_loop's send ban, so no locking here - see
 * h264_send_rect(). */
static int h264_broadcast(const h264_au_t *aus, int ntiles,
    unsigned int flags, int suppress_tight) {
	rfbClientIteratorPtr iter;
	rfbClientPtr cl;
	int sent = 0, refused = 0, i;
	unsigned long total = 0;

	for (i = 0; i < ntiles; i++) {
		total += aus[i].len;
	}

	iter = rfbGetClientIterator(screen);
	while ((cl = rfbClientIteratorNext(iter)) != NULL) {
		h264_client_t *st;
		int use_fence;

		if (!h264_client_active(cl)) {
			continue;
		}
		st = (h264_client_t *) rfbGetExtensionClientData(cl,
		    &h264_extension);
		use_fence = h264_fence && st != NULL && st->fence_ok;

		/*
		 * Fence gate: do not send another frame until the client has
		 * echoed the fence that followed the last one.  This is the
		 * real backpressure the socket queue and the update request
		 * cannot give us (plan §17): the echo is proof the client's
		 * single-threaded socket loop drained the previous frame and
		 * came back for more, so between frames its loop is guaranteed
		 * to return to the event pump and service input and redraw.
		 *
		 * The timeout keeps a lost echo from freezing the stream the
		 * other way: past it, send anyway and re-arm with a fresh fence.
		 */
		if (use_fence && st->fence_outstanding) {
			double waited = (now_s() - st->fence_sent_at) * 1000.0;
			if (waited < (double) h264_fence_timeout_ms) {
				refused++;
				continue;
			}
			st->fence_outstanding = 0;      /* give up on this one */
			st_timeouts++;
		}

		/*
		 * Only send if the client has actually asked for an update.
		 *
		 * This is RFB's flow control and libvncserver's own encoders
		 * obey it; pushing frames on a timer regardless does not just
		 * waste bandwidth, it removes the only signal a slow client
		 * has to say "ease off".  A viewer decoding 2560x1440 H.264
		 * in software falls behind under sustained full-screen change,
		 * stops requesting, and the frames it never asked for pile up
		 * in the tunnel - so the picture it shows is a minute stale
		 * while the server looks perfectly healthy.  The socket queue
		 * shows nothing either, because sshd drains it eagerly into
		 * buffers of its own.
		 */
		if (sraRgnEmpty(cl->requestedRegion)) {
			refused++;
			st_norequest++;
			continue;
		}
		if (!h264_send_tiles(cl, aus, ntiles, flags)) {
			/*
			 * Either the socket could not take the whole unit or
			 * the write failed.  Do not clear modifiedRegion:
			 * this client did not get the frame, so Tight must
			 * still be allowed to paint for it.
			 */
			refused++;
			continue;
		}
		/*
		 * Chase the frame with a request-fence.  It rides the same
		 * send ban and the same socket, in order, so the client sees
		 * [frame][fence] and echoes the fence only after decoding the
		 * frame.  Arm the gate; the echo (or the timeout) reopens it.
		 */
		if (use_fence) {
			unsigned char seqbuf[4];
			uint32_t seq = ++st->fence_seq;
			seqbuf[0] = (unsigned char) (seq >> 24);
			seqbuf[1] = (unsigned char) (seq >> 16);
			seqbuf[2] = (unsigned char) (seq >> 8);
			seqbuf[3] = (unsigned char) (seq);
			st->fence_sent_at = now_s();
			st->fence_outstanding = 1;
			if (!h264_send_fence(cl, FENCE_FLAG_REQUEST, seqbuf, 4)) {
				/* write failed: do not strand the gate on it */
				st->fence_outstanding = 0;
			}
		}
		/*
		 * The H.264 rect just repainted the whole served region, so
		 * anything libvncserver still has pending for this client is
		 * both redundant and would overwrite it with Tight.
		 */
		if (suppress_tight) {
			sraRgnMakeEmpty(cl->modifiedRegion);
		}
		/* the request is now satisfied, as libvncserver does after an update */
		sraRgnMakeEmpty(cl->requestedRegion);
		st_frames++;
		st_bytes += total;
		sent++;
	}
	rfbReleaseClientIterator(iter);
	h264_last_refused = refused;
	return sent;
}

/* ------------------------------------------------------------------ *
 * Tiling (plan §22)
 * ------------------------------------------------------------------ */

static struct {
	int y, h;               /* band position in the served region */
	h264_enc_t *enc;
} h264_tiles[H264_MAX_TILES];
static int h264_ntiles = 0;
static int tiles_w = 0, tiles_h = 0;

static void h264_tiles_close(void) {
	int i;
	for (i = 0; i < h264_ntiles; i++) {
		h264_enc_close(&h264_tiles[i].enc);
	}
	h264_ntiles = 0;
	tiles_w = tiles_h = 0;
}

static int h264_tiles_open(int w, int h) {
	int want, band, i, y;

	if (h264_ntiles > 0 && w == tiles_w && h == tiles_h) {
		return 1;               /* already laid out for this geometry */
	}
	h264_tiles_close();
	if (w <= 0 || h <= 0) {
		return 0;
	}

	/*
	 * As few bands as keep every one inside the client's decode buffer.
	 * Band height is rounded UP to a multiple of 16 so each tile is a whole
	 * number of macroblock rows - an odd or unaligned height would need
	 * cropping in the SPS, and TigerVNC applies frame cropping through
	 * offset_x/offset_y when it blits, which is a needless place to be
	 * subtly wrong.  The last band takes the remainder.
	 */
	want = 1;
	if (h264_tile_pixels > 0) {
		while (want < H264_MAX_TILES &&
		    (double) w * h / want > (double) h264_tile_pixels) {
			want++;
		}
	}
	band = ((h + want - 1) / want + 15) / 16 * 16;
	if (band < 16) {
		band = 16;
	}

	y = 0;
	for (i = 0; i < want && y < h; i++) {
		int bh = (y + band <= h) ? band : (h - y);
		if (bh & 1) {
			bh++;           /* H.264 needs even dimensions */
		}
		if (y + bh > h) {
			bh = h - y;     /* cannot exceed the region */
		}
		h264_tiles[i].y = y;
		h264_tiles[i].h = bh;
		h264_tiles[i].enc = h264_enc_open(w, bh);
		if (h264_tiles[i].enc == NULL) {
			h264_ntiles = i;
			h264_tiles_close();
			return 0;
		}
		y += bh;
		h264_ntiles = i + 1;
	}
	if (y != h) {
		rfbLog("h264: tiling %dx%d left %d rows uncovered - refusing\n",
		    w, h, h - y);
		h264_tiles_close();
		return 0;
	}

	tiles_w = w;
	tiles_h = h;
	rfbLog("h264: %d tile(s) of %dx%d for %dx%d "
	    "(client limit %d px per rect)\n",
	    h264_ntiles, w, h264_tiles[0].h, w, h, h264_tile_pixels);
	return 1;
}

static int h264_tiles_open_any(void) {
	return h264_ntiles > 0;
}

void h264_encoders_reset(void) {
	if (h264_ntiles > 0) {
		h264_tiles_close();
	}
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
static double stalled_since = 0.0;      /* when the client first fell behind */

int h264_owns_output(void) {
	return exclusive;
}

/*
 * ------------------------------------------------------------------
 * Where the pixels come from (plan §24, "Phase 3'")
 * ------------------------------------------------------------------
 *
 * While H.264 owns every client, nothing reads main_fb: libvncserver is told
 * about no damage at all, so the only consumer of those pixels is this
 * encoder.  Filling it costs two copies per dirty tile - NVFBC's buffer into
 * an XImage, then that XImage into main_fb - which at 2560x1440 is 2 x 14.7 MB
 * per captured frame, measured as the bulk of the residual CPU.  So scan.c
 * skips them and the encoder reads NVFBC's buffer where it already is.
 *
 * The price is that main_fb goes stale for as long as this holds.  Every path
 * out of H.264 mode therefore goes through h264_release_output(), which
 * refills it before Tight is asked to repaint - otherwise Tight faithfully
 * repaints the image from the moment the gate engaged.
 */
static int fb_stale = 0;        /* main_fb has not been refilled since the skip */

/*
 * NVFBC's capture buffer, but only when it can stand in for main_fb exactly:
 * same geometry, and no scaling or colour transformation in between.  NULL
 * otherwise, and then everything falls back to the framebuffer.
 */
static const unsigned char *h264_nvfbc_pixels(int *stride) {
#if HAVE_NVFBC
	if (screen == NULL || rfb_fb != main_fb ||
	    screen->width != dpy_x || screen->height != dpy_y) {
		return NULL;
	}
	return (const unsigned char *) nvfbc_served_pixels(stride);
#else
	(void) stride;
	return NULL;
#endif
}

/*
 * Asked by scan.c once per cycle, before it would do the copies.
 * h264_fb_copy_skipped() records that it acted on the answer, which is what
 * tells the exit path main_fb needs refilling.
 */
int h264_fb_copy_skippable(void) {
	int stride = 0;

	return h264_enable && exclusive &&
	    h264_nvfbc_pixels(&stride) != NULL;
}

void h264_fb_copy_skipped(void) {
	fb_stale = 1;
	st_skips++;
}

/*
 * This tick's source.
 *
 * scan.c decided whether to skip the copies earlier in the same watch_loop
 * iteration; in principle the two decisions can disagree (a grab that fails in
 * between, say), so falling back to the framebuffer has to repair it first.
 *
 * LIFETIME: NVFBC's buffer is overwritten by the next grab, which happens at
 * the top of the next scan_for_updates().  This runs after this cycle's grab
 * and inside watch_loop's send ban, so the frame is ours for the duration.
 */
static const unsigned char *h264_frame_source(int *stride) {
	int s = 0;
	const unsigned char *p = h264_nvfbc_pixels(&s);

	if (p != NULL) {
		*stride = s;
		st_direct++;
		return p;
	}
	if (fb_stale) {
		fb_stale = 0;
		copy_screen();
	}
	*stride = screen->paddedWidthInBytes;
	return (const unsigned char *) screen->frameBuffer;
}

/*
 * Hand the output back to Tight.
 *
 * Order matters twice over.  `exclusive` has to drop first, because
 * mark_rect_as_modified() is suppressed while H.264 owns the output and would
 * otherwise swallow the very repaint being asked for here (§13's bug).  And
 * main_fb has to be refilled before the mark, because Tight will repaint
 * exactly what is in it - which, after a period of skipped copies, is the
 * screen as it was when the gate engaged.
 *
 * Idempotent: fb_stale is what limits the refill to once per H.264 period.
 */
static void h264_release_output(void) {
	exclusive = 0;
	h264_reset_fences();
	if (fb_stale) {
		fb_stale = 0;
		copy_screen();
	}
	mark_rect_as_modified(0, 0, screen->width, screen->height, 1);
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
			 * The client holds a 4:2:0 decode of the whole region;
			 * repaint it with Tight so settled text is crisp again.
			 * h264_release_output() drops the claim first (the
			 * guard in scan.c would otherwise swallow this very
			 * mark) and refills main_fb first (the copies have been
			 * skipped for the whole H.264 period).
			 */
			h264_release_output();
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
			initial_paint_done = 0;
			stalled_since = 0.0;
			/*
			 * The last H.264 client leaving is an exit like any
			 * other: main_fb is stale, and the next viewer to
			 * connect would be served that stale image by Tight.
			 */
			if (exclusive || fb_stale) {
				h264_release_output();
			}
			if (h264_tiles_open_any()) {
				h264_tiles_close();
				need_idr = 1;
				in_h264_mode = 0;
				rfbLog("h264: no clients want encoding 50, "
				    "encoders closed\n");
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
			int total = 0, was = exclusive;
			rfbClientIteratorPtr it = rfbGetClientIterator(screen);
			while (rfbClientIteratorNext(it) != NULL) total++;
			rfbReleaseClientIterator(it);
			exclusive = in_h264_mode && total > 0 &&
			    h264_active_clients() == total;
			/*
			 * The claim can also drop without the gate exiting - a
			 * second, Tight-only viewer joining the session is
			 * enough.  That viewer would be served a main_fb the
			 * scan stopped filling, so treat it as an exit too.
			 */
			if (was && !exclusive) {
				h264_release_output();
			}
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

		/*
		 * Back off before encoding if the client is behind.  Dropping
		 * a frame breaks P-frame prediction, so whatever we send next
		 * has to be an IDR.
		 */
		if (h264_clients_backed_up()) {
			if (stalled_since == 0.0) {
				stalled_since = t;
			}
			need_idr = 1;
			/*
			 * Persistently behind means H.264 is simply too much
			 * for this link right now.  Hand back to Tight, which
			 * libvncserver paces properly, rather than keep the
			 * screen frozen.
			 */
			if (t - stalled_since > 2.0) {
				rfbLog("h264: client backed up for %.1fs, "
				    "falling back to Tight\n", t - stalled_since);
				in_h264_mode = 0;
				stalled_since = 0.0;
				h264_release_output();
			}
			return;
		}
		stalled_since = 0.0;    /* caught up */

		/* H.264 owns the region for as long as the gate says so */
		h264_suppress_tight();

		/*
		 * Periodic delivery report.  fps here is FRAMES THE CLIENT
		 * ACKNOWLEDGED, not frames offered: with fences on, echoes
		 * should track frames one-for-one, and timeouts should be 0.
		 * A high timeout count means the client is not keeping up and
		 * the gate is running on the fallback rather than on acks.
		 */
		if (st_since == 0.0) {
			st_since = t;
		} else if (t - st_since >= 10.0) {
			double dt2 = t - st_since;
			rfbLog("h264 stats: %.1f fps sent, %.1f MB/s, "
			    "%lu echoes, %lu timeouts, %lu held, %lu unrequested, "
			    "%lu direct, %.0f fb-skips/s\n",
			    st_frames / dt2, st_bytes / dt2 / 1048576.0,
			    st_echoes, st_timeouts, st_held, st_norequest,
			    st_direct, st_skips / dt2);
			st_frames = st_bytes = st_echoes = 0;
			st_timeouts = st_held = st_norequest = st_direct = 0;
			st_skips = 0;
			st_since = t;
		}

		if (t - last < period) {
			return;
		}

		/*
		 * Fence flow control: if every client still owes an ack for the
		 * last frame, hold here instead of encoding one they cannot yet
		 * take.  Holding (rather than encode-and-drop) is what keeps the
		 * stream as P-frames - see h264_fence_holding().  The encoder is
		 * not advanced and need_idr is left alone, so the next frame,
		 * once an ack arrives, predicts cleanly from the last one sent.
		 */
		if (h264_fence_holding(t)) {
			st_held++;
			return;
		}
		if (!h264_tiles_open_any()) {
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
			fb_stale = 0;   /* just refilled */
			need_idr = 1;
		}
		if (!h264_tiles_open(w, h)) {
			return;
		}
		if (getenv("H264_DUMP_FB")) {
			/*
			 * One-shot: what does the encoder actually see?  It
			 * has to read whichever source the encode below will,
			 * or it answers a question nobody asked - this is the
			 * probe that settles cursor and geometry arguments.
			 * Read-only, so no st_direct and no copy_screen().
			 */
			static int dumped = 0;
			if (!dumped) {
				FILE *f = fopen("/tmp/h264-fb.ppm", "wb");
				dumped = 1;
				if (f) {
					int dstride = 0;
					const unsigned char *fb =
					    h264_nvfbc_pixels(&dstride);
					const char *what = "nvfbc";
					int x, y;
					if (fb == NULL) {
						fb = (const unsigned char *)
						    screen->frameBuffer;
						dstride = screen->paddedWidthInBytes;
						what = "main_fb";
					}
					fprintf(f, "P6\n%d %d\n255\n", w, h);
					for (y = 0; y < h; y++) {
						const unsigned char *row = fb +
						    (size_t) y * dstride;
						for (x = 0; x < w; x++) {
							fputc(row[x*4+2], f);
							fputc(row[x*4+1], f);
							fputc(row[x*4+0], f);
						}
					}
					fclose(f);
					rfbLog("h264: dumped first encoded frame "
					    "(from %s) to /tmp/h264-fb.ppm\n", what);
				}
			}
		}
		{
			h264_au_t tiles[H264_MAX_TILES];
			const unsigned char *fb;
			int stride;
			int i, ok = 1;

			/*
			 * Encode each band from the source in place: rows are
			 * contiguous, so a horizontal band is just an offset
			 * pointer with the same stride.  That keeps the
			 * zero-copy property of the single-rect path - no
			 * allocation, no copy, and it is why the region is
			 * split into bands rather than columns.
			 *
			 * The source is NVFBC's capture buffer whenever it is
			 * usable, and main_fb otherwise; the band layout is
			 * identical either way because the fork already
			 * captures exactly the served region.
			 */
			fb = h264_frame_source(&stride);
			for (i = 0; i < h264_ntiles; i++) {
				const unsigned char *p = fb +
				    (size_t) h264_tiles[i].y * stride;
				if (!h264_enc_frame(h264_tiles[i].enc, p,
				    stride, need_idr, &tiles[i].data,
				    &tiles[i].len)) {
					ok = 0;
					break;
				}
				tiles[i].x = 0;
				tiles[i].y = h264_tiles[i].y;
				tiles[i].w = w;
				tiles[i].h = h264_tiles[i].h;
			}
			if (!ok) {
				/*
				 * A partial frame cannot be sent: the tiles
				 * that did encode have advanced their
				 * reference chains past what the client holds,
				 * so the next frame must re-key everything.
				 */
				need_idr = 1;
				return;
			}
			last = t;
			flags = need_idr ? H264_RESET_CONTEXT : 0;
			need_idr = 0;
			h264_broadcast(tiles, h264_ntiles, flags, 1);
			if (h264_last_refused > 0) {
				/* a refused unit breaks prediction */
				need_idr = 1;
			}
		}
		return;
	}

	if (!h264_testfile_loaded()) {
		return;
	}
	au = h264_testfile_next(&len, &flags);
	if (au != NULL) {
		h264_au_t one;
		one.x = one.y = 0;
		one.w = (int) tf_w;
		one.h = (int) tf_h;
		one.data = au;
		one.len = len;
		h264_broadcast(&one, 1, flags, 0);
	}
}
