/* Host video encoder for the guest Codec2 HAL (gfxstream backend).
 *
 * The guest encoder sends a virtio-gpu resource id per frame over the
 * virtio-serial port tebox.venc (venc.sock). The pixels never cross the
 * guest: QEMU reads that gfxstream ColorBuffer into a buffer shared with us
 * (res.sock, virtio-gpu-rutabaga.c), the regular cast encoder turns it into
 * H.264 on the host hardware encoder, and the bitstream goes back on the
 * same port. One reply per frame, in submission order per session.
 * Several guest encoders (two screen-cast clients, say) run at once: every
 * message carries the guest's session id and each session has its own host
 * encoder.
 */
#include "cast/venc.h"
#include "cast/encoder.h"
#include "cast/protocol.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define VENC_BUF_SIZE (64u << 20) /* up to 4096x4096 RGBA */
#define RES_MAGIC 0x53455254u     /* 'TRES', virtio-gpu-rutabaga.c */
#define RES_DMABUF_MAGIC 0x44455254u /* 'TRED': the ColorBuffer's dma-buf */
#define MAX_PAYLOAD (1u << 20)
#define MAX_SESSIONS 8

struct res_req {
    uint32_t magic, resource_id, width, height;
};
struct res_rep {
    int32_t status;
    uint32_t stride;
};

struct venc_session {
    uint32_t id;
    bool used;
    uint64_t last_use;  /* for evicting the least recently used */
    struct cast_encoder *enc;
    struct cast_encoder_cfg cfg;
};

struct cast_venc {
    char venc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char res_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char buf_dir[512];
    pthread_t thread;
    volatile bool run;
    int venc_fd, res_fd;
    bool no_dmabuf; /* QEMU/gfxstream cannot export the frames */
    uint8_t *buf;
    struct venc_session sessions[MAX_SESSIONS];
    uint64_t clock;
};

static int dial(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) return -1;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
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
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Connects res.sock and hands QEMU the shared frame buffer. */
static bool res_connect(struct cast_venc *v)
{
    char tmpl[600];
    char ctrl[CMSG_SPACE(sizeof(int))];
    uint64_t size = VENC_BUF_SIZE;
    struct iovec iov = { &size, sizeof(size) };
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    int fd, file;

    fd = dial(v->res_path);
    if (fd < 0) return false;
    snprintf(tmpl, sizeof(tmpl), "%s/.venc-XXXXXX", v->buf_dir);
    file = mkstemp(tmpl);
    if (file < 0) {
        close(fd);
        return false;
    }
    unlink(tmpl);
    if (ftruncate(file, VENC_BUF_SIZE)) goto fail;
    if (!v->buf) {
        v->buf = mmap(NULL, VENC_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (v->buf == MAP_FAILED) {
            v->buf = NULL;
            goto fail;
        }
    } else if (mmap(v->buf, VENC_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                    file, 0) == MAP_FAILED) {
        goto fail;
    }
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &file, sizeof(int));
    if (sendmsg(fd, &msg, 0) != sizeof(size)) goto fail;
    close(file);
    v->res_fd = fd;
    return true;
fail:
    close(file);
    close(fd);
    return false;
}

/* Reads one resource into v->buf; returns the row stride or -1. */
static int res_read(struct cast_venc *v, uint32_t id, uint32_t w, uint32_t h)
{
    struct res_req req = { RES_MAGIC, id, w, h };
    struct res_rep rep;

    if (v->res_fd < 0 && !res_connect(v)) return -1;
    if (!write_full(v->res_fd, &req, sizeof(req)) || !read_full(v->res_fd, &rep, sizeof(rep))) {
        close(v->res_fd);
        v->res_fd = -1;
        return -1;
    }
    return rep.status == 0 ? (int)rep.stride : -1;
}

