/* Host video decoder for the guest Codec2 HAL (gfxstream backend).
 *
 * The guest decoder sends the bitstream and empty RGBA output buffers
 * (virtio-gpu resource ids) over the virtio-serial port tebox.vdec
 * (vdec.sock). The host V4L2 stateful decoder (CIX "mvx") decodes into its
 * own NV12/P010 buffers; each picture is exported as dma-bufs and QEMU draws
 * it into the next output buffer on the GPU (blit.sock,
 * virtio-gpu-rutabaga.c). No pixel crosses the CPU or the guest.
 * See CAST_VDEC_* in protocol.h.
 */
#include "cast/vdec.h"
#include "cast/protocol.h"

#include <stddef.h>

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PAYLOAD (16u << 20)
#define NUM_OUTPUT 8
#define MAX_CAPTURE 24
#define EXTRA_CAPTURE 4
#define MAX_TARGETS 64
#define MAX_PTS 64
#define MAX_SESSIONS 8

/* blit.sock (virtio-gpu-rutabaga.c): render a YUV dma-buf into a resource. */
#define BLIT_MAGIC 0x544c4254u /* 'TBLT' */
#define DRM_FORMAT_NV12 0x3231564eu
#define DRM_FORMAT_P010 0x30313050u

struct blit_req {
    uint32_t magic;
    uint32_t resource_id;
    uint32_t dst_width, dst_height;
    uint32_t fourcc;
    uint32_t width, height;     /* picture buffer size */
    uint32_t crop_w, crop_h;    /* visible part */
    uint32_t num_fds;           /* one per plane, SCM_RIGHTS */
    uint32_t offsets[2], pitches[2];
} __attribute__((packed));

struct pending_input {
    struct pending_input *next;
    struct cast_vdec_input in;
    size_t size;
    uint8_t data[];
};

struct capture_buf {
    int fds[2];          /* exported planes */
    bool queued;         /* owned by the driver */
    bool ready;          /* decoded, waiting for an output buffer */
    uint64_t index;
    uint32_t flags;
};

struct cast_vdec;

/* One guest decoder: its own V4L2 decoder instance and buffers. */
struct vdec_session {
    struct cast_vdec *owner;
    uint32_t id;
    bool used;
    int fd;               /* V4L2 decoder, -1 when idle */
    uint32_t codec;
    bool out_on, cap_on, stopping;
    bool eos_after_ready; /* LAST seen; the EOS marker follows the ready pictures */
    void *out_map[NUM_OUTPUT];
    size_t out_len[NUM_OUTPUT];
    bool out_free[NUM_OUTPUT];
    unsigned num_out;
    struct capture_buf cap[MAX_CAPTURE];
    unsigned num_cap, num_planes;
    uint32_t cap_fourcc, cap_w, cap_h, crop_w, crop_h, pitches[2], plane_off;
    struct pending_input *in_head, *in_tail;
    bool eos_pending;     /* EOS input seen; DECODER_CMD_STOP after the queue drains */
    struct cast_vdec_output targets[MAX_TARGETS];
    unsigned target_head, target_count;
    struct { uint64_t index; int64_t pts; } pts[MAX_PTS];
    unsigned pts_next;
    uint64_t ready_order[MAX_CAPTURE];
    unsigned ready_head, ready_count;
};

struct cast_vdec {
    char vdec_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char blit_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    pthread_t thread;
    volatile bool run;
    int guest_fd, blit_fd;
    struct vdec_session sessions[MAX_SESSIONS];
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;

    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int dial(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) return -1;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;

    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void send_guest(struct cast_vdec *v, uint32_t type, const void *payload, uint32_t size)
{
    struct cast_vdec_hdr hdr = { CAST_VDEC_MAGIC, type, size };

    if (v->guest_fd < 0) return;
    if (!write_full(v->guest_fd, &hdr, sizeof(hdr)) ||
        (size && !write_full(v->guest_fd, payload, size))) {
        shutdown(v->guest_fd, SHUT_RDWR); /* the reader notices and resets */
    }
}

static void send_picture(struct vdec_session *v, uint64_t id, uint64_t index, int32_t status,
                         uint32_t flags)
{
    struct cast_vdec_picture pic = { v->id, id, index, 0, status, flags, v->crop_w, v->crop_h };

    for (unsigned i = 0; i < MAX_PTS; ++i) {
        if (v->pts[i].index == index) {
            pic.pts_us = v->pts[i].pts;
            break;
        }
    }
    send_guest(v->owner, CAST_VDEC_PICTURE, &pic, sizeof(pic));
}

/* ---- device ---- */

static bool decodes(const char *path, uint32_t codec)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    bool ok = false;

