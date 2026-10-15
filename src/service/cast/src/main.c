#include "cast/capture.h"
#include "cast/control.h"
#include "cast/encoder.h"
#include "cast/media.h"
#include "cast/venc.h"
#include "cast/vdec.h"
#include "cast/transport.h"

#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;

static struct cast_venc *g_venc;
static struct cast_vdec *g_vdec;

struct app {
    struct cast_encoder *enc;
    struct cast_encoder_cfg cfg;
    struct cast_transport *tx;
    struct cast_file_sink *file;
    struct cast_control *ctrl;
    struct cast_media *media;
    uint64_t frames_in;
    uint64_t packets_out;
};

static void on_audio(void *user, const uint8_t *adts, size_t size, uint64_t pts_us)
{
    struct app *app = user;

    cast_transport_send_packet(app->tx, CAST_CODEC_AAC, 0, pts_us, adts, size);
}

/* Viewer uplink: camera frames and microphone audio go to the guest. */
static void on_uplink(void *user, uint8_t codec, const uint8_t *payload, size_t size)
{
    struct app *app = user;

    if (codec == CAST_CODEC_MJPEG) {
        cast_media_camera_jpeg(app->media, payload, size);
    } else if (codec == CAST_CODEC_PCM_S16LE) {
        cast_media_mic_pcm(app->media, payload, size);
    }
}

static void on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}

static void on_control(void *user, const struct cast_encoder_cfg *partial,
                       bool force_idr_only)
{
    struct app *app = user;
    struct cast_encoder_cfg next;

    if (force_idr_only) {
        cast_encoder_force_idr(app->enc);
        return;
    }
    cast_encoder_get_cfg(app->enc, &next);
    if (partial->bitrate > 0) {
        next.bitrate = partial->bitrate;
    }
    if (partial->fps > 0) {
        next.fps = partial->fps;
    }
    if (partial->gop_sec > 0) {
        next.gop_sec = partial->gop_sec;
    }
    if (partial->max_width > 0) {
        next.max_width = partial->max_width;
    }
    if (partial->max_height > 0) {
        next.max_height = partial->max_height;
    }
    if (partial->width >= 0) {
        next.codec = partial->codec;
    }
    if (partial->bitrate > 0 || partial->fps > 0 || partial->max_width > 0 ||
        partial->max_height > 0 || partial->gop_sec > 0 ||
        partial->width >= 0) {
        next.prefer_hw = app->cfg.prefer_hw;
        next.force_encoder = app->cfg.force_encoder;
        if (cast_encoder_reconfigure(app->enc, &next) == 0) {
            app->cfg = next;
            fprintf(stderr, "cast: reconfigured encoder=%s\n",
                    cast_encoder_name(app->enc));
        }
    }
}

static void on_stats(void *user, char *out, size_t out_sz)
{
    struct app *app = user;
    struct cast_encoder_cfg cfg;

    cast_encoder_get_cfg(app->enc, &cfg);
    snprintf(out, out_sz,
             "{\"ok\":true,\"cmd\":\"get_stats\",\"encoder\":\"%s\","
             "\"codec\":\"%s\",\"bitrate\":%lld,\"fps\":%d,\"gop_sec\":%d,"
             "\"frames_in\":%llu,\"packets_out\":%llu}",
             cast_encoder_name(app->enc), cast_codec_name(cfg.codec),
             (long long)cfg.bitrate, cfg.fps, cfg.gop_sec,
             (unsigned long long)app->frames_in,
             (unsigned long long)app->packets_out);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --capture-sock PATH   Unix socket from QEMU (required unless --demo)\n"
            "  --listen HOST:PORT    Encoded stream TCP listen (default 0.0.0.0:9901)\n"
            "  --control HOST:PORT   Control JSON TCP (default 127.0.0.1:9902)\n"
            "  --out FILE            Also write raw bitstream to FILE\n"
            "  --codec NAME          h264|hevc|vp8|vp9|av1 (default h264)\n"
            "  --bitrate N           bits/sec (default 4000000)\n"
            "  --fps N               (default 30)\n"
            "  --max-size WxH        scale clamp\n"
            "  --gop-sec N           keyint seconds (default 2)\n"
            "  --soft                Prefer software encoders\n"
            "  --encoder NAME        Force FFmpeg encoder name\n"
            "  --demo WxH            Synthesize BGRA frames (no QEMU)\n",
            argv0);
}

static int parse_host_port(const char *s, char *host, size_t host_sz, int *port,
                           const char *def_host, int def_port)
{
    const char *colon;

    snprintf(host, host_sz, "%s", def_host);
    *port = def_port;
    if (!s || !s[0]) {
        return 0;
    }
    colon = strrchr(s, ':');
    if (!colon) {
        *port = atoi(s);
        return 0;
    }
    if ((size_t)(colon - s) >= host_sz) {
        return -1;
    }
    memcpy(host, s, (size_t)(colon - s));
    host[colon - s] = '\0';
    *port = atoi(colon + 1);
    return 0;
}

