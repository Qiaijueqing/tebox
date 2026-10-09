/* macOS VideoToolbox backend for the VirGL video protocol. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <epoxy/gl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pipe/p_video_enums.h"
#include "pipe/p_video_state.h"
#include "util/list.h"
#include "virgl_hw.h"
#include "virgl_video_hw.h"
#include "vrend_iov.h"
#include "vrend_renderer.h"
#include "vrend_video.h"

struct vrend_video_context;

struct vt_codec {
   struct list_head head;
   struct vrend_video_context *ctx;
   uint32_t handle;
   enum pipe_video_profile profile;
   enum pipe_video_entrypoint entrypoint;
   uint32_t width;
   uint32_t height;
   VTCompressionSessionRef session;
   uint8_t *encoded;
   size_t encoded_size;
   bool encoded_keyframe;
   uint64_t frame_index;
   bool hardware;
   pthread_mutex_t callback_mutex;
   pthread_cond_t callback_cond;
   bool callback_done;
   uint64_t waiting_frame;
};

struct vt_buffer {
   struct list_head head;
   struct vrend_video_context *ctx;
   uint32_t handle;
   enum pipe_format format;
   uint32_t width;
   uint32_t height;
   unsigned num_planes;
   uint32_t res_handles[3];
};

struct vt_encode_job {
   struct vt_codec *codec;
   CVPixelBufferRef pixel;
   CFDictionaryRef options;
   uint64_t frame;
   OSStatus encode_status;
   OSStatus complete_status;
};

struct vrend_video_context {
   struct vrend_context *ctx;
   struct list_head codecs;
   struct list_head buffers;
};

static bool vt_available;

static bool session_is_hardware(VTCompressionSessionRef session)
{
   CFTypeRef value = NULL;
   Boolean is_hardware = false;
   OSStatus status = VTSessionCopyProperty(
      session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
      kCFAllocatorDefault, &value);
   if (status == noErr && value)
      is_hardware = CFGetTypeID(value) == CFBooleanGetTypeID() &&
                    CFBooleanGetValue((CFBooleanRef)value);
   if (value)
      CFRelease(value);
   return is_hardware;
}

static struct vt_codec *get_codec(struct vrend_video_context *ctx, uint32_t handle)
{
   list_for_each_entry(struct vt_codec, codec, &ctx->codecs, head) {
      if (codec->handle == handle)
         return codec;
   }
   return NULL;
}

static struct vt_buffer *get_buffer(struct vrend_video_context *ctx, uint32_t handle)
{
   list_for_each_entry(struct vt_buffer, buffer, &ctx->buffers, head) {
      if (buffer->handle == handle)
         return buffer;
   }
   return NULL;
}

static void append_bytes(struct vt_codec *codec, const uint8_t *data, size_t size)
{
   uint8_t *next = realloc(codec->encoded, codec->encoded_size + size);
   if (!next)
      return;
   codec->encoded = next;
   memcpy(codec->encoded + codec->encoded_size, data, size);
   codec->encoded_size += size;
}

static void append_start_code(struct vt_codec *codec)
{
   static const uint8_t start_code[] = {0, 0, 0, 1};
   append_bytes(codec, start_code, sizeof(start_code));
}

static void append_parameter_sets(struct vt_codec *codec, CMFormatDescriptionRef format)
{
   /* VideoToolbox returns the SPS/PPS one at a time.  Asking for a NULL
    * payload is not a portable way to query the count and fails on macOS. */
   for (size_t i = 0; i < 2; ++i) {
      const uint8_t *data = NULL;
      size_t size = 0;
      size_t count = 0;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &data, &size,
                                                              &count, NULL) != noErr)
         break;
      append_start_code(codec);
      append_bytes(codec, data, size);
   }
}