    if (fd < 0) return false;
    for (unsigned i = 0; !ok; i++) {
        struct v4l2_fmtdesc desc = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };

        if (xioctl(fd, VIDIOC_ENUM_FMT, &desc)) break;
        ok = desc.pixelformat == codec;
    }
    close(fd);
    return ok;
}

static int open_decoder(uint32_t codec)
{
    const char *forced = getenv("TEBOX_VDEC_DEVICE");
    char path[64];
    DIR *dir;
    struct dirent *e;
    int fd = -1;

    if (forced && forced[0])
        return decodes(forced, codec) ? open(forced, O_RDWR | O_NONBLOCK | O_CLOEXEC) : -1;
    dir = opendir("/dev");
    if (!dir) return -1;
    while (fd < 0 && (e = readdir(dir))) {
        if (strncmp(e->d_name, "video", 5) || strlen(e->d_name) > 20) continue;
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        if (decodes(path, codec)) fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
    closedir(dir);
    return fd;
}

static void free_capture(struct vdec_session *v)
{
    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP };

    for (unsigned i = 0; i < v->num_cap; ++i) {
        for (unsigned p = 0; p < 2; ++p) {
            if (v->cap[i].fds[p] >= 0) close(v->cap[i].fds[p]);
        }
    }
    memset(v->cap, 0, sizeof(v->cap));
    v->num_cap = 0;
    v->ready_head = v->ready_count = 0;
    if (v->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(v->fd, VIDIOC_STREAMOFF, &type);
        xioctl(v->fd, VIDIOC_REQBUFS, &req);
    }
    v->cap_on = false;
}

static void drop_inputs(struct vdec_session *v)
{
    while (v->in_head) {
        struct pending_input *p = v->in_head;
        v->in_head = p->next;
        free(p);
    }
    v->in_tail = NULL;
}

static void close_session(struct vdec_session *v)
{
    drop_inputs(v);
    if (v->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        free_capture(v);
        xioctl(v->fd, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < v->num_out; ++i) munmap(v->out_map[i], v->out_len[i]);
        close(v->fd);
    }
    v->fd = -1;
    v->num_out = 0;
    v->out_on = v->cap_on = v->stopping = v->eos_pending = v->eos_after_ready = false;
    v->target_head = v->target_count = 0;
}

