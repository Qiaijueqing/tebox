#include "util/detect_os.h"
 
#include "target-helpers/drm_helper.h"
#include "target-helpers/sw_helper.h"
#include "util/format/u_formats.h"

struct pipe_context;
struct pipe_resource;
struct pipe_video_buffer;
struct pipe_video_buffer *virgl_video_create_buffer_from_resource(
   struct pipe_context *, struct pipe_resource *, enum pipe_format,
   unsigned, unsigned);
PUBLIC struct pipe_video_buffer *gki_virgl_video_create_buffer_from_resource(
   struct pipe_context *, struct pipe_resource *, enum pipe_format,
   unsigned, unsigned);

/* Android's linker does not reliably resolve exported data symbols from a
 * DRI megadriver.  Keep the normal descriptor for Mesa, but expose a tiny
 * function entrypoint for vendor Codec2's Gallium video bridge. */
PUBLIC const struct drm_driver_descriptor *gki_virtio_gpu_driver_descriptor(void);
PUBLIC const struct drm_driver_descriptor *
gki_virtio_gpu_driver_descriptor(void)
{
   return &virtio_gpu_driver_descriptor;
}

PUBLIC struct pipe_video_buffer *
gki_virgl_video_create_buffer_from_resource(struct pipe_context *ctx,
                                            struct pipe_resource *resource,
                                            enum pipe_format format,
                                            unsigned width, unsigned height)
{
   return virgl_video_create_buffer_from_resource(ctx, resource, format, width, height);
}
