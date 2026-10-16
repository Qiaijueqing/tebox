/*
 * tebox (local, not for upstream): host video encoder for the guest Codec2
 * HAL on macOS. Linux has tebox-cast venc (V4L2) for this; on macOS QEMU
 * itself is the client of the guest's tebox.venc port ($TEBOX_VENC_SOCK) and
 * encodes on VideoToolbox. Each frame is a gfxstream ColorBuffer that
 * gfxstream blits on the GPU into a BGRA IOSurface from the encoder's pool
 * (stream_renderer_tebox_read_iosurface); VideoToolbox encodes that
 * IOSurface, so no pixel touches the CPU.
 *
 * Protocol: CAST_VENC_* in src/service/cast/include/cast/protocol.h, mirrored
 * here. One reply per frame, in order: a reader thread encodes each frame and
 * waits for VideoToolbox (real time, no frame reordering) before replying.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

void tebox_vt_venc_start(void);

#define VENC_MAGIC 0x434e4556u /* 'VENC' */

enum {
    VENC_CONFIG = 1,
    VENC_FRAME = 2,
    VENC_CLOSE = 3,
    VENC_BITSTREAM = 16,
};

#define VENC_FLAG_KEY 0x01u

struct venc_hdr {
    uint32_t magic, type, size;
} QEMU_PACKED;

struct venc_config {
    uint32_t session, width, height, bitrate, fps, gop_sec;
} QEMU_PACKED;

struct venc_frame {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    uint32_t resource_id, width, height, format, flags;
} QEMU_PACKED;

struct venc_out {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags;
} QEMU_PACKED;

#define MAX_SESSIONS 8
#define MAX_MESSAGE (1u << 20)
#define ENCODE_TIMEOUT_NS (2 * 1000000000LL)

typedef int (*read_fn)(uint32_t res_handle, void *io_surface);

struct vt_session {
    bool used;
    uint32_t id;
    uint64_t last_use;
    struct venc_config cfg;
    VTCompressionSessionRef vt;
    /* The frame in flight; the output callback fills these. */
    QemuMutex lock;
    QemuCond done_cond;
    bool done;
    int32_t status;
    bool key;
    uint8_t *data;
    size_t size;
};

static struct {
    int fd;
    read_fn read;
    uint64_t clock;
    struct vt_session sessions[MAX_SESSIONS];
} vt = { .fd = -1 };

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;

    while (n) {
        ssize_t r = read(fd, p, n);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        ssize_t r = write(fd, p, n);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void send_out(const struct venc_frame *f, int32_t status, uint32_t flags,
                     const uint8_t *data, size_t size)
{
    struct venc_hdr hdr = { VENC_MAGIC, VENC_BITSTREAM, (uint32_t)(sizeof(struct venc_out) + size) };
    struct venc_out out = { f->session, f->index, f->pts_us, status, flags };

    if (!write_full(vt.fd, &hdr, sizeof(hdr)) || !write_full(vt.fd, &out, sizeof(out)) ||
        (size && !write_full(vt.fd, data, size))) {
        error_report_once("tebox-vt-venc: write to the guest failed: %s", strerror(errno));
    }
}

/* ---- VideoToolbox ---- */

/* Appends |size| bytes of a 4-byte length-prefixed (AVCC) stream as Annex-B. */
static void append_annexb(GByteArray *out, const uint8_t *avcc, size_t size)
{
    static const uint8_t start[4] = { 0, 0, 0, 1 };
    size_t pos = 0;

    while (pos + 4 <= size) {
        uint32_t len = ((uint32_t)avcc[pos] << 24) | ((uint32_t)avcc[pos + 1] << 16) |
                       ((uint32_t)avcc[pos + 2] << 8) | avcc[pos + 3];

        pos += 4;
        if (len > size - pos) {
            break;
        }
        g_byte_array_append(out, start, sizeof(start));
        g_byte_array_append(out, avcc + pos, len);
        pos += len;
    }
}

static void encoded(void *opaque, void *frame, OSStatus status, VTEncodeInfoFlags info,
                    CMSampleBufferRef sample)
{
    struct vt_session *s = opaque;
    GByteArray *out = g_byte_array_new();
    bool key = false;

    if (status == noErr && sample && CMSampleBufferDataIsReady(sample)) {
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        size_t total = block ? CMBlockBufferGetDataLength(block) : 0;

        key = true;
        if (attachments && CFArrayGetCount(attachments) > 0) {
            CFDictionaryRef a = CFArrayGetValueAtIndex(attachments, 0);

            key = !CFDictionaryContainsKey(a, kCMSampleAttachmentKey_NotSync);
        }
        if (key) {
            /* Annex-B with SPS/PPS before each IDR. */
            CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
            size_t count = 0;
            static const uint8_t start[4] = { 0, 0, 0, 1 };

            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL, &count, NULL);
            for (size_t i = 0; i < count; i++) {
                const uint8_t *ps;
                size_t len;

                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &len, NULL,
                                                                       NULL) == noErr) {
                    g_byte_array_append(out, start, sizeof(start));
                    g_byte_array_append(out, ps, len);
                }
            }
        }
        if (total) {
            uint8_t *avcc = g_malloc(total);

            if (CMBlockBufferCopyDataBytes(block, 0, total, avcc) == kCMBlockBufferNoErr) {
                append_annexb(out, avcc, total);
            }
            g_free(avcc);
        }
    }
    qemu_mutex_lock(&s->lock);
    g_free(s->data);
    s->size = out->len;
    s->data = (uint8_t *)g_byte_array_free(out, false);
    s->key = key;
    s->status = status == noErr ? 0 : -EIO;
    s->done = true;
    qemu_cond_signal(&s->done_cond);
    qemu_mutex_unlock(&s->lock);
}