static bool open_session(struct vdec_session *v, const struct cast_vdec_config *c)
{
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    struct v4l2_requestbuffers req = { .count = NUM_OUTPUT,
                                       .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP };
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

    close_session(v);
    v->codec = c->codec;
    v->fd = open_decoder(c->codec);
    if (v->fd < 0) {
        fprintf(stderr, "vdec: no V4L2 decoder for %.4s\n", (const char *)&c->codec);
        return false;
    }
    fmt.fmt.pix_mp.pixelformat = c->codec;
    fmt.fmt.pix_mp.width = c->width;
    fmt.fmt.pix_mp.height = c->height;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 8u << 20;
    if (xioctl(v->fd, VIDIOC_S_FMT, &fmt) || xioctl(v->fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
        fprintf(stderr, "vdec: S_FMT %.4s: %s\n", (const char *)&c->codec, strerror(errno));
        close_session(v);
        return false;
    }
    sub.type = V4L2_EVENT_EOS;
    xioctl(v->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if (xioctl(v->fd, VIDIOC_REQBUFS, &req) || !req.count) {
        close_session(v);
        return false;
    }
    v->num_out = req.count < NUM_OUTPUT ? req.count : NUM_OUTPUT;
    for (unsigned i = 0; i < v->num_out; ++i) {
        struct v4l2_plane plane = { 0 };
        struct v4l2_buffer buf = { .index = i, .type = req.type, .memory = V4L2_MEMORY_MMAP,
                                   .m.planes = &plane, .length = 1 };

        if (xioctl(v->fd, VIDIOC_QUERYBUF, &buf)) {
            v->num_out = i;
            close_session(v);
            return false;
        }
        v->out_len[i] = plane.length;
        v->out_map[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd,
                             plane.m.mem_offset);
        v->out_free[i] = v->out_map[i] != MAP_FAILED;
        if (!v->out_free[i]) {
            v->num_out = i;
            close_session(v);
            return false;
        }
    }
    if (xioctl(v->fd, VIDIOC_STREAMON, &type)) {
        close_session(v);
        return false;
    }
    v->out_on = true;
    fprintf(stderr, "vdec: session %u %.4s %ux%u\n", v->id, (const char *)&c->codec, c->width,
            c->height);
    return true;
}

static bool queue_capture(struct vdec_session *v, unsigned i)
{
    struct v4l2_plane planes[2] = { 0 };
    struct v4l2_buffer buf = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                               .memory = V4L2_MEMORY_MMAP, .m.planes = planes,
                               .length = v->num_planes };

    if (xioctl(v->fd, VIDIOC_QBUF, &buf)) return false;
    v->cap[i].queued = true;
    v->cap[i].ready = false;
    return true;
}

/* SOURCE_CHANGE: (re)allocate the pictures in the stream's native depth. */
static bool setup_capture(struct vdec_session *v)
{
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    struct v4l2_selection sel = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                  .target = V4L2_SEL_TGT_COMPOSE };
    struct v4l2_control min = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP };
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    uint32_t native;

    free_capture(v);
    if (xioctl(v->fd, VIDIOC_G_FMT, &fmt)) return false;
    native = fmt.fmt.pix_mp.pixelformat;
    /* 10-bit streams report P010-type pictures (M010); keep the depth and let
     * the GPU sample it. Everything else decodes to NV12. */
    fmt.fmt.pix_mp.pixelformat = native == v4l2_fourcc('M', '0', '1', '0') ||
                                         native == V4L2_PIX_FMT_P010
                                 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    /* Mali imports dma-bufs only with aligned rows (a 1080-byte NV12 luma
     * pitch is refused): ask for 64-byte aligned pitches. */
    {
        const uint32_t bpp = fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 ? 2 : 1;
        const uint32_t pitch = (fmt.fmt.pix_mp.width * bpp + 63) & ~63u;

        for (unsigned p = 0; p < fmt.fmt.pix_mp.num_planes && p < 2; ++p) {
            fmt.fmt.pix_mp.plane_fmt[p].bytesperline = pitch;
            fmt.fmt.pix_mp.plane_fmt[p].sizeimage = 0;
        }
    }
    if (xioctl(v->fd, VIDIOC_S_FMT, &fmt) || fmt.fmt.pix_mp.num_planes > 2) return false;
    if (fmt.fmt.pix_mp.plane_fmt[0].bytesperline % 16)
        fprintf(stderr, "vdec: session %u: pitch %u is not GPU-importable\n", v->id,
                fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
    v->cap_fourcc = fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 ? DRM_FORMAT_P010
                                                                    : DRM_FORMAT_NV12;
    v->cap_w = fmt.fmt.pix_mp.width;
    v->cap_h = fmt.fmt.pix_mp.height;
    v->num_planes = fmt.fmt.pix_mp.num_planes;
    v->pitches[0] = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    v->pitches[1] = v->num_planes > 1 ? fmt.fmt.pix_mp.plane_fmt[1].bytesperline
                                      : v->pitches[0];
    /* Single-plane layout: chroma follows the luma rows. */
    v->plane_off = v->num_planes > 1 ? 0 : v->pitches[0] * v->cap_h;
    v->crop_w = v->cap_w;
    v->crop_h = v->cap_h;
    if (!xioctl(v->fd, VIDIOC_G_SELECTION, &sel) && sel.r.width && sel.r.height) {
        v->crop_w = sel.r.width;
        v->crop_h = sel.r.height;
    }
    req.count = (xioctl(v->fd, VIDIOC_G_CTRL, &min) ? 8 : (unsigned)min.value) + EXTRA_CAPTURE;
    if (req.count > MAX_CAPTURE) req.count = MAX_CAPTURE;
    if (xioctl(v->fd, VIDIOC_REQBUFS, &req) || !req.count) return false;
    v->num_cap = req.count;
    for (unsigned i = 0; i < v->num_cap; ++i) {
        v->cap[i].fds[0] = v->cap[i].fds[1] = -1;
        for (unsigned p = 0; p < v->num_planes; ++p) {
            struct v4l2_exportbuffer exp = { .type = req.type, .index = i, .plane = p,
                                             .flags = O_RDONLY | O_CLOEXEC };

            if (xioctl(v->fd, VIDIOC_EXPBUF, &exp)) return false;
            v->cap[i].fds[p] = exp.fd;
        }
        if (!queue_capture(v, i)) return false;
    }
    if (xioctl(v->fd, VIDIOC_STREAMON, &type)) return false;
    v->cap_on = true;
    fprintf(stderr, "vdec: session %u pictures %ux%u (visible %ux%u) pitch %u %.4s x%u\n", v->id,
            v->cap_w, v->cap_h, v->crop_w, v->crop_h, v->pitches[0],
            (const char *)&v->cap_fourcc, v->num_cap);
    return true;
}

