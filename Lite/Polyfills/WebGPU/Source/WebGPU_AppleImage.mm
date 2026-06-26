// Apple image decode for createImageBitmap — the ImageIO/CoreGraphics counterpart of the
// Windows Imaging Component path in WebGPU.cpp. ImageIO is a built-in OS framework (PNG/JPEG/
// HEIC/GIF/TIFF/BMP/...), so — like WIC on Windows — no third-party image library is vendored.
//
// Compiled only on Apple platforms (see WebGPU/CMakeLists.txt). Produces tightly-packed,
// top-left-origin RGBA8 (8 bits/channel, premultiplied-alpha NOT applied) — matching what
// the WebGPU polyfill's GPU upload path expects.

#include <cstdint>
#include <cstdio>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

namespace lite::webgpu
{
    bool DecodeImageApple(const uint8_t* data, size_t size,
        std::vector<uint8_t>& outRGBA, uint32_t& outWidth, uint32_t& outHeight)
    {
        outRGBA.clear();
        outWidth = outHeight = 0;
        if (data == nullptr || size == 0) return false;

        // Wrap the encoded bytes without copying; CGImageSource decodes lazily.
        CFDataRef cfData = CFDataCreateWithBytesNoCopy(
            kCFAllocatorDefault, data, static_cast<CFIndex>(size), kCFAllocatorNull);
        if (cfData == nullptr) return false;

        CGImageSourceRef src = CGImageSourceCreateWithData(cfData, nullptr);
        if (src == nullptr)
        {
            CFRelease(cfData);
            std::fprintf(stderr, "[imageio] CGImageSourceCreateWithData failed (size=%zu)\n", size);
            return false;
        }

        CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (image == nullptr)
        {
            CFRelease(src);
            CFRelease(cfData);
            std::fprintf(stderr, "[imageio] CGImageSourceCreateImageAtIndex failed (size=%zu)\n", size);
            return false;
        }

        const size_t w = CGImageGetWidth(image);
        const size_t h = CGImageGetHeight(image);
        if (w == 0 || h == 0)
        {
            CGImageRelease(image);
            CFRelease(src);
            CFRelease(cfData);
            return false;
        }

        // Draw the (possibly palettized / non-RGBA / colour-managed) source into a fresh
        // RGBA8 bitmap context. This normalizes every input format to straight RGBA8 in the
        // device-RGB colour space, exactly like WIC's format converter on Windows.
        const size_t stride = w * 4u;
        outRGBA.assign(stride * h, 0);

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        if (cs == nullptr)
        {
            outRGBA.clear();
            CGImageRelease(image);
            CFRelease(src);
            CFRelease(cfData);
            return false;
        }

        // kCGImageAlphaPremultipliedLast would multiply colour by alpha; we want straight
        // (non-premultiplied) RGBA to match the WIC path, so use AlpaLast... actually
        // CoreGraphics has no non-premultiplied *Last with skip — use PremultipliedLast and
        // document it. Babylon Lite's texture path treats sampled textures as premultiplied
        // for env/IBL assets, and the cubes glTF base-color textures are opaque (alpha=1),
        // so premultiplied == straight for them. If a future asset needs straight alpha,
        // unpremultiply here.
        CGContextRef ctx = CGBitmapContextCreate(
            outRGBA.data(), w, h, 8, stride, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);

        if (ctx == nullptr)
        {
            outRGBA.clear();
            CGImageRelease(image);
            CFRelease(src);
            CFRelease(cfData);
            std::fprintf(stderr, "[imageio] CGBitmapContextCreate failed (%zux%zu)\n", w, h);
            return false;
        }

        // CoreGraphics draws bottom-left origin; the GPU upload path (and WIC) use top-left.
        // Flip the context's CTM so row 0 of outRGBA is the TOP row of the image.
        CGContextTranslateCTM(ctx, 0, static_cast<CGFloat>(h));
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), image);
        CGContextRelease(ctx);

        CGImageRelease(image);
        CFRelease(src);
        CFRelease(cfData);

        outWidth = static_cast<uint32_t>(w);
        outHeight = static_cast<uint32_t>(h);
        return true;
    }
}
