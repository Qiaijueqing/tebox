/* tebox host cast wire formats (raw frames + encoded stream). */
#ifndef TEBOX_CAST_PROTOCOL_H
#define TEBOX_CAST_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAST_RAW_MAGIC   0x54434652u /* 'TCFR' */
#define CAST_RAW_VERSION 1u

#define CAST_FMT_BGRA8888 0u
#define CAST_FMT_RGBA8888 1u /* GLES readback (egl-headless) */
/* Format flag: the frame is in a dma-buf sent with SCM_RIGHTS alongside the
 * header (QEMU's GPU scaled it there); the 4-byte body is unused. The
 * receiver acks with one byte once the buffer may be reused. */
#define CAST_FMT_DMABUF   0x100u

enum cast_codec {
    CAST_CODEC_H264 = 0,
    CAST_CODEC_HEVC = 1,
    CAST_CODEC_VP8  = 2,
    CAST_CODEC_VP9  = 3,
    CAST_CODEC_AV1  = 4,
    /* Server -> viewer audio: AAC-LC 48 kHz stereo, one ADTS frame per record. */
    CAST_CODEC_AAC  = 16,
    /* Viewer -> server (same record format, sent after the handshake line):
     * a camera frame as one JPEG (any size, the server scales it), and
     * microphone audio as PCM s16le 48 kHz mono. */
    CAST_CODEC_MJPEG = 32,
    CAST_CODEC_PCM_S16LE = 33,
};

#define CAST_FLAG_KEY 0x01u

/* QEMU → tebox-cast raw frame header (all multi-byte fields big-endian). */
struct cast_raw_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t pts_us;
    uint32_t nbytes;
} __attribute__((packed));

/* Encoded stream: [u32 be length][u8 codec][u8 flags][u64 be pts_us][payload]
 * length covers codec..end of payload (not including the length field). */
#define CAST_ENC_HDR_SIZE 10u
/* Largest record a viewer may send upstream (one camera JPEG). */
#define CAST_UPLINK_MAX (4u << 20)

/* Guest media over virtio-serial (QEMU chardev sockets next to cast.sock):
 *   audio-out.sock  guest -> host  PCM s16le 48 kHz stereo
 *   audio-in.sock   host -> guest  PCM s16le 48 kHz mono (microphone)
 *   camera.sock     host -> guest  camera frames, each a cast_camera_header
 *                                  followed by NV12 (width*height*3/2 bytes)
 * Multi-byte header fields are little-endian (both ends are ARM64). */
#define CAST_AUDIO_RATE       48000
#define CAST_CAMERA_MAGIC     0x4d414354u /* 'TCAM' */
#define CAST_CAMERA_FMT_NV12  1u
#define CAST_CAMERA_WIDTH     1280
#define CAST_CAMERA_HEIGHT    720

struct cast_camera_header {
    uint32_t magic;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    uint32_t reserved;
    uint64_t pts_us;
} __attribute__((packed));

/* Host video encoder for the guest Codec2 HAL (gfxstream backend).
 * virtio-serial port "tebox.venc" (socket venc.sock next to cast.sock).
 * Messages both ways: struct cast_venc_hdr, then hdr.size payload bytes.
 * The guest sends only virtio-gpu resource ids; tebox-cast reads the pixels
 * from QEMU (res.sock, virtio-gpu-rutabaga.c) and encodes them on the host.
 * Several encoders share the port: every payload starts with the guest's
 * session id, and each session has its own host encoder.
 * Multi-byte fields are little-endian (both ends are ARM64). */
#define CAST_VENC_MAGIC 0x434e4556u /* 'VENC' */

enum cast_venc_type {
    CAST_VENC_CONFIG = 1,     /* guest -> host: struct cast_venc_config */
    CAST_VENC_FRAME = 2,      /* guest -> host: struct cast_venc_frame */
    CAST_VENC_CLOSE = 3,      /* guest -> host: u32 session; end it */
    CAST_VENC_BITSTREAM = 16, /* host -> guest: struct cast_venc_out + H.264 */
};