/* ---- bitstream ---- */

static void feed(struct vdec_session *v)
{
    while (v->in_head && v->out_on) {
        struct pending_input *p = v->in_head;
        unsigned i = 0;

        while (i < v->num_out && !v->out_free[i]) ++i;
        if (i == v->num_out) return;
        if (p->size > v->out_len[i]) {
            fprintf(stderr, "vdec: %zu-byte access unit dropped\n", p->size);
        } else if (p->size) {
            struct v4l2_plane plane = { .bytesused = (uint32_t)p->size };
            struct v4l2_buffer buf = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP, .m.planes = &plane,
                                       .length = 1 };

            memcpy(v->out_map[i], p->data, p->size);
            /* The guest index rides in the timestamp to the pictures. */
            buf.timestamp.tv_sec = (time_t)(p->in.index / 1000000);
            buf.timestamp.tv_usec = (suseconds_t)(p->in.index % 1000000);
            if (xioctl(v->fd, VIDIOC_QBUF, &buf)) {
                fprintf(stderr, "vdec: QBUF bitstream: %s\n", strerror(errno));
            } else {
                v->out_free[i] = false;
            }
        }
        v->in_head = p->next;
        if (!v->in_head) v->in_tail = NULL;
        free(p);
    }
    if (!v->in_head && v->eos_pending && v->fd >= 0) {
        struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };

        v->eos_pending = false;
        if (!v->cap_on || xioctl(v->fd, VIDIOC_DECODER_CMD, &cmd)) {
            /* Nothing decoded yet (or no STOP support): end right here. */
            send_picture(v, 0, 0, 0, CAST_VDEC_FLAG_EOS);
        } else {
            v->stopping = true;
        }
    }
}

static void add_input(struct vdec_session *v, const uint8_t *payload, uint32_t size)
{
    struct pending_input *p;
    size_t data = size - sizeof(struct cast_vdec_input);

    p = malloc(sizeof(*p) + data);
    if (!p) return;
    memcpy(&p->in, payload, sizeof(p->in));
    memcpy(p->data, payload + sizeof(p->in), data);
    p->size = data;
    p->next = NULL;
    v->pts[v->pts_next].index = p->in.index;
    v->pts[v->pts_next].pts = p->in.pts_us;
    v->pts_next = (v->pts_next + 1) % MAX_PTS;
    if (p->in.flags & CAST_VDEC_FLAG_EOS) v->eos_pending = true;
    if (v->in_tail) v->in_tail->next = p; else v->in_head = p;
    v->in_tail = p;
}

