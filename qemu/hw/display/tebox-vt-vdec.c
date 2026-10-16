/*
 * tebox (local, not for upstream): host video decoder for the guest Codec2
 * HAL on macOS. Linux has tebox-cast vdec (V4L2) for this; on macOS QEMU
 * itself is the client of the guest's tebox.vdec port ($TEBOX_VDEC_SOCK) and
 * decodes on VideoToolbox. Each picture comes out as a BGRA IOSurface and
 * gfxstream blits it into the guest output buffer's ColorBuffer on the GPU
 * (stream_renderer_tebox_blit_iosurface), so no pixel touches the CPU.
 *
 * Protocol: CAST_VDEC_* in src/service/cast/include/cast/protocol.h, mirrored
 * here. A reader thread handles the guest's messages; VideoToolbox hands the
 * pictures over in display order on its own thread, which renders them into
 * the waiting output buffers. One lock guards the sessions.
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

void tebox_vt_vdec_start(void);

#define VDEC_MAGIC 0x43454456u /* 'VDEC' */

enum {
    VDEC_CONFIG = 1,
    VDEC_INPUT = 2,
    VDEC_OUTPUT = 3,
    VDEC_FLUSH = 4,
    VDEC_CLOSE = 5,
    VDEC_PICTURE = 16,
    VDEC_FLUSHED = 17,
};

#define VDEC_FLAG_EOS 0x01u
#define VDEC_FLAG_CONFIG 0x02u

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

struct vdec_hdr {
    uint32_t magic, type, size;
} QEMU_PACKED;

struct vdec_config {
    uint32_t session, codec, width, height;
} QEMU_PACKED;

struct vdec_input {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    uint32_t flags;
} QEMU_PACKED;

struct vdec_output {
    uint32_t session;
    uint64_t id;
    uint32_t resource_id, width, height;
} QEMU_PACKED;

struct vdec_picture {
    uint32_t session;
    uint64_t id, index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags, width, height;
} QEMU_PACKED;

#define MAX_SESSIONS 8
#define MAX_TARGETS 32
#define MAX_READY 32
#define MAX_PARAM_SETS 8
#define MAX_INPUT (32u << 20)

typedef int (*blit_fn)(uint32_t res_handle, void *io_surface);

struct ready_picture {
    CVPixelBufferRef buffer;
    uint64_t index;
    int64_t pts_us;
};

/* Rides along with each frame through VideoToolbox. */
struct frame_ref {
    uint32_t generation;
    uint64_t index;
    int64_t pts_us;
};

struct param_set {
    uint8_t *data;
    size_t size;
};

struct vt_session {
    bool used;
    uint32_t id;
    bool hevc;
    uint32_t generation;  /* bumped by FLUSH: late pictures are dropped */
    VTDecompressionSessionRef vt;
    CMVideoFormatDescriptionRef format;
    struct param_set params[MAX_PARAM_SETS]; /* the stream's VPS/SPS/PPS */
    unsigned num_params;
    bool params_changed;
    struct vdec_output targets[MAX_TARGETS];
    unsigned target_head, target_count;
    struct ready_picture ready[MAX_READY];
    unsigned ready_head, ready_count;
    bool eos_after_ready;
};

static struct {
    QemuMutex lock;
    QemuMutex write_lock;
    int fd;
    blit_fn blit;
    struct vt_session sessions[MAX_SESSIONS];
} vt = { .fd = -1 };

/* ---- guest messages ---- */

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

static void send_guest(uint32_t type, const void *payload, uint32_t size)
{
    struct vdec_hdr hdr = { VDEC_MAGIC, type, size };

    qemu_mutex_lock(&vt.write_lock);
    if (!write_full(vt.fd, &hdr, sizeof(hdr)) ||
        (size && !write_full(vt.fd, payload, size))) {
        error_report_once("tebox-vt-vdec: write to the guest failed: %s", strerror(errno));
    }
    qemu_mutex_unlock(&vt.write_lock);
}