static void emit_packets(struct app *app, struct cast_packet *pkts, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        app->packets_out++;
        if (app->tx && cast_transport_ready(app->tx)) {
            cast_transport_send_packet(app->tx, pkts[i].codec, pkts[i].flags,
                                       pkts[i].pts_us, pkts[i].data,
                                       pkts[i].size);
        }
        if (app->file) {
            cast_file_sink_write(app->file, pkts[i].data, pkts[i].size);
        }
    }
    cast_encoder_free_packets(pkts, n);
}

static int push_frame(struct app *app, const struct cast_frame *fr, uint64_t pts_us,
                      struct cast_packet **pkts, int *n)
{
    if (fr->dmabuf) {
        return cast_encoder_push_dmabuf(app->enc, fr->format, fr->dmabuf_fd, (int)fr->width,
                                        (int)fr->height, (int)fr->stride, pts_us, pkts, n);
    }
    return cast_encoder_push(app->enc, fr->format, fr->data, (int)fr->width,
                             (int)fr->height, (int)fr->stride, pts_us, pkts, n);
}

static int run_demo(struct app *app, int w, int h, int seconds)
{
    uint8_t *bgra = malloc((size_t)w * (size_t)h * 4);
    int frame;
    int total = app->cfg.fps * (seconds > 0 ? seconds : 3);

    if (!bgra) {
        return 1;
    }
    fprintf(stderr, "cast: demo %dx%d %d frames encoder=%s\n", w, h, total,
            cast_encoder_name(app->enc));
    for (frame = 0; frame < total && g_run; frame++) {
        int y, x;
        struct cast_packet *pkts = NULL;
        int n = 0;
        uint8_t v = (uint8_t)(frame * 8);

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4;
                bgra[i + 0] = (uint8_t)(x + v);     /* B */
                bgra[i + 1] = (uint8_t)(y + v);     /* G */
                bgra[i + 2] = (uint8_t)(x ^ y ^ v); /* R */
                bgra[i + 3] = 255;
            }
        }
        if (cast_encoder_push_bgra(app->enc, bgra, w, h, w * 4,
                                   (uint64_t)frame * 1000000ull /
                                       (uint64_t)app->cfg.fps,
                                   &pkts, &n) == 0) {
            emit_packets(app, pkts, n);
        }
        if (app->ctrl) {
            cast_control_poll(app->ctrl);
        }
        if (app->tx) {
            cast_transport_try_accept(app->tx);
        }
        usleep(1000000 / (app->cfg.fps > 0 ? app->cfg.fps : 30));
    }
    free(bgra);
    fprintf(stderr, "cast: demo done packets=%llu\n",
            (unsigned long long)app->packets_out);
    return 0;
}