/* ---- pictures ---- */

static bool blit(struct vdec_session *v, const struct capture_buf *c,
                 const struct cast_vdec_output *o)
{
    struct blit_req req = { BLIT_MAGIC, o->resource_id, o->width, o->height, v->cap_fourcc,
                            v->cap_w, v->cap_h, v->crop_w, v->crop_h, v->num_planes > 1 ? 2 : 1,
                            { 0, v->plane_off }, { v->pitches[0], v->pitches[1] } };
    char ctrl[CMSG_SPACE(2 * sizeof(int))];
    struct iovec iov = { &req, sizeof(req) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctrl,
                          .msg_controllen = CMSG_SPACE(req.num_fds * sizeof(int)) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    int32_t status = -EIO;

    if (v->owner->blit_fd < 0) v->owner->blit_fd = dial(v->owner->blit_path);
    if (v->owner->blit_fd < 0) return false;
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(req.num_fds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), c->fds, req.num_fds * sizeof(int));
    if (sendmsg(v->owner->blit_fd, &msg, MSG_NOSIGNAL) != sizeof(req) ||
        !read_full(v->owner->blit_fd, &status, sizeof(status))) {
        close(v->owner->blit_fd);
        v->owner->blit_fd = -1;
        return false;
    }
    return status == 0;
}

/* After the last picture: tell the guest, and accept a new stream. */
static void end_of_stream(struct vdec_session *v)
{
    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_START };

    v->eos_after_ready = v->stopping = false;
    send_picture(v, 0, 0, 0, CAST_VDEC_FLAG_EOS);
    xioctl(v->fd, VIDIOC_DECODER_CMD, &cmd);
}

/* Hands ready pictures to waiting output buffers, oldest first. */
static void deliver(struct vdec_session *v)
{
    while (v->ready_count && v->target_count) {
        unsigned i = (unsigned)v->ready_order[v->ready_head];
        struct capture_buf *c = &v->cap[i];
        struct cast_vdec_output o = v->targets[v->target_head];
        int32_t status = blit(v, c, &o) ? 0 : -EIO;

        v->ready_head = (v->ready_head + 1) % MAX_CAPTURE;
        v->ready_count--;
        v->target_head = (v->target_head + 1) % MAX_TARGETS;
        v->target_count--;
        send_picture(v, o.id, c->index, status, c->flags);
        queue_capture(v, i);
    }
    if (v->eos_after_ready && !v->ready_count) end_of_stream(v);
}

static void reclaim(struct vdec_session *v)
{
    for (;;) {
        struct v4l2_plane plane = { 0 };
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                   .memory = V4L2_MEMORY_MMAP, .m.planes = &plane,
                                   .length = 1 };

        if (xioctl(v->fd, VIDIOC_DQBUF, &buf)) break;
        if (buf.index < v->num_out) v->out_free[buf.index] = true;
    }
}

static void collect(struct vdec_session *v)
{
    while (v->cap_on) {
        struct v4l2_plane planes[2] = { 0 };
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                   .memory = V4L2_MEMORY_MMAP, .m.planes = planes,
                                   .length = v->num_planes };
        bool last;

        if (xioctl(v->fd, VIDIOC_DQBUF, &buf)) break;
        if (buf.index >= v->num_cap) continue;
        last = (buf.flags & V4L2_BUF_FLAG_LAST) != 0;
        v->cap[buf.index].queued = false;
        if (planes[0].bytesused && !(buf.flags & V4L2_BUF_FLAG_ERROR)) {
            struct capture_buf *c = &v->cap[buf.index];

            c->index = (uint64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
            c->flags = 0;
            c->ready = true;
            v->ready_order[(v->ready_head + v->ready_count) % MAX_CAPTURE] = buf.index;
            v->ready_count++;
        } else if (!last) {
            queue_capture(v, buf.index);
        }
        if (last) {
            /* Pictures still waiting for buffers go out first; the marker
             * after them tells the guest the stream is over. */
            if (!planes[0].bytesused) queue_capture(v, buf.index);
            v->eos_after_ready = true;
            deliver(v);
        }
    }
}

