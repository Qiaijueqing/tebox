/* Host video decoder for the guest Codec2 HAL (gfxstream backend).
 * See CAST_VDEC_* in protocol.h. */
#ifndef TEBOX_CAST_VDEC_H
#define TEBOX_CAST_VDEC_H

#ifdef __cplusplus
extern "C" {
#endif

struct cast_vdec;

/* Serves vdec.sock and renders pictures through blit.sock, both in |dir|
 * (next to cast.sock). Runs on its own thread; reconnects when QEMU
 * restarts. NULL where the host has no V4L2 decoder support. */
struct cast_vdec *cast_vdec_start(const char *dir);
void cast_vdec_stop(struct cast_vdec *v);

#ifdef __cplusplus
}
#endif

#endif