static void set_number(VTCompressionSessionRef session, CFStringRef key, int32_t value)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);

    VTSessionSetProperty(session, key, n);
    CFRelease(n);
}

static void destroy_vt(struct vt_session *s)
{
    if (s->vt) {
        VTCompressionSessionInvalidate(s->vt);
        CFRelease(s->vt);
        s->vt = NULL;
    }
}

static bool create_vt(struct vt_session *s)
{
    const struct venc_config *c = &s->cfg;
    const int32_t bgra = kCVPixelFormatType_32BGRA;
    const int32_t w = (int32_t)c->width, h = (int32_t)c->height;
    CFNumberRef pixel_format = CFNumberCreate(NULL, kCFNumberSInt32Type, &bgra);
    CFNumberRef width = CFNumberCreate(NULL, kCFNumberSInt32Type, &w);
    CFNumberRef height = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFDictionaryRef iosurface = CFDictionaryCreate(NULL, NULL, NULL, 0,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey,
                           kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey,
                           kCVPixelBufferMetalCompatibilityKey };
    const void *values[] = { pixel_format, width, height, iosurface, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *spec_keys[] = {
        kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder };
    const void *spec_values[] = { kCFBooleanTrue };
    CFDictionaryRef spec = CFDictionaryCreate(NULL, spec_keys, spec_values, 1,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    OSStatus err = VTCompressionSessionCreate(kCFAllocatorDefault, w, h, kCMVideoCodecType_H264,
                                              spec, attrs, NULL, encoded, s, &s->vt);

    CFRelease(spec);
    CFRelease(attrs);
    CFRelease(iosurface);
    CFRelease(height);
    CFRelease(width);
    CFRelease(pixel_format);
    if (err != noErr) {
        warn_report("tebox-vt-venc: no hardware H.264 encoder for %dx%d (%d)", w, h, (int)err);
        s->vt = NULL;
        return false;
    }
    /* One output per input, right away: the guest expects a reply per frame. */
    VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_High_AutoLevel);
    set_number(s->vt, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
    if (c->bitrate) {
        set_number(s->vt, kVTCompressionPropertyKey_AverageBitRate, (int32_t)c->bitrate);
    }
    if (c->fps) {
        set_number(s->vt, kVTCompressionPropertyKey_ExpectedFrameRate, (int32_t)c->fps);
    }
    set_number(s->vt, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
               c->gop_sec ? (int32_t)c->gop_sec : 2);
    VTCompressionSessionPrepareToEncodeFrames(s->vt);
    return true;
}

/* ---- sessions (reader thread only) ---- */

static struct vt_session *find_session(uint32_t id)
{
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        if (vt.sessions[i].used && vt.sessions[i].id == id) {
            return &vt.sessions[i];
        }
    }
    return NULL;
}

static void end_session(struct vt_session *s)
{
    destroy_vt(s);
    g_free(s->data);
    s->data = NULL;
    s->size = 0;
    s->used = false;
}

static void configure(const struct venc_config *c)
{
    struct vt_session *s = find_session(c->session);

    if (!s) {
        /* A free slot, or the least recently used session. */
        for (unsigned i = 0; i < MAX_SESSIONS; i++) {
            struct vt_session *t = &vt.sessions[i];

            if (!t->used) {
                s = t;
                break;
            }
            if (!s || t->last_use < s->last_use) {
                s = t;
            }
        }
        if (s->used) {
            warn_report("tebox-vt-venc: too many encoders; dropping session %u", s->id);
            end_session(s);
        }
    }
    destroy_vt(s);
    s->used = true;
    s->id = c->session;
    s->cfg = *c;
    s->last_use = ++vt.clock;
    if (c->width && c->height && c->width <= 8192 && c->height <= 8192) {
        create_vt(s);
    }
}

static void encode(const struct venc_frame *f)
{
    struct vt_session *s = find_session(f->session);
    CVPixelBufferPoolRef pool;
    CVPixelBufferRef pixels = NULL;
    IOSurfaceRef surface;
    CFDictionaryRef props = NULL;
    OSStatus err;
    int32_t status;

    if (!s || !s->vt || f->width != s->cfg.width || f->height != s->cfg.height) {
        send_out(f, -EINVAL, 0, NULL, 0);
        return;
    }
    s->last_use = ++vt.clock;
    pool = VTCompressionSessionGetPixelBufferPool(s->vt);
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixels) != kCVReturnSuccess) {
        send_out(f, -ENOMEM, 0, NULL, 0);
        return;
    }
    surface = CVPixelBufferGetIOSurface(pixels);
    status = surface ? vt.read(f->resource_id, surface) : -ENOTSUP;
    if (status) {
        error_report_once("tebox-vt-venc: GPU read of resource %u failed (%d)", f->resource_id,
                          status);
        CVPixelBufferRelease(pixels);
        send_out(f, status, 0, NULL, 0);
        return;
    }
    if (f->flags & VENC_FLAG_KEY) {
        const void *keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *values[] = { kCFBooleanTrue };

        props = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
    }
    qemu_mutex_lock(&s->lock);
    s->done = false;
    qemu_mutex_unlock(&s->lock);
    err = VTCompressionSessionEncodeFrame(s->vt, pixels, CMTimeMake(f->pts_us, 1000000),
                                          kCMTimeInvalid, props, NULL, NULL);
    if (props) {
        CFRelease(props);
    }
    CVPixelBufferRelease(pixels);
    if (err != noErr) {
        warn_report_once("tebox-vt-venc: encode failed (%d)", (int)err);
        send_out(f, -EIO, 0, NULL, 0);
        return;
    }
    qemu_mutex_lock(&s->lock);
    if (!s->done) {
        int64_t deadline = g_get_monotonic_time() + ENCODE_TIMEOUT_NS / 1000;

        while (!s->done && g_get_monotonic_time() < deadline) {
            qemu_cond_timedwait(&s->done_cond, &s->lock, 100);
        }
    }
    if (s->done) {
        send_out(f, s->status, s->key ? VENC_FLAG_KEY : 0, s->data, s->size);
    } else {
        /* Held back: an empty reply keeps the guest's frames in step. */
        send_out(f, 0, 0, NULL, 0);
    }
    qemu_mutex_unlock(&s->lock);
}