static void send_picture(struct vt_session *s, uint64_t id, uint64_t index, int64_t pts_us,
                         int32_t status, uint32_t flags, uint32_t width, uint32_t height)
{
    struct vdec_picture pic = { s->id, id, index, pts_us, status, flags, width, height };

    send_guest(VDEC_PICTURE, &pic, sizeof(pic));
}

/* ---- pictures (vt.lock held) ---- */

static void drop_ready(struct vt_session *s)
{
    while (s->ready_count) {
        CVPixelBufferRelease(s->ready[s->ready_head].buffer);
        s->ready_head = (s->ready_head + 1) % MAX_READY;
        s->ready_count--;
    }
    s->ready_head = 0;
}

/* Renders ready pictures into waiting output buffers, oldest first. */
static void deliver(struct vt_session *s)
{
    while (s->ready_count && s->target_count) {
        struct ready_picture *r = &s->ready[s->ready_head];
        struct vdec_output o = s->targets[s->target_head];
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(r->buffer);
        int32_t status = surface && vt.blit ? vt.blit(o.resource_id, surface) : -ENOTSUP;

        if (status) {
            error_report_once("tebox-vt-vdec: GPU blit into resource %u failed (%d)",
                              o.resource_id, status);
        }
        send_picture(s, o.id, r->index, r->pts_us, status, 0,
                     (uint32_t)CVPixelBufferGetWidth(r->buffer),
                     (uint32_t)CVPixelBufferGetHeight(r->buffer));
        CVPixelBufferRelease(r->buffer);
        s->ready_head = (s->ready_head + 1) % MAX_READY;
        s->ready_count--;
        s->target_head = (s->target_head + 1) % MAX_TARGETS;
        s->target_count--;
    }
    if (s->eos_after_ready && !s->ready_count) {
        s->eos_after_ready = false;
        send_picture(s, 0, 0, 0, 0, VDEC_FLAG_EOS, 0, 0);
    }
}

static void decoded(void *opaque, void *frame, OSStatus status, VTDecodeInfoFlags info,
                    CVImageBufferRef image, CMTime pts, CMTime duration)
{
    struct vt_session *s = opaque;
    struct frame_ref *ref = frame;

    qemu_mutex_lock(&vt.lock);
    if (!s->used || ref->generation != s->generation) {
        goto out;
    }
    if (status != noErr || !image) {
        if (status != noErr) {
            warn_report_once("tebox-vt-vdec: frame %" PRIu64 " not decoded (%d)",
                             ref->index, (int)status);
        }
        goto out;
    }
    if (s->ready_count == MAX_READY) {
        /* The guest stopped handing out buffers: lose the oldest picture. */
        CVPixelBufferRelease(s->ready[s->ready_head].buffer);
        s->ready_head = (s->ready_head + 1) % MAX_READY;
        s->ready_count--;
    }
    s->ready[(s->ready_head + s->ready_count) % MAX_READY] =
        (struct ready_picture){ CVPixelBufferRetain(image), ref->index, ref->pts_us };
    s->ready_count++;
    deliver(s);
out:
    qemu_mutex_unlock(&vt.lock);
    g_free(ref);
}

/* ---- VideoToolbox (reader thread) ---- */

/* Waits for the frames in flight; their callbacks take vt.lock. */
static void wait_frames(struct vt_session *s, bool finish)
{
    VTDecompressionSessionRef session = s->vt;

    if (!session) {
        return;
    }
    CFRetain(session);
    qemu_mutex_unlock(&vt.lock);
    if (finish) {
        VTDecompressionSessionFinishDelayedFrames(session);
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    qemu_mutex_lock(&vt.lock);
    CFRelease(session);
}

static void destroy_vt(struct vt_session *s)
{
    if (s->vt) {
        VTDecompressionSessionInvalidate(s->vt);
        CFRelease(s->vt);
        s->vt = NULL;
    }
    if (s->format) {
        CFRelease(s->format);
        s->format = NULL;
    }
}

static void clear_params(struct vt_session *s)
{
    for (unsigned i = 0; i < s->num_params; i++) {
        g_free(s->params[i].data);
    }
    s->num_params = 0;
}

static CMVideoFormatDescriptionRef make_format(struct vt_session *s)
{
    const uint8_t *sets[MAX_PARAM_SETS];
    size_t sizes[MAX_PARAM_SETS];
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus err;

    for (unsigned i = 0; i < s->num_params; i++) {
        sets[i] = s->params[i].data;
        sizes[i] = s->params[i].size;
    }
    if (s->hevc) {
        err = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault, s->num_params, sets, sizes, 4, NULL, &format);
    } else {
        err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, s->num_params, sets, sizes, 4, &format);
    }
    if (err != noErr) {
        warn_report("tebox-vt-vdec: bad parameter sets (%d)", (int)err);
        return NULL;
    }
    return format;
}