/* The frame's own dma-buf (zero-copy) or -1; *stride gets its pitch. */
static int res_dmabuf(struct cast_venc *v, uint32_t id, uint32_t w, uint32_t h, int *stride)
{
    struct res_req req = { RES_DMABUF_MAGIC, id, w, h };
    struct res_rep rep = { -1, 0 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { &rep, sizeof(rep) };
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    int fd = -1;

    if (v->no_dmabuf || (v->res_fd < 0 && !res_connect(v))) return -1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    if (!write_full(v->res_fd, &req, sizeof(req)) ||
        recvmsg(v->res_fd, &msg, MSG_WAITALL) != sizeof(rep)) {
        close(v->res_fd);
        v->res_fd = -1;
        return -1;
    }
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    }
    if (rep.status != 0 || fd < 0) {
        if (fd >= 0) close(fd);
        v->no_dmabuf = true; /* e.g. macOS: read back from now on */
        fprintf(stderr, "venc: no dma-buf frames (%d); using readback\n", rep.status);
        return -1;
    }
    *stride = (int)rep.stride;
    return fd;
}

static bool send_out(struct cast_venc *v, const struct cast_venc_frame *f, int32_t status,
                     uint32_t flags, const uint8_t *data, size_t size)
{
    struct cast_venc_hdr hdr = { CAST_VENC_MAGIC, CAST_VENC_BITSTREAM,
                                 (uint32_t)(sizeof(struct cast_venc_out) + size) };
    struct cast_venc_out out = { f->session, f->index, f->pts_us, status, flags };

    return write_full(v->venc_fd, &hdr, sizeof(hdr)) &&
           write_full(v->venc_fd, &out, sizeof(out)) &&
           (!size || write_full(v->venc_fd, data, size));
}

static struct venc_session *find_session(struct cast_venc *v, uint32_t id)
{
    for (unsigned i = 0; i < MAX_SESSIONS; ++i) {
        if (v->sessions[i].used && v->sessions[i].id == id) return &v->sessions[i];
    }
    return NULL;
}

static void end_session(struct venc_session *s)
{
    if (s->enc) cast_encoder_destroy(s->enc);
    memset(s, 0, sizeof(*s));
}

static void end_all(struct cast_venc *v)
{
    for (unsigned i = 0; i < MAX_SESSIONS; ++i) end_session(&v->sessions[i]);
}

static void encode_frame(struct cast_venc *v, const struct cast_venc_frame *f)
{
    struct venc_session *s = find_session(v, f->session);
    struct cast_packet *pkts = NULL;
    int n = 0, stride = 0, rc = -EINVAL, dmabuf;
    uint32_t fmt = f->format == 5 ? CAST_FMT_BGRA8888 : CAST_FMT_RGBA8888;
    uint8_t *joined = NULL;
    size_t joined_len = 0;
    uint32_t flags = 0;

    if (!s || !s->enc || f->width != (uint32_t)s->cfg.width ||
        f->height != (uint32_t)s->cfg.height) {
        send_out(v, f, -EINVAL, 0, NULL, 0);
        return;
    }
    s->last_use = ++v->clock;
    if (f->flags & CAST_VENC_FLAG_KEY) cast_encoder_force_idr(s->enc);
    dmabuf = res_dmabuf(v, f->resource_id, f->width, f->height, &stride);
    if (dmabuf >= 0) {
        /* The VPU reads the gfxstream ColorBuffer in place. */
        rc = cast_encoder_push_dmabuf(s->enc, fmt, dmabuf, (int)f->width, (int)f->height,
                                      stride, (uint64_t)f->pts_us, &pkts, &n);
        close(dmabuf);
    } else {
        stride = res_read(v, f->resource_id, f->width, f->height);
        if (stride < 0) {
            send_out(v, f, -EIO, 0, NULL, 0);
            return;
        }
        rc = cast_encoder_push(s->enc, fmt, v->buf, (int)f->width, (int)f->height, stride,
                               (uint64_t)f->pts_us, &pkts, &n);
    }
    if (rc == 0) {
        /* Usually one packet per frame; keep the reply 1:1 with the frame. */
        for (int i = 0; i < n; ++i) joined_len += pkts[i].size;
        if (joined_len) joined = malloc(joined_len);
        joined_len = 0;
        for (int i = 0; joined && i < n; ++i) {
            memcpy(joined + joined_len, pkts[i].data, pkts[i].size);
            joined_len += pkts[i].size;
            if (pkts[i].flags & CAST_FLAG_KEY) flags |= CAST_VENC_FLAG_KEY;
        }
        cast_encoder_free_packets(pkts, n);
    }
    send_out(v, f, rc, flags, joined, joined_len);
    free(joined);
}

