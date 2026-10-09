#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct pipe_context;
struct pipe_screen;
struct pipe_video_buffer;
struct pipe_video_codec;
struct pipe_resource;

// Gallium/VirGL video helper for the vendor Codec2 store.
// Caps are read from virtio-gpu VirGL capset2. A pipe_screen is opened via
// dlopen of virtio_gpu_dri.so (driver_descriptor) when available.

namespace gki {
namespace mesa_video {

enum class Profile {
    kAvcBaseline,
    kAvcMain,
    kAvcHigh,
};

enum class Entrypoint {
    kDecode,
    kEncode,
};

struct Caps {
    bool avc_decode = false;
    bool avc_encode = false;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
};

// Snapshot of host-reported VirGL video support. Cheap after first call.
const Caps& queryCaps();

// True when AVC encode and/or decode is advertised by VirGL video caps,
// or when GKI_MESA_VIDEO_FORCE=1 (bring-up / registration testing).
bool hasAvcDecode();
bool hasAvcEncode();
bool forceVideoEnabled();

// A capset is only an advertisement. This reports whether the guest Gallium
// driver can actually create a pipe_video_codec for the selected entrypoint.
bool backendAvailable(Entrypoint ep);

class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // width/height are coded picture size. Returns false if pipe/video
    // unavailable (caller should fail the Codec2 start()).
    bool open(Entrypoint ep, Profile profile, uint32_t width, uint32_t height);
    void close();

    bool isOpen() const { return mOpen; }
    Entrypoint entrypoint() const { return mEp; }
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }

    // Decode one access unit (Annex-B or length-prefixed AVC). Fills NV12
    // into outNv12 (size must be width*height*3/2). Returns bytes written
    // or <0 on error / no output yet.
    int decode(const uint8_t* bitstream, size_t size, int64_t ptsUs,
               std::vector<uint8_t>* outNv12, int64_t* outPtsUs, bool* outEos);

    // Encode one NV12 frame. Appends bitstream (possibly with CSD on first
    // IDR) into outBs. Returns bytes written or <0.
    int encode(const uint8_t* nv12, size_t nv12Size, int64_t ptsUs, bool keyframe,
               std::vector<uint8_t>* outBs, int64_t* outPtsUs, bool* outEos);
    // Encode a dmabuf-backed RGBA surface without mapping it in the guest.
    int encodeGraphic(int fd, uint32_t width, uint32_t height, uint32_t stride,
                      uint32_t format, int64_t ptsUs, bool keyframe,
                      std::vector<uint8_t>* outBs, int64_t* outPtsUs, bool* outEos);
    uint64_t frameCount() const { return mFrameIndex; }

    // Signal end-of-stream / drain. May emit a final frame into out*.
    int drain(std::vector<uint8_t>* out, int64_t* outPtsUs, bool* outEos);

    void flush();

private:
    bool ensurePipe();
    bool createCodec();

    bool mOpen = false;
    Entrypoint mEp = Entrypoint::kDecode;
    Profile mProfile = Profile::kAvcMain;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mVisibleWidth = 0;
    uint32_t mVisibleHeight = 0;

    void* mDriLib = nullptr;     // dlopen handle
    pipe_screen* mScreen = nullptr;
    pipe_context* mContext = nullptr;
    pipe_video_codec* mCodec = nullptr;
    pipe_video_buffer* mTarget = nullptr;
    pipe_video_buffer* mSource = nullptr;
    pipe_resource* mBitstream = nullptr;
    using CreateExternalBufferFn = pipe_video_buffer* (*)(
            pipe_context*, pipe_resource*, int, unsigned, unsigned);
    CreateExternalBufferFn mCreateExternalBuffer = nullptr;
    bool mSourceExternal = false;

    bool mPipeOk = false;
    bool mGotCsd = false;
    uint32_t mFrameIndex = 0;
};

}  // namespace mesa_video
}  // namespace gki