static bool create_vt(struct vt_session *s, CMVideoFormatDescriptionRef format)
{
    const int32_t bgra = kCVPixelFormatType_32BGRA;
    CFNumberRef pixel_format = CFNumberCreate(NULL, kCFNumberSInt32Type, &bgra);
    CFDictionaryRef iosurface = CFDictionaryCreate(NULL, NULL, NULL, 0,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey,
                           kCVPixelBufferMetalCompatibilityKey };
    const void *values[] = { pixel_format, iosurface, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *spec_keys[] = {
        kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder };
    const void *spec_values[] = { kCFBooleanTrue };
    CFDictionaryRef spec = CFDictionaryCreate(NULL, spec_keys, spec_values, 1,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback = { decoded, s };
    OSStatus err = VTDecompressionSessionCreate(kCFAllocatorDefault, format, spec, attrs,
                                                &callback, &s->vt);

    CFRelease(spec);
    CFRelease(attrs);
    CFRelease(iosurface);
    CFRelease(pixel_format);
    if (err != noErr) {
        warn_report("tebox-vt-vdec: no hardware %s decoder (%d)", s->hevc ? "HEVC" : "AVC",
                    (int)err);
        s->vt = NULL;
        return false;
    }
    s->format = format;
    return true;
}

static bool is_param_set(struct vt_session *s, const uint8_t *nal)
{
    if (s->hevc) {
        unsigned type = (nal[0] >> 1) & 0x3f;

        return type >= 32 && type <= 34; /* VPS, SPS, PPS */
    }
    return (nal[0] & 0x1f) == 7 || (nal[0] & 0x1f) == 8; /* SPS, PPS */
}

/* Next Annex-B NAL unit at or after *pos, without its start code. */
static bool next_nal(const uint8_t *data, size_t size, size_t *pos, const uint8_t **nal,
                     size_t *nal_size)
{
    size_t i = *pos, start, end;

    while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
        i++;
    }
    if (i + 3 > size) {
        return false;
    }
    start = i + 3;
    end = start;
    while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 &&
                                (data[end + 2] == 1 ||
                                 (data[end + 2] == 0 && end + 3 < size && data[end + 3] == 1)))) {
        end++;
    }
    if (end + 3 > size) {
        end = size;
    }
    *pos = end;
    *nal = data + start;
    *nal_size = end - start;
    while (*nal_size && (*nal)[*nal_size - 1] == 0) {
        (*nal_size)--; /* trailing zero bytes belong to the next start code */
    }
    return *nal_size > 0;
}