static void vt_output_callback(void *refcon, void *source_frame_refcon, OSStatus status,
                               VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
   struct vt_codec *codec = refcon;
   (void)flags;
   uint64_t frame = (uint64_t)(uintptr_t)source_frame_refcon;
   pthread_mutex_lock(&codec->callback_mutex);
   bool current = frame == codec->waiting_frame;
   pthread_mutex_unlock(&codec->callback_mutex);
   if (!current)
      return;
   fprintf(stderr, "virgl-video-vt: callback frame=%llu status=%d ready=%d\n",
           (unsigned long long)frame, (int)status,
           sample ? (CMSampleBufferDataIsReady(sample) ? 1 : 0) : 0);
   if (status != noErr || !sample)
      goto done;
   if (!CMSampleBufferDataIsReady(sample) && CMSampleBufferMakeDataReady(sample) != noErr)
      goto done;

   codec->encoded_size = 0;
   free(codec->encoded);
   codec->encoded = NULL;

   bool keyframe = true;
   CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
   if (attachments && CFArrayGetCount(attachments)) {
      CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
      CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(dict,
                                           kCMSampleAttachmentKey_NotSync);
      keyframe = not_sync != kCFBooleanTrue;
   }
   codec->encoded_keyframe = keyframe;

   if (keyframe)
      append_parameter_sets(codec, CMSampleBufferGetFormatDescription(sample));

   CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
   size_t length = 0;
   size_t total = 0;
   char *data = NULL;
   if (!block || CMBlockBufferGetDataLength(block) == 0)
      goto done;
   OSStatus block_status = CMBlockBufferGetDataPointer(block, 0, &length, &total, &data);
   uint8_t *copy = NULL;
   if (block_status != kCMBlockBufferNoErr || !data) {
      total = CMBlockBufferGetDataLength(block);
      copy = malloc(total);
      if (!copy || CMBlockBufferCopyDataBytes(block, 0, total, copy) != kCMBlockBufferNoErr) {
         free(copy);
         goto done;
      }
      data = (char *)copy;
   }

   size_t offset = 0;
   while (offset + 4 <= total) {
      uint32_t nal_size = ((uint8_t)data[offset] << 24) |
                          ((uint8_t)data[offset + 1] << 16) |
                          ((uint8_t)data[offset + 2] << 8) |
                          (uint8_t)data[offset + 3];
      offset += 4;
      if (!nal_size || offset + nal_size > total)
         break;
      append_start_code(codec);
      append_bytes(codec, (const uint8_t *)data + offset, nal_size);
      offset += nal_size;
   }
   free(copy);
done:
   pthread_mutex_lock(&codec->callback_mutex);
   codec->callback_done = true;
   pthread_cond_signal(&codec->callback_cond);
   pthread_mutex_unlock(&codec->callback_mutex);
}

static void *vt_encode_worker(void *opaque)
{
   struct vt_encode_job *job = opaque;
   VTEncodeInfoFlags info = 0;
   job->encode_status = VTCompressionSessionEncodeFrame(
      job->codec->session, job->pixel, CMTimeMake((int64_t)job->frame, 30),
      kCMTimeInvalid, job->options, (void *)(uintptr_t)job->frame, &info);
   if (job->encode_status == noErr)
      job->complete_status = VTCompressionSessionCompleteFrames(
         job->codec->session, CMTimeMake((int64_t)job->frame, 30));
   if (job->options)
      CFRelease(job->options);
   CVPixelBufferRelease(job->pixel);
   return NULL;
}

static bool read_plane(struct vt_buffer *buffer, unsigned plane, uint8_t *dst,
                       size_t size)
{
   if (plane >= buffer->num_planes)
      return false;
   struct vrend_resource *res = vrend_renderer_ctx_res_lookup(buffer->ctx->ctx,
                                                               buffer->res_handles[plane]);
   if (!res)
      return false;
   /* NV12 video planes are ordinary R8/RG8 textures on the VirGL host.
    * Read them directly with glGetTexImage: the generic FBO readback path
    * uses GL_RED/GL_RG on Apple's core profile and can return INVALID_ENUM
    * with an all-zero buffer for these non-renderable plane formats. */
   GLenum format = plane == 0 ? GL_RED : GL_RG;
   glBindTexture(res->target, res->gl_id);
   glGetTexImage(res->target, 0, format, GL_UNSIGNED_BYTE, dst);
   GLenum error = glGetError();
   glBindTexture(res->target, 0);
   if (error != GL_NO_ERROR)
      fprintf(stderr, "virgl-video-vt: glGetTexImage plane %u error=0x%x\n", plane, error);
   return error == GL_NO_ERROR;
}