static void configure(struct cast_venc *v, const struct cast_venc_config *c)
{
    struct cast_encoder_cfg cfg = { 0 };
    struct venc_session *s = find_session(v, c->session);

    if (!s) {
        /* A free slot, or the least recently used session (a guest client
         * that went away without closing). */
        for (unsigned i = 0; i < MAX_SESSIONS && !s; ++i) {
            if (!v->sessions[i].used) s = &v->sessions[i];
        }
        if (!s) {
            s = &v->sessions[0];
            for (unsigned i = 1; i < MAX_SESSIONS; ++i) {
                if (v->sessions[i].last_use < s->last_use) s = &v->sessions[i];
            }
            fprintf(stderr, "venc: evicting session %u\n", s->id);
        }
    }
    end_session(s);
    cfg.codec = CAST_CODEC_H264;
    cfg.width = (int)c->width;
    cfg.height = (int)c->height;
    cfg.fps = c->fps ? (int)c->fps : 30;
    cfg.bitrate = c->bitrate ? c->bitrate : 8000000;
    cfg.gop_sec = c->gop_sec ? (int)c->gop_sec : 2;
    cfg.prefer_hw = true;
    s->used = true;
    s->id = c->session;
    s->last_use = ++v->clock;
    s->cfg = cfg;
    s->enc = cast_encoder_create(&cfg);
    fprintf(stderr, "venc: session %u %ux%u %u kbit/s %u fps encoder=%s\n", c->session,
            c->width, c->height, (unsigned)(cfg.bitrate / 1000), (unsigned)cfg.fps,
            s->enc ? cast_encoder_name(s->enc) : "(none)");
}

static void *venc_thread(void *opaque)
{
    struct cast_venc *v = opaque;
    uint8_t *payload = malloc(MAX_PAYLOAD);

    while (v->run && payload) {
        struct cast_venc_hdr hdr;

        if (v->venc_fd < 0) {
            v->venc_fd = dial(v->venc_path);
            if (v->venc_fd < 0) {
                usleep(500 * 1000);
                continue;
            }
        }
        if (!read_full(v->venc_fd, &hdr, sizeof(hdr)) || hdr.magic != CAST_VENC_MAGIC ||
            hdr.size > MAX_PAYLOAD || (hdr.size && !read_full(v->venc_fd, payload, hdr.size))) {
            /* QEMU went away or the stream desynced: start over. */
            close(v->venc_fd);
            v->venc_fd = -1;
            end_all(v);
            continue;
        }
        if (hdr.type == CAST_VENC_CONFIG && hdr.size >= sizeof(struct cast_venc_config)) {
            configure(v, (const struct cast_venc_config *)payload);
        } else if (hdr.type == CAST_VENC_FRAME && hdr.size >= sizeof(struct cast_venc_frame)) {
            encode_frame(v, (const struct cast_venc_frame *)payload);
        } else if (hdr.type == CAST_VENC_CLOSE && hdr.size >= sizeof(uint32_t)) {
            uint32_t id;
            struct venc_session *s;

            memcpy(&id, payload, sizeof(id));
            s = find_session(v, id);
            if (s) end_session(s);
        }
    }
    free(payload);
    return NULL;
}

struct cast_venc *cast_venc_start(const char *dir)
{
    struct cast_venc *v = calloc(1, sizeof(*v));

    if (!v) return NULL;
    snprintf(v->venc_path, sizeof(v->venc_path), "%s/venc.sock", dir);
    snprintf(v->res_path, sizeof(v->res_path), "%s/res.sock", dir);
    snprintf(v->buf_dir, sizeof(v->buf_dir), "%s", dir);
    v->venc_fd = v->res_fd = -1;
    v->run = true;
    if (pthread_create(&v->thread, NULL, venc_thread, v)) {
        free(v);
        return NULL;
    }
    return v;
}

void cast_venc_stop(struct cast_venc *v)
{
    if (!v) return;
    v->run = false;
    if (v->venc_fd >= 0) shutdown(v->venc_fd, SHUT_RDWR);
    pthread_join(v->thread, NULL);
    if (v->venc_fd >= 0) close(v->venc_fd);
    if (v->res_fd >= 0) close(v->res_fd);
    end_all(v);
    if (v->buf) munmap(v->buf, VENC_BUF_SIZE);
    free(v);
}