static void decode(struct vt_session *s, const struct vdec_input *in, const uint8_t *data,
                   size_t size)
{
    const uint8_t *nal;
    size_t nal_size, pos = 0, avcc_size = 0;
    bool new_params = false;
    uint8_t *avcc;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing = { kCMTimeInvalid, CMTimeMake(in->pts_us, 1000000),
                                  kCMTimeInvalid };
    struct frame_ref *ref;
    OSStatus err;

    /* Parameter sets update the stream; the rest becomes 4-byte length-prefixed. */
    avcc = g_malloc(size + size / 2 + 16);
    while (next_nal(data, size, &pos, &nal, &nal_size)) {
        if (is_param_set(s, nal)) {
            if (!new_params) {
                clear_params(s);
                new_params = true;
            }
            if (s->num_params < MAX_PARAM_SETS) {
                s->params[s->num_params++] =
                    (struct param_set){ g_memdup2(nal, nal_size), nal_size };
            }
            continue;
        }
        avcc[avcc_size++] = (uint8_t)(nal_size >> 24);
        avcc[avcc_size++] = (uint8_t)(nal_size >> 16);
        avcc[avcc_size++] = (uint8_t)(nal_size >> 8);
        avcc[avcc_size++] = (uint8_t)nal_size;
        memcpy(avcc + avcc_size, nal, nal_size);
        avcc_size += nal_size;
    }
    if (new_params) {
        s->params_changed = true;
    }
    if (!avcc_size) {
        g_free(avcc);
        return;
    }
    if (s->params_changed && s->num_params) {
        CMVideoFormatDescriptionRef format = make_format(s);

        s->params_changed = false;
        if (format && s->vt && !VTDecompressionSessionCanAcceptFormatDescription(s->vt, format)) {
            /* New stream geometry: finish the old pictures, then start over. */
            wait_frames(s, true);
            destroy_vt(s);
        }
        if (format && !s->vt) {
            if (!create_vt(s, format)) {
                CFRelease(format);
            }
        } else if (format) {
            if (s->format) {
                CFRelease(s->format);
            }
            s->format = format;
        }
    }
    if (!s->vt) {
        g_free(avcc);
        return;
    }
    err = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, avcc, avcc_size,
                                             kCFAllocatorMalloc, NULL, 0, avcc_size, 0, &block);
    if (err != noErr) {
        g_free(avcc);
        return;
    }
    err = CMSampleBufferCreateReady(kCFAllocatorDefault, block, s->format, 1, 1, &timing, 1,
                                    &avcc_size, &sample);
    CFRelease(block);
    if (err != noErr) {
        return;
    }
    ref = g_new(struct frame_ref, 1);
    *ref = (struct frame_ref){ s->generation, in->index, in->pts_us };
    {
        VTDecompressionSessionRef session = s->vt;

        /* The callback may run right away and takes vt.lock. */
        CFRetain(session);
        qemu_mutex_unlock(&vt.lock);
        err = VTDecompressionSessionDecodeFrame(
            session, sample,
            kVTDecodeFrame_EnableAsynchronousDecompression |
                kVTDecodeFrame_EnableTemporalProcessing,
            ref, NULL);
        qemu_mutex_lock(&vt.lock);
        CFRelease(session);
    }
    CFRelease(sample);
    if (err != noErr) {
        warn_report_once("tebox-vt-vdec: decode failed (%d)", (int)err);
        /* VideoToolbox reports a failed frame through the callback, which
         * frees |ref|; if it did not, a few bytes leak. */
    }
}

/* ---- sessions (vt.lock held) ---- */

static struct vt_session *find_session(uint32_t id, bool create)
{
    struct vt_session *free_slot = NULL;

    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        struct vt_session *s = &vt.sessions[i];

        if (s->used && s->id == id) {
            return s;
        }
        if (!s->used && !free_slot) {
            free_slot = s;
        }
    }
    if (!create || !free_slot) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->id = id;
    return free_slot;
}

static void end_session(struct vt_session *s)
{
    s->generation++;
    wait_frames(s, false);
    destroy_vt(s);
    drop_ready(s);
    clear_params(s);
    s->used = false;
}

static void flush(struct vt_session *s)
{
    s->generation++;
    wait_frames(s, false);
    drop_ready(s);
    s->target_head = s->target_count = 0;
    s->eos_after_ready = false;
    send_guest(VDEC_FLUSHED, &s->id, sizeof(s->id));
}