static void *reader(void *opaque)
{
    struct venc_hdr hdr;
    uint8_t *payload = NULL;

    while (read_full(vt.fd, &hdr, sizeof(hdr))) {
        if (hdr.magic != VENC_MAGIC || hdr.size > MAX_MESSAGE) {
            error_report("tebox-vt-venc: bad message from the guest");
            break;
        }
        payload = g_realloc(payload, hdr.size ? hdr.size : 1);
        if (!read_full(vt.fd, payload, hdr.size)) {
            break;
        }
        if (hdr.type == VENC_CONFIG && hdr.size >= sizeof(struct venc_config)) {
            struct venc_config c;

            memcpy(&c, payload, sizeof(c));
            configure(&c);
        } else if (hdr.type == VENC_FRAME && hdr.size >= sizeof(struct venc_frame)) {
            struct venc_frame f;

            memcpy(&f, payload, sizeof(f));
            encode(&f);
        } else if (hdr.type == VENC_CLOSE && hdr.size >= sizeof(uint32_t)) {
            uint32_t id;
            struct vt_session *s;

            memcpy(&id, payload, sizeof(id));
            s = find_session(id);
            if (s) {
                end_session(s);
            }
        }
    }
    g_free(payload);
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        if (vt.sessions[i].used) {
            end_session(&vt.sessions[i]);
        }
    }
    error_report("tebox-vt-venc: guest port closed; host encoding off");
    return NULL;
}

/* Called once the chardevs exist (their listening sockets are bound). */
void tebox_vt_venc_start(void)
{
    const char *path = g_getenv("TEBOX_VENC_SOCK");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    QemuThread thread;
    int fd;

    if (!path || !*path || strlen(path) >= sizeof(addr.sun_path) || vt.fd >= 0) {
        return;
    }
    vt.read = (read_fn)dlsym(RTLD_DEFAULT, "stream_renderer_tebox_read_iosurface");
    if (!vt.read) {
        warn_report("tebox-vt-venc: gfxstream has no IOSurface read; host encoding off");
        return;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        warn_report("tebox-vt-venc: %s: %s", path, strerror(errno));
        close(fd);
        return;
    }
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        qemu_mutex_init(&vt.sessions[i].lock);
        qemu_cond_init(&vt.sessions[i].done_cond);
    }
    vt.fd = fd;
    qemu_thread_create(&thread, "tebox-vt-venc", reader, NULL, QEMU_THREAD_DETACHED);
}