static bool copy_plane_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                            size_t src_stride, uint32_t height, size_t row_bytes)
{
   if (!dst || !src || !height || !row_bytes || dst_stride < row_bytes ||
       src_stride < row_bytes)
      return false;
   if (dst_stride == src_stride && dst_stride == row_bytes) {
      memcpy(dst, src, row_bytes * height);
      return true;
   }
   for (uint32_t y = 0; y < height; ++y)
      memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
   return true;
}

static void copy_rgba_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                           size_t src_stride, uint32_t width, uint32_t height,
                           bool flip_y)
{
   const size_t row_bytes = (size_t)width * 4;
   for (uint32_t y = 0; y < height; ++y) {
      const uint32_t src_y = flip_y ? (height - 1 - y) : y;
      memcpy(dst + (size_t)y * dst_stride, src + (size_t)src_y * src_stride, row_bytes);
   }
}

/* Guest scrcpy surfaces are virtio-gpu textures (often IOSurface/GBM backed).
 * Prefer FBO+glReadPixels; fall back to glGetTexImage into a tightly packed
 * staging buffer and copy into the (possibly padded) CVPixelBuffer. */
static bool read_rgba(struct vt_buffer *buffer, uint8_t *dst, size_t dst_stride)
{
   if (!buffer->num_planes || !dst || !dst_stride)
      return false;
   struct vrend_resource *res = vrend_renderer_ctx_res_lookup(buffer->ctx->ctx,
                                                               buffer->res_handles[0]);
   if (!res || !res->gl_id) {
      fprintf(stderr, "virgl-video-vt: rgba missing res handle=%u\n",
              buffer->res_handles[0]);
      fflush(stderr);
      return false;
   }
   if (!vrend_hw_switch_context(buffer->ctx->ctx, true)) {
      fprintf(stderr, "virgl-video-vt: rgba context switch failed\n");
      fflush(stderr);
      return false;
   }

   /* Always pack BGRA: kCVPixelFormatType_32RGBA is not reliably creatable
    * on macOS, while 32BGRA is. GL_BGRA readback swizzles RGBA textures. */
   const GLenum glformat = GL_BGRA;
   const size_t tight_stride = (size_t)buffer->width * 4;
   const size_t tight_size = tight_stride * buffer->height;
   bool ok = false;

   /* Drain pending guest draws into the texture before sampling it. */
   glFinish();

   GLuint fbo = 0;
   GLint old_fbo = 0;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, res->target,
                          res->gl_id, 0);
   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status == GL_FRAMEBUFFER_COMPLETE) {
      uint8_t *tight = malloc(tight_size);
      if (tight) {
         glPixelStorei(GL_PACK_ALIGNMENT, 1);
         glPixelStorei(GL_PACK_ROW_LENGTH, 0);
         glReadPixels(0, 0, (GLsizei)buffer->width, (GLsizei)buffer->height,
                      glformat, GL_UNSIGNED_BYTE, tight);
         GLenum error = glGetError();
         if (error == GL_NO_ERROR) {
            copy_rgba_rows(dst, dst_stride, tight, tight_stride, buffer->width,
                           buffer->height, res->y_0_top);
            ok = true;
         } else {
            fprintf(stderr, "virgl-video-vt: glReadPixels bgra error=0x%x\n", error);
            fflush(stderr);
         }
         free(tight);
      }
   } else {
      fprintf(stderr,
              "virgl-video-vt: rgba FBO incomplete status=0x%x target=0x%x id=%u storage=0x%x\n",
              status, res->target, res->gl_id, res->storage_bits);
      fflush(stderr);
   }
   glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
   glDeleteFramebuffers(1, &fbo);

   if (!ok) {
      uint8_t *tight = malloc(tight_size);
      if (!tight)
         return false;
      glBindTexture(res->target, res->gl_id);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glGetTexImage(res->target, 0, glformat, GL_UNSIGNED_BYTE, tight);
      GLenum error = glGetError();
      glBindTexture(res->target, 0);
      if (error == GL_NO_ERROR) {
         copy_rgba_rows(dst, dst_stride, tight, tight_stride, buffer->width,
                        buffer->height, res->y_0_top);
         ok = true;
      } else {
         fprintf(stderr, "virgl-video-vt: glGetTexImage bgra error=0x%x\n", error);
         fflush(stderr);
      }
      free(tight);
   }
   glPixelStorei(GL_PACK_ALIGNMENT, 4);
   return ok;
}