static void handle(uint32_t type, const uint8_t *payload, uint32_t size)
{
    struct vt_session *s;
    uint32_t id;

    if (size < sizeof(id)) {
        return;
    }
    memcpy(&id, payload, sizeof(id));
    s = find_session(id, type == VDEC_CONFIG);
    if (!s) {
        if (type == VDEC_CONFIG) {
            struct vt_session refused = { .id = id };

            send_picture(&refused, 0, 0, 0, -EBUSY, VDEC_FLAG_EOS, 0, 0);
        } else if (type == VDEC_FLUSH) {
            send_guest(VDEC_FLUSHED, &id, sizeof(id));
        }
        return;
    }
    if (type == VDEC_CONFIG && size >= sizeof(struct vdec_config)) {
        struct vdec_config c;

        memcpy(&c, payload, sizeof(c));
        if (c.codec != FOURCC('H', 'E', 'V', 'C') && c.codec != FOURCC('H', '2', '6', '4')) {
            send_picture(s, 0, 0, 0, -ENODEV, VDEC_FLAG_EOS, 0, 0);
            end_session(s);
            return;
        }
        if (s->vt) {
            end_session(s);
            s = find_session(id, true);
        }
        s->hevc = c.codec == FOURCC('H', 'E', 'V', 'C');
    } else if (type == VDEC_INPUT && size >= sizeof(struct vdec_input)) {
        struct vdec_input in;

        memcpy(&in, payload, sizeof(in));
        decode(s, &in, payload + sizeof(in), size - sizeof(in));
        if (in.flags & VDEC_FLAG_EOS) {
            /* The last pictures, then the EOS marker; a new stream may follow. */
            wait_frames(s, true);
            s->eos_after_ready = true;
            deliver(s);
        }
    } else if (type == VDEC_OUTPUT && size >= sizeof(struct vdec_output)) {
        if (s->target_count < MAX_TARGETS) {
            memcpy(&s->targets[(s->target_head + s->target_count) % MAX_TARGETS], payload,
                   sizeof(struct vdec_output));
            s->target_count++;
            deliver(s);
        }
    } else if (type == VDEC_FLUSH) {
        flush(s);
    } else if (type == VDEC_CLOSE) {
        end_session(s);
    }
}

static void *reader(void *opaque)
{
    struct vdec_hdr hdr;
    uint8_t *payload = NULL;

    while (read_full(vt.fd, &hdr, sizeof(hdr))) {
        if (hdr.magic != VDEC_MAGIC || hdr.size > MAX_INPUT) {
            error_report("tebox-vt-vdec: bad message from the guest");
            break;
        }
        payload = g_realloc(payload, hdr.size ? hdr.size : 1);
        if (!read_full(vt.fd, payload, hdr.size)) {
            break;
        }
        qemu_mutex_lock(&vt.lock);
        handle(hdr.type, payload, hdr.size);
        qemu_mutex_unlock(&vt.lock);
    }
    g_free(payload);
    qemu_mutex_lock(&vt.lock);
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        if (vt.sessions[i].used) {
            end_session(&vt.sessions[i]);
        }
    }
    qemu_mutex_unlock(&vt.lock);
    error_report("tebox-vt-vdec: guest port closed; host decoding off");
    return NULL;
}

/* Called once the chardevs exist (their listening sockets are bound). */
void tebox_vt_vdec_start(void)
{
    const char *path = g_getenv("TEBOX_VDEC_SOCK");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    QemuThread thread;
    int fd;

    if (!path || !*path || strlen(path) >= sizeof(addr.sun_path) || vt.fd >= 0) {
        return;
    }
    vt.blit = (blit_fn)dlsym(RTLD_DEFAULT, "stream_renderer_tebox_blit_iosurface");
    if (!vt.blit) {
        warn_report("tebox-vt-vdec: gfxstream has no IOSurface blit; host decoding off");
        return;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        warn_report("tebox-vt-vdec: %s: %s", path, strerror(errno));
        close(fd);
        return;
    }
    vt.fd = fd;
    qemu_mutex_init(&vt.lock);
    qemu_mutex_init(&vt.write_lock);
    qemu_thread_create(&thread, "tebox-vt-vdec", reader, NULL, QEMU_THREAD_DETACHED);
}