#define CAST_VENC_FLAG_KEY 0x01u /* FRAME: force IDR; BITSTREAM: is IDR */

struct cast_venc_hdr {
    uint32_t magic;
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct cast_venc_config {
    uint32_t session;
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;   /* bits/s */
    uint32_t fps;
    uint32_t gop_sec;
} __attribute__((packed));

struct cast_venc_frame {
    uint32_t session;
    uint64_t index;     /* submission order; echoed in the reply */
    int64_t pts_us;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;    /* Android PixelFormat: 1 RGBA, 2 RGBX, 5 BGRA */
    uint32_t flags;
} __attribute__((packed));

/* One reply per frame, in order; size 0 = the encoder held the frame back
 * or failed (status < 0). Annex-B with SPS/PPS before each IDR. */
struct cast_venc_out {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags;
} __attribute__((packed));

/* Host video decoder for the guest Codec2 HAL: virtio-serial port tebox.vdec
 * (vdec.sock). The guest sends the bitstream and empty RGBA output buffers
 * (virtio-gpu resource ids); tebox-cast decodes on the host hardware decoder
 * and QEMU renders each picture into an output buffer on the GPU (blit.sock,
 * virtio-gpu-rutabaga.c). Same header and endianness as VENC. Several
 * decoders share the port: every payload starts with the guest's session id
 * (FLUSH, CLOSE and FLUSHED carry only that) and each session has its own
 * host decoder. */
#define CAST_VDEC_MAGIC 0x43454456u /* 'VDEC' */

enum cast_vdec_type {
    CAST_VDEC_CONFIG = 1,  /* guest -> host: struct cast_vdec_config */
    CAST_VDEC_INPUT = 2,   /* guest -> host: struct cast_vdec_input + bitstream */
    CAST_VDEC_OUTPUT = 3,  /* guest -> host: struct cast_vdec_output (empty buffer) */
    CAST_VDEC_FLUSH = 4,   /* guest -> host: drop queued input, pictures and buffers */
    CAST_VDEC_CLOSE = 5,   /* guest -> host: end the session */
    CAST_VDEC_PICTURE = 16, /* host -> guest: struct cast_vdec_picture */
    CAST_VDEC_FLUSHED = 17, /* host -> guest: FLUSH done; every buffer is back */
};

struct cast_vdec_hdr {
    uint32_t magic;
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

#define CAST_VDEC_FLAG_EOS 0x01u    /* INPUT: last one; PICTURE: no more follow */
#define CAST_VDEC_FLAG_CONFIG 0x02u /* INPUT: codec config (CSD) only */

struct cast_vdec_config {
    uint32_t session;
    uint32_t codec;  /* V4L2 fourcc: 'HEVC', 'H264', 'VP90', 'AV01' */
    uint32_t width;  /* hint; the stream decides */
    uint32_t height;
} __attribute__((packed));

/* hdr.size = sizeof(struct cast_vdec_input) + bitstream bytes */
struct cast_vdec_input {
    uint32_t session;
    uint64_t index;  /* guest frame index, echoed by the pictures it produces */
    int64_t pts_us;
    uint32_t flags;
} __attribute__((packed));

struct cast_vdec_output {
    uint32_t session;
    uint64_t id;          /* guest handle, echoed in the picture */
    uint32_t resource_id; /* RGBA virtio-gpu resource */
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* A decoded picture, rendered into output |id| (or none: status < 0, or an
 * EOS marker with id 0). Pictures come in display order. */
struct cast_vdec_picture {
    uint32_t session;
    uint64_t id;
    uint64_t index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags;
    uint32_t width;   /* visible size */
    uint32_t height;
} __attribute__((packed));

const char *cast_codec_name(enum cast_codec c);
enum cast_codec cast_codec_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
