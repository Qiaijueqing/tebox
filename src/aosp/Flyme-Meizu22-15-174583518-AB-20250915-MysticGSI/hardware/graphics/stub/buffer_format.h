#pragma once

#include <stdint.h>

// Android PixelFormat -> DRM little-endian memory layout. Keep allocator,
// mapper metadata and display copies in agreement.
struct BufferFormat {
    uint32_t fourcc;
    int bytes;
    int components;
    int offsets[4];  // R, G, B, A
    int bits[4];
};

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}

constexpr int kYv12 = 0x32315659;  // HAL_PIXEL_FORMAT_YV12

inline BufferFormat bufferFormat(int format) {
    switch (format) {
        case 1: return {fourcc('A','B','2','4'), 4, 4, {0,8,16,24}, {8,8,8,8}};
        case 2: return {fourcc('X','B','2','4'), 4, 3, {0,8,16,0}, {8,8,8,0}};
        case 3: return {fourcc('B','G','2','4'), 3, 3, {0,8,16,0}, {8,8,8,0}};
        case 4: return {fourcc('R','G','1','6'), 2, 3, {11,5,0,0}, {5,6,5,0}};
        case 5: return {fourcc('A','R','2','4'), 4, 4, {16,8,0,24}, {8,8,8,8}};
        case 0x16: return {fourcc('A','B','4','H'), 8, 4, {0,16,32,48}, {16,16,16,16}};
        case 0x2b: return {fourcc('A','B','3','2'), 4, 4, {0,10,20,30}, {10,10,10,2}};
        // YUV_420_888 is a multi-plane CPU buffer. Keep it on the ashmem
        // fallback; the VirGL GBM path only supports packed RGB formats.
        case 35: return {0, 1, 3, {0,0,0,0}, {8,8,8,0}};
        // BLOB (camera JPEG): width bytes x 1 row of opaque data, ashmem too.
        case 0x21: return {0, 1, 1, {0,0,0,0}, {8,0,0,0}};
        // YV12 (Codec2 software decoders' output): three planes, see yv12Layout.
        case kYv12: return {fourcc('Y','V','1','2'), 1, 3, {0,0,0,0}, {8,8,8,0}};
        default: return {};
    }
}

// Android YV12: Y, then Cr, then Cb. Y rows are aligned to 32 bytes (Android
// asks for 16; gfxstream's virtio-gpu transfers assume 32) and chroma rows
// are half of that, so the layout is also what gfxstream expects.
struct Yv12Layout {
    uint32_t yStride, cStride, ySize, cSize, size;
};

inline Yv12Layout yv12Layout(uint32_t width, uint32_t height) {
    Yv12Layout l;
    l.yStride = (width + 31) & ~31u;
    l.cStride = l.yStride / 2;
    l.ySize = l.yStride * height;
    l.cSize = l.cStride * (height / 2);
    l.size = l.ySize + 2 * l.cSize;
    return l;
}