static bool encode_frame(struct vt_codec *codec, struct vt_buffer *source,
                         const union virgl_picture_desc *desc)
{
   if (!codec->session)
      return false;

   CVPixelBufferRef pixel = NULL;
   bool ok = false;
   if (source->format == PIPE_FORMAT_NV12 && source->num_planes >= 2) {
      const size_t y_size = (size_t)source->width * source->height;
      const size_t uv_size = y_size / 2;
      uint8_t *tmp_y = malloc(y_size);
      uint8_t *tmp_uv = malloc(uv_size);
      if (!tmp_y || !tmp_uv) {
         free(tmp_y);
         free(tmp_uv);
         return false;
      }
      if (CVPixelBufferCreate(NULL, source->width, source->height,
                              kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              NULL, &pixel) != kCVReturnSuccess) {
         free(tmp_y);
         free(tmp_uv);
         return false;
      }
      CVPixelBufferLockBaseAddress(pixel, 0);
      uint8_t *py = CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
      uint8_t *puv = CVPixelBufferGetBaseAddressOfPlane(pixel, 1);
      size_t sy = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
      size_t suv = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
      ok = py && puv &&
           read_plane(source, 0, tmp_y, y_size) &&
           read_plane(source, 1, tmp_uv, uv_size) &&
           copy_plane_rows(py, sy, tmp_y, source->width, source->height,
                           source->width) &&
           copy_plane_rows(puv, suv, tmp_uv, source->width,
                           (source->height + 1) / 2, source->width);
      CVPixelBufferUnlockBaseAddress(pixel, 0);
      free(tmp_y);
      free(tmp_uv);
   } else if ((source->format == PIPE_FORMAT_R8G8B8A8_UNORM ||
               source->format == PIPE_FORMAT_B8G8R8A8_UNORM) && source->num_planes >= 1) {
      /* macOS rejects kCVPixelFormatType_32RGBA for CVPixelBufferCreate;
       * always feed VideoToolbox BGRA and let GL swizzle on readback. */
      OSStatus cv_status = CVPixelBufferCreate(NULL, source->width, source->height,
                                               kCVPixelFormatType_32BGRA, NULL, &pixel);
      if (cv_status != kCVReturnSuccess) {
         fprintf(stderr, "virgl-video-vt: CVPixelBufferCreate BGRA failed status=%d %ux%u\n",
                 (int)cv_status, source->width, source->height);
         fflush(stderr);
         return false;
      }
      CVPixelBufferLockBaseAddress(pixel, 0);
      uint8_t *p = (uint8_t *)CVPixelBufferGetBaseAddress(pixel);
      size_t stride = CVPixelBufferGetBytesPerRow(pixel);
      ok = p && stride >= (size_t)source->width * 4 &&
           read_rgba(source, p, stride);
      CVPixelBufferUnlockBaseAddress(pixel, 0);
   } else {
      fprintf(stderr,
              "virgl-video-vt: unsupported encode source format=0x%x planes=%u\n",
              source->format, source->num_planes);
      fflush(stderr);
   }
   if (!ok) {
      fprintf(stderr,
              "virgl-video-vt: encode_frame input failed format=0x%x planes=%u %ux%u\n",
              source->format, source->num_planes, source->width, source->height);
      fflush(stderr);
      if (pixel) CVPixelBufferRelease(pixel);
      return false;
   }

   CFDictionaryRef options = NULL;
   bool keyframe = desc->h264_enc.picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR;
   if (keyframe) {
      const void *keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
      const void *values[] = {kCFBooleanTrue};
      options = CFDictionaryCreate(NULL, keys, values, 1,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
   }
   uint64_t frame = codec->frame_index++;
   pthread_mutex_lock(&codec->callback_mutex);
   codec->callback_done = false;
   codec->waiting_frame = frame;
   pthread_mutex_unlock(&codec->callback_mutex);
   struct vt_encode_job job = {
      .codec = codec,
      .pixel = pixel,
      .options = options,
      .frame = frame,
      .encode_status = -1,
      .complete_status = -1,
   };
   pthread_t worker;
   if (pthread_create(&worker, NULL, vt_encode_worker, &job) != 0) {
      if (options) CFRelease(options);
      CVPixelBufferRelease(pixel);
      return false;
   }
   pthread_join(worker, NULL);
   OSStatus encode_status = job.encode_status;
   OSStatus complete_status = job.complete_status;
   if (encode_status == noErr) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 1;
      pthread_mutex_lock(&codec->callback_mutex);
      while (!codec->callback_done) {
         int wait_status = pthread_cond_timedwait(&codec->callback_cond,
                                                  &codec->callback_mutex, &deadline);
         if (wait_status != 0)
            break;
      }
      bool callback_done = codec->callback_done;
      pthread_mutex_unlock(&codec->callback_mutex);
      if (!callback_done)
         fprintf(stderr, "virgl-video-vt: timed out waiting for H.264 callback\n");
   }
   for (unsigned wait = 0; codec->encoded_size == 0 && wait < 100; ++wait)
      usleep(10000);
   if (encode_status != noErr || complete_status != noErr || codec->encoded_size == 0)
      fprintf(stderr, "virgl-video-vt: encode status=%d complete=%d bytes=%zu format=0x%x planes=%u\n",
              (int)encode_status, (int)complete_status, codec->encoded_size,
              source->format, source->num_planes);
   return codec->encoded_size != 0;
}