int main(int argc, char **argv)
{
    struct app app;
    struct cast_capture *cap = NULL;
    char listen_host[128] = "0.0.0.0";
    char control_host[128] = "127.0.0.1";
    char capture_sock[512] = "";
    char out_path[512] = "";
    char encoder_force[64] = "";
    int listen_port = 9901;
    int control_port = 9902;
    int demo_w = 0, demo_h = 0;
    int soft = 0;
    int rc = 1;

    static const struct option opts[] = {
        {"capture-sock", required_argument, 0, 's'},
        {"listen", required_argument, 0, 'l'},
        {"control", required_argument, 0, 'c'},
        {"out", required_argument, 0, 'o'},
        {"codec", required_argument, 0, 'C'},
        {"bitrate", required_argument, 0, 'b'},
        {"fps", required_argument, 0, 'f'},
        {"max-size", required_argument, 0, 'm'},
        {"gop-sec", required_argument, 0, 'g'},
        {"soft", no_argument, 0, 'S'},
        {"encoder", required_argument, 0, 'e'},
        {"demo", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    memset(&app, 0, sizeof(app));
    app.cfg.codec = CAST_CODEC_H264;
    app.cfg.fps = 30;
    app.cfg.bitrate = 4000000;
    app.cfg.gop_sec = 1;
    app.cfg.prefer_hw = true;
    app.cfg.width = 0;
    app.cfg.height = 0;

    for (;;) {
        int c = getopt_long(argc, argv, "h", opts, NULL);

        if (c < 0) {
            break;
        }
        switch (c) {
        case 's':
            snprintf(capture_sock, sizeof(capture_sock), "%s", optarg);
            break;
        case 'l':
            parse_host_port(optarg, listen_host, sizeof(listen_host),
                            &listen_port, "0.0.0.0", 9901);
            break;
        case 'c':
            parse_host_port(optarg, control_host, sizeof(control_host),
                            &control_port, "127.0.0.1", 9902);
            break;
        case 'o':
            snprintf(out_path, sizeof(out_path), "%s", optarg);
            break;
        case 'C':
            app.cfg.codec = cast_codec_from_name(optarg);
            break;
        case 'b':
            app.cfg.bitrate = atoll(optarg);
            break;
        case 'f':
            app.cfg.fps = atoi(optarg);
            break;
        case 'm':
            if (sscanf(optarg, "%dx%d", &app.cfg.max_width,
                       &app.cfg.max_height) != 2) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'g':
            app.cfg.gop_sec = atoi(optarg);
            break;
        case 'S':
            soft = 1;
            app.cfg.prefer_hw = false;
            break;
        case 'e':
            snprintf(encoder_force, sizeof(encoder_force), "%s", optarg);
            app.cfg.force_encoder = encoder_force;
            break;
        case 'd':
            if (sscanf(optarg, "%dx%d", &demo_w, &demo_h) != 2) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'h':
        default:
            usage(argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

    if (!capture_sock[0] && demo_w <= 0) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (demo_w > 0) {
        app.cfg.width = demo_w;
        app.cfg.height = demo_h;
    }

    app.enc = cast_encoder_create(&app.cfg);
    if (!app.enc) {
        fprintf(stderr, "cast: encoder init failed\n");
        return 1;
    }

    app.tx = cast_transport_listen(listen_host, listen_port);
    if (!app.tx) {
        fprintf(stderr, "cast: transport listen failed\n");
        goto out;
    }

    app.ctrl = cast_control_listen(control_host, control_port, on_control,
                                   on_stats, &app);
    if (!app.ctrl) {
        fprintf(stderr, "cast: control listen failed\n");
        goto out;
    }

    if (out_path[0]) {
        app.file = cast_file_sink_open(out_path);
        if (!app.file) {
            fprintf(stderr, "cast: cannot open %s\n", out_path);
            goto out;
        }
    }

    if (demo_w > 0) {
        rc = run_demo(&app, demo_w, demo_h, 3);
        goto out;
    }

    cap = cast_capture_listen_unix(capture_sock);
    if (!cap) {
        fprintf(stderr, "cast: capture listen failed on %s\n", capture_sock);
        goto out;
    }
    {
        /* Guest audio/camera sockets sit next to cast.sock (boot-qemu.sh). */
        char dir[sizeof(capture_sock)];

        snprintf(dir, sizeof(dir), "%s", capture_sock);
        const char *media_dir = dirname(dir);

        app.media = cast_media_open(media_dir);
        /* Host H.264 encoding for the guest Codec2 HAL (venc.sock/res.sock). */
        g_venc = cast_venc_start(media_dir);
        /* Host decoding for the guest Codec2 HAL (vdec.sock/blit.sock). */
        g_vdec = cast_vdec_start(media_dir);
        cast_transport_set_uplink(app.tx, on_uplink, &app);
    }

    fprintf(stderr, "cast: running soft=%d encoder=%s\n", soft,
            cast_encoder_name(app.enc));
    struct cast_frame last;

    memset(&last, 0, sizeof(last));
    while (g_run) {
        struct cast_frame fr;
        int got;

        /* A new viewer cannot decode until the next IDR (SPS/PPS + key).
         * Re-encode the last frame at once: a static screen sends nothing. */
        if (cast_transport_try_accept(app.tx) == 2) {
            cast_encoder_force_idr(app.enc);
            if (last.data || last.dmabuf) {
                struct cast_packet *pkts = NULL;
                int n = 0;

                if (push_frame(&app, &last, 0, &pkts, &n) == 0) {
                    emit_packets(&app, pkts, n);
                }
            }
        }
        cast_control_poll(app.ctrl);
        cast_media_poll(app.media, cast_transport_ready(app.tx), on_audio, &app);
        got = cast_capture_poll_frame(cap, &fr);
        if (got < 0) {
            usleep(2000);
            continue;
        }
        if (got == 0) {
            usleep(1000);
            continue;
        }
        app.frames_in++;
        {
            struct cast_packet *pkts = NULL;
            int n = 0;

            if (push_frame(&app, &fr, fr.pts_us, &pkts, &n) == 0) {
                emit_packets(&app, pkts, n);
            }
            if (fr.dmabuf) {
                /* The encoder is done with it: QEMU may render the next one. */
                cast_capture_ack(cap);
            }
        }
        cast_frame_free(&last);
        last = fr;
    }
    cast_frame_free(&last);
    rc = 0;
    fprintf(stderr, "cast: stop frames_in=%llu packets_out=%llu\n",
            (unsigned long long)app.frames_in,
            (unsigned long long)app.packets_out);

out:
    cast_venc_stop(g_venc);
    cast_vdec_stop(g_vdec);
    cast_media_close(app.media);
    cast_capture_destroy(cap);
    cast_control_destroy(app.ctrl);
    cast_file_sink_close(app.file);
    cast_transport_destroy(app.tx);
    cast_encoder_destroy(app.enc);
    return rc;
}