static void events(struct vdec_session *v)
{
    struct v4l2_event ev;

    while (!xioctl(v->fd, VIDIOC_DQEVENT, &ev)) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
            (ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
            /* Pictures of the old size cannot be used any more. */
            deliver(v);
            if (!setup_capture(v)) {
                fprintf(stderr, "vdec: capture setup failed: %s\n", strerror(errno));
                send_picture(v, 0, 0, -EIO, 0);
            }
        }
    }
}

static void flush(struct vdec_session *v)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

    drop_inputs(v);
    v->eos_pending = v->stopping = v->eos_after_ready = false;
    v->target_head = v->target_count = 0;
    if (v->fd >= 0) {
        xioctl(v->fd, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < v->num_out; ++i) v->out_free[i] = true;
        xioctl(v->fd, VIDIOC_STREAMON, &type);
        if (v->cap_on) {
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(v->fd, VIDIOC_STREAMOFF, &type);
            v->ready_head = v->ready_count = 0;
            for (unsigned i = 0; i < v->num_cap; ++i) queue_capture(v, i);
            xioctl(v->fd, VIDIOC_STREAMON, &type);
        }
    }
    send_guest(v->owner, CAST_VDEC_FLUSHED, &v->id, sizeof(v->id));
}

/* The session a message belongs to. CONFIG takes a free slot; with every
 * slot in use it is refused and the guest component fails to start. */
static struct vdec_session *find_session(struct cast_vdec *v, uint32_t id, bool create)
{
    struct vdec_session *free_slot = NULL;

    for (unsigned i = 0; i < MAX_SESSIONS; ++i) {
        struct vdec_session *s = &v->sessions[i];

        if (s->used && s->id == id) return s;
        if (!s->used && !free_slot) free_slot = s;
    }
    if (!create || !free_slot) return NULL;
    free_slot->used = true;
    free_slot->id = id;
    return free_slot;
}

static void end_session(struct vdec_session *s)
{
    struct cast_vdec *owner = s->owner;

    close_session(s);
    memset(s, 0, sizeof(*s));
    s->owner = owner;
    s->fd = -1;
}

static void handle(struct cast_vdec *v, uint32_t type, const uint8_t *payload, uint32_t size)
{
    struct vdec_session *s;
    uint32_t id;

    if (size < sizeof(id)) return;
    memcpy(&id, payload, sizeof(id));
    s = find_session(v, id, type == CAST_VDEC_CONFIG);
    if (!s) {
        if (type == CAST_VDEC_CONFIG) {
            struct vdec_session refused = { .owner = v, .id = id, .fd = -1 };

            fprintf(stderr, "vdec: no free session for %u\n", id);
            send_picture(&refused, 0, 0, -EBUSY, CAST_VDEC_FLAG_EOS);
        } else if (type == CAST_VDEC_FLUSH) {
            send_guest(v, CAST_VDEC_FLUSHED, &id, sizeof(id));
        }
        return;
    }
    if (type == CAST_VDEC_CONFIG && size >= sizeof(struct cast_vdec_config)) {
        if (!open_session(s, (const struct cast_vdec_config *)payload)) {
            send_picture(s, 0, 0, -ENODEV, CAST_VDEC_FLAG_EOS);
            end_session(s);
        }
    } else if (type == CAST_VDEC_INPUT && size >= sizeof(struct cast_vdec_input)) {
        if (s->fd >= 0) add_input(s, payload, size);
    } else if (type == CAST_VDEC_OUTPUT && size >= sizeof(struct cast_vdec_output)) {
        if (s->target_count < MAX_TARGETS) {
            memcpy(&s->targets[(s->target_head + s->target_count) % MAX_TARGETS], payload,
                   sizeof(struct cast_vdec_output));
            s->target_count++;
        }
    } else if (type == CAST_VDEC_FLUSH) {
        flush(s);
    } else if (type == CAST_VDEC_CLOSE) {
        end_session(s);
    }
}