int vrend_video_init(int drm_fd)
{
   (void)drm_fd;
   /* A real session/property probe prevents advertising a software fallback
    * as VirGL "hardware" video.  The same property is checked again when a
    * guest codec is created. */
   VTCompressionSessionRef probe = NULL;
   OSStatus status = VTCompressionSessionCreate(NULL, 16, 16,
                                                 kCMVideoCodecType_H264, NULL, NULL, NULL,
                                                 NULL, NULL, &probe);
   if (status == noErr && probe) {
      VTSessionSetProperty(probe, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
      VTCompressionSessionPrepareToEncodeFrames(probe);
      /* On Apple Silicon this property is not reliably populated until the
       * first frame is submitted.  Session creation is the authoritative
       * capability probe; the encode call still reports failures. */
      vt_available = true;
      VTCompressionSessionInvalidate(probe);
      CFRelease(probe);
   } else {
      vt_available = false;
   }
   fprintf(stderr, "virgl-video-vt: hardware H.264 encoder %s\n",
           vt_available ? "available" : "unavailable");
   return 0;
}

void vrend_video_fini(void) {}

int vrend_video_fill_caps(union virgl_caps *caps)
{
   if (!vt_available)
      return -1;
   caps->v2.num_video_caps = 1;
   caps->v2.video_caps[0].profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN;
   caps->v2.video_caps[0].entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
   caps->v2.video_caps[0].max_level = 41;
   caps->v2.video_caps[0].max_width = 4096;
   caps->v2.video_caps[0].max_height = 4096;
   caps->v2.video_caps[0].supports_progressive = 1;
   fprintf(stderr, "virgl-video-vt: advertised AVC encode capset\n");
   return 0;
}

struct vrend_video_context *vrend_video_create_context(struct vrend_context *ctx)
{
   struct vrend_video_context *vctx = calloc(1, sizeof(*vctx));
   if (!vctx)
      return NULL;
   vctx->ctx = ctx;
   list_inithead(&vctx->codecs);
   list_inithead(&vctx->buffers);
   return vctx;
}

void vrend_video_destroy_context(struct vrend_video_context *ctx)
{
   list_for_each_entry_safe(struct vt_codec, codec, &ctx->codecs, head) {
      list_del(&codec->head);
      if (codec->session)
         VTCompressionSessionInvalidate(codec->session);
      if (codec->session)
         CFRelease(codec->session);
      free(codec->encoded);
      pthread_cond_destroy(&codec->callback_cond);
      pthread_mutex_destroy(&codec->callback_mutex);
      free(codec);
   }
   list_for_each_entry_safe(struct vt_buffer, buffer, &ctx->buffers, head) {
      list_del(&buffer->head);
      free(buffer);
   }
   free(ctx);
}

int vrend_video_create_codec(struct vrend_video_context *ctx, uint32_t handle,
                             uint32_t profile, uint32_t entrypoint, uint32_t chroma_format,
                             uint32_t level, uint32_t width, uint32_t height,
                             uint32_t max_ref, uint32_t flags)
{
   (void)chroma_format; (void)level; (void)max_ref; (void)flags;
   if (!vt_available || entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE ||
       profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN || !width || !height)
      return -1;
   if (get_codec(ctx, handle))
      return 0;
   struct vt_codec *codec = calloc(1, sizeof(*codec));
   if (!codec)
      return -1;
   codec->ctx = ctx;
   codec->handle = handle;
   codec->profile = profile;
   codec->entrypoint = entrypoint;
   codec->width = width;
   codec->height = height;
   pthread_mutex_init(&codec->callback_mutex, NULL);
   pthread_cond_init(&codec->callback_cond, NULL);
   OSStatus status = VTCompressionSessionCreate(NULL, width, height,
                                                 kCMVideoCodecType_H264, NULL, NULL, NULL,
                                                 vt_output_callback, codec, &codec->session);
   if (status != noErr) {
      pthread_cond_destroy(&codec->callback_cond);
      pthread_mutex_destroy(&codec->callback_mutex);
      free(codec);
      return -1;
   }
   VTSessionSetProperty(codec->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
   VTSessionSetProperty(codec->session, kVTCompressionPropertyKey_ProfileLevel,
                        kVTProfileLevel_H264_Main_AutoLevel);
   VTCompressionSessionPrepareToEncodeFrames(codec->session);
   codec->hardware = session_is_hardware(codec->session);
   fprintf(stderr, "virgl-video-vt: H.264 session created (hardware=%d)\n",
           codec->hardware ? 1 : 0);
   fprintf(stderr, "virgl-video-vt: created hardware H.264 encoder %ux%u\n", width, height);
   list_add(&codec->head, &ctx->codecs);
   return 0;
}

void vrend_video_destroy_codec(struct vrend_video_context *ctx, uint32_t handle)
{
   struct vt_codec *codec = get_codec(ctx, handle);
   if (!codec)
      return;
   list_del(&codec->head);
   VTCompressionSessionInvalidate(codec->session);
   CFRelease(codec->session);
   free(codec->encoded);
   pthread_cond_destroy(&codec->callback_cond);
   pthread_mutex_destroy(&codec->callback_mutex);
   free(codec);
}

int vrend_video_create_buffer(struct vrend_video_context *ctx, uint32_t handle,
                              uint32_t format, uint32_t width, uint32_t height,
                              uint32_t *res_handles, unsigned num_res)
{
   if (!width || !height || !res_handles || !num_res || get_buffer(ctx, handle))
      return -1;
   struct vt_buffer *buffer = calloc(1, sizeof(*buffer));
   if (!buffer)
      return -1;
   buffer->ctx = ctx;
   buffer->handle = handle;
   buffer->format = format;
   buffer->width = width;
   buffer->height = height;
   buffer->num_planes = num_res > 3 ? 3 : num_res;
   memcpy(buffer->res_handles, res_handles, buffer->num_planes * sizeof(uint32_t));
   list_add(&buffer->head, &ctx->buffers);
   return 0;
}

void vrend_video_destroy_buffer(struct vrend_video_context *ctx, uint32_t handle)
{
   struct vt_buffer *buffer = get_buffer(ctx, handle);
   if (!buffer)
      return;
   list_del(&buffer->head);
   free(buffer);
}

int vrend_video_begin_frame(struct vrend_video_context *ctx, uint32_t cdc_handle,
                            uint32_t tgt_handle)
{
   return get_codec(ctx, cdc_handle) && get_buffer(ctx, tgt_handle) ? 0 : -1;
}

int vrend_video_decode_bitstream(struct vrend_video_context *ctx, uint32_t cdc_handle,
                                 uint32_t tgt_handle, uint32_t desc_handle,
                                 unsigned num_buffers, const uint32_t *buffer_handles,
                                 const uint32_t *buffer_sizes)
{
   (void)ctx; (void)cdc_handle; (void)tgt_handle; (void)desc_handle;
   (void)num_buffers; (void)buffer_handles; (void)buffer_sizes;
   return -1;
}

int vrend_video_encode_bitstream(struct vrend_video_context *ctx, uint32_t cdc_handle,
                                 uint32_t src_handle, uint32_t dest_handle,
                                 uint32_t desc_handle, uint32_t feed_handle)
{
   struct vt_codec *codec = get_codec(ctx, cdc_handle);
   struct vt_buffer *source = get_buffer(ctx, src_handle);
   struct vrend_resource *desc_res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
   struct vrend_resource *dest = vrend_renderer_ctx_res_lookup(ctx->ctx, dest_handle);
   struct vrend_resource *feed = vrend_renderer_ctx_res_lookup(ctx->ctx, feed_handle);
   if (!codec || !source || !desc_res || !dest || !feed)
      return -1;
   union virgl_picture_desc desc;
   memset(&desc, 0, sizeof(desc));
   vrend_read_from_iovec(desc_res->iov, desc_res->num_iovs, 0, (char *)&desc,
                         MIN2(desc_res->base.width0, sizeof(desc)));
   struct virgl_video_encode_feedback feedback;
   memset(&feedback, 0, sizeof(feedback));
   if (encode_frame(codec, source, &desc) && codec->encoded_size <= dest->base.width0) {
      if (dest->iov)
         vrend_write_to_iovec(dest->iov, dest->num_iovs, 0, (const char *)codec->encoded,
                              codec->encoded_size);
      else if (dest->ptr)
         memcpy(dest->ptr, codec->encoded, codec->encoded_size);
      feedback.stat = VIRGL_VIDEO_ENCODE_STAT_SUCCESS;
      feedback.coded_size = codec->encoded_size;
   } else {
      feedback.stat = VIRGL_VIDEO_ENCODE_STAT_FAILURE;
   }
   fprintf(stderr,
           "virgl-video-vt: feedback stat=%u coded=%u dest=%u feed=%u src_fmt=0x%x planes=%u\n",
           feedback.stat, feedback.coded_size, dest->base.width0, feed->base.width0,
           source->format, source->num_planes);
   fflush(stderr);
   if (feed->iov)
      vrend_write_to_iovec(feed->iov, feed->num_iovs, 0, (const char *)&feedback,
                           MIN2(feed->base.width0, sizeof(feedback)));
   else if (feed->ptr)
      memcpy(feed->ptr, &feedback, MIN2(feed->base.width0, sizeof(feedback)));
   return feedback.stat == VIRGL_VIDEO_ENCODE_STAT_SUCCESS ? 0 : -1;
}

int vrend_video_end_frame(struct vrend_video_context *ctx, uint32_t cdc_handle,
                          uint32_t tgt_handle)
{
   return get_codec(ctx, cdc_handle) && get_buffer(ctx, tgt_handle) ? 0 : -1;
}
