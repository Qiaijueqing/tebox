/*
 * tebox: draw a decoded YUV picture into an RGBA dma-buf on the GPU.
 *
 * The host video decoder (tebox-cast vdec) hands over its NV12/P010 picture
 * as dma-bufs; the destination is a guest output buffer's gfxstream
 * ColorBuffer, also a dma-buf. Both are imported as EGLImages and the GPU's
 * external sampler does the colour conversion, so no pixel touches the CPU.
 * Called from virtio-gpu-rutabaga.c on the main loop, with QEMU's EGL
 * display.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/egl-helpers.h"
#include "standard-headers/drm/drm_fourcc.h"

#include <epoxy/gl.h>

struct tebox_yuv_picture {
    uint32_t fourcc;          /* DRM_FORMAT_NV12 / DRM_FORMAT_P010 */
    uint32_t width, height;   /* buffer size */
    uint32_t crop_w, crop_h;  /* visible part, from the top left */
    int fds[2];               /* luma, chroma (may be the same buffer) */
    uint32_t offsets[2], pitches[2];
};

bool tebox_yuv_blit(const struct tebox_yuv_picture *src, int dst_fd, uint32_t dst_offset,
                    uint32_t dst_pitch, uint32_t dst_w, uint32_t dst_h);

static struct {
    EGLContext ctx;
    GLuint program, fbo, src_tex, dst_tex;
    GLint scale_loc;
    bool failed;
} blit;

static const char kVertex[] =
    "attribute vec2 pos;\n"
    "uniform vec2 scale;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    uv = (pos * 0.5 + 0.5) * scale;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char kFragment[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        error_report("tebox-yuv-blit: shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool init_gl(void)
{
    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    GLuint vs, fs;
    GLint ok = 0;

    blit.ctx = eglCreateContext(qemu_egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                                ctx_attrs);
    if (blit.ctx == EGL_NO_CONTEXT) {
        error_report("tebox-yuv-blit: eglCreateContext 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, blit.ctx)) {
        error_report("tebox-yuv-blit: eglMakeCurrent 0x%x", eglGetError());
        return false;
    }
    vs = compile(GL_VERTEX_SHADER, kVertex);
    fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) {
        return false;
    }
    blit.program = glCreateProgram();
    glAttachShader(blit.program, vs);
    glAttachShader(blit.program, fs);
    glBindAttribLocation(blit.program, 0, "pos");
    glLinkProgram(blit.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(blit.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        error_report("tebox-yuv-blit: program link failed");
        return false;
    }
    glUseProgram(blit.program);
    glUniform1i(glGetUniformLocation(blit.program, "tex"), 0);
    blit.scale_loc = glGetUniformLocation(blit.program, "scale");
    glGenFramebuffers(1, &blit.fbo);
    glGenTextures(1, &blit.src_tex);
    glGenTextures(1, &blit.dst_tex);
    return true;
}

static EGLImageKHR import_yuv(const struct tebox_yuv_picture *p)
{
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)p->width,
        EGL_HEIGHT, (EGLint)p->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)p->fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, p->fds[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)p->offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)p->pitches[0],
        EGL_DMA_BUF_PLANE1_FD_EXT, p->fds[1],
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)p->offsets[1],
        EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)p->pitches[1],
        EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
        EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
        EGL_NONE,
    };

    return eglCreateImageKHR(qemu_egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                             attrs);
}

static EGLImageKHR import_rgba(int fd, uint32_t offset, uint32_t pitch, uint32_t w,
                               uint32_t h)
{
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)w,
        EGL_HEIGHT, (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE,
    };

    return eglCreateImageKHR(qemu_egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                             attrs);
}

bool tebox_yuv_blit(const struct tebox_yuv_picture *src, int dst_fd, uint32_t dst_offset,
                    uint32_t dst_pitch, uint32_t dst_w, uint32_t dst_h)
{
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    EGLContext prev_ctx = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    EGLImageKHR src_img = EGL_NO_IMAGE_KHR, dst_img = EGL_NO_IMAGE_KHR;
    bool ok = false;

    if (blit.failed || !qemu_egl_display) {
        return false;
    }
    if (!blit.ctx) {
        if (!init_gl()) {
            blit.failed = true;
            goto restore;
        }
    } else if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               blit.ctx)) {
        goto restore;
    }
    src_img = import_yuv(src);
    dst_img = import_rgba(dst_fd, dst_offset, dst_pitch, dst_w, dst_h);
    if (src_img == EGL_NO_IMAGE_KHR || dst_img == EGL_NO_IMAGE_KHR) {
        error_report_once("tebox-yuv-blit: EGL import failed (0x%x)", eglGetError());
        goto out;
    }
    glBindTexture(GL_TEXTURE_2D, blit.dst_tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)dst_img);
    glBindFramebuffer(GL_FRAMEBUFFER, blit.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blit.dst_tex,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_report_once("tebox-yuv-blit: output buffer not renderable");
        goto out;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, blit.src_tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)src_img);
    glUseProgram(blit.program);
    /* Sample only the visible part. Row 0 of both buffers is GL row 0. */
    glUniform2f(blit.scale_loc, (GLfloat)src->crop_w / src->width,
                (GLfloat)src->crop_h / src->height);
    glViewport(0, 0, (GLsizei)dst_w, (GLsizei)dst_h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    /* The guest samples the buffer as soon as it hears back. */
    glFinish();
    ok = glGetError() == GL_NO_ERROR;
out:
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (src_img != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(qemu_egl_display, src_img);
    }
    if (dst_img != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(qemu_egl_display, dst_img);
    }
restore:
    eglMakeCurrent(qemu_egl_display, prev_draw, prev_read, prev_ctx);
    return ok;
}