static void end_all(struct cast_vdec *v)
{
    for (unsigned i = 0; i < MAX_SESSIONS; ++i) {
        if (v->sessions[i].used) end_session(&v->sessions[i]);
    }
}

static void *vdec_thread(void *opaque)
{
    struct cast_vdec *v = opaque;
    uint8_t *payload = malloc(MAX_PAYLOAD);

    while (v->run && payload) {
        struct pollfd fds[1 + MAX_SESSIONS];
        struct vdec_session *polled[1 + MAX_SESSIONS];
        nfds_t n = 0;

        if (v->guest_fd < 0) {
            v->guest_fd = dial(v->vdec_path);
            if (v->guest_fd < 0) {
                usleep(500 * 1000);
                continue;
            }
        }
        fds[n++] = (struct pollfd){ v->guest_fd, POLLIN, 0 };
        for (unsigned i = 0; i < MAX_SESSIONS; ++i) {
            if (v->sessions[i].used && v->sessions[i].fd >= 0) {
                polled[n] = &v->sessions[i];
                fds[n++] = (struct pollfd){ v->sessions[i].fd, POLLIN | POLLOUT | POLLPRI, 0 };
            }
        }
        if (poll(fds, n, 100) < 0 && errno != EINTR) break;
        if (fds[0].revents) {
            struct cast_vdec_hdr hdr;

            if (!read_full(v->guest_fd, &hdr, sizeof(hdr)) || hdr.magic != CAST_VDEC_MAGIC ||
                hdr.size > MAX_PAYLOAD ||
                (hdr.size && !read_full(v->guest_fd, payload, hdr.size))) {
                /* QEMU went away or the stream desynced: start over. */
                close(v->guest_fd);
                v->guest_fd = -1;
                end_all(v);
                continue;
            }
            handle(v, hdr.type, payload, hdr.size);
        }
        for (nfds_t i = 1; i < n; ++i) {
            struct vdec_session *s = polled[i];

            if (!s->used || s->fd < 0) continue;  /* closed by the message above */
            if (fds[i].revents & POLLPRI) events(s);
            reclaim(s);
            collect(s);
            deliver(s);
            feed(s);
        }
    }
    free(payload);
    return NULL;
}

struct cast_vdec *cast_vdec_start(const char *dir)
{
    struct cast_vdec *v = calloc(1, sizeof(*v));

    if (!v) return NULL;
    snprintf(v->vdec_path, sizeof(v->vdec_path), "%s/vdec.sock", dir);
    snprintf(v->blit_path, sizeof(v->blit_path), "%s/blit.sock", dir);
    v->guest_fd = v->blit_fd = -1;
    for (unsigned i = 0; i < MAX_SESSIONS; ++i) {
        v->sessions[i].owner = v;
        v->sessions[i].fd = -1;
    }
    v->run = true;
    if (pthread_create(&v->thread, NULL, vdec_thread, v)) {
        free(v);
        return NULL;
    }
    return v;
}

void cast_vdec_stop(struct cast_vdec *v)
{
    if (!v) return;
    v->run = false;
    if (v->guest_fd >= 0) shutdown(v->guest_fd, SHUT_RDWR);
    pthread_join(v->thread, NULL);
    end_all(v);
    if (v->guest_fd >= 0) close(v->guest_fd);
    if (v->blit_fd >= 0) close(v->blit_fd);
    free(v);
}

#else /* !__linux__: no V4L2 decoder on this host */

struct cast_vdec *cast_vdec_start(const char *dir)
{
    (void)dir;
    return NULL;
}

void cast_vdec_stop(struct cast_vdec *v)
{
    (void)v;
}

#endif
