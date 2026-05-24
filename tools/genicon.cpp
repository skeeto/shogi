// genicon.cpp - generates the application icon from the in-game artwork:
//   src/shogi.ico  - multi-resolution Windows icon, compiled into the .exe
//   src/shogi.icns - multi-resolution macOS icon, bundled into Shogi.app
//   src/icon.hpp   - embedded RGBA window icon, set via SDL_SetWindowIcon
//
// The picture is the in-game gold-general tile: an up-pointing pentagon - the
// same signed-distance silhouette as ui.cpp's buildTileTextures() - in the
// brighter of the game's two piece face colours so the icon pops, with the 金
// kanji taken from the committed glyphs.hpp atlas.  No font file needed.
//
// Build & run from the repo root:
//   c++ -O2 -std=c++17 -Isrc -Itools -o /tmp/genicon tools/genicon.cpp
//   /tmp/genicon                  # writes src/shogi.{ico,icns} and icon.hpp
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"       // PNG encoder (used by writeIcns)

#include "glyphs.hpp"              // shogi::KANJI_GLYPHS, GLYPH_PIX, GlyphInfo

using namespace shogi;

namespace {

struct RGBA { uint8_t r, g, b, a; };

// --- pentagon signed-distance helpers (verbatim from ui.cpp) ---------------
float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
  float c = vx * vx + vy * vy;
  float t = c > 0 ? std::clamp((wx * vx + wy * vy) / c, 0.f, 1.f) : 0.f;
  float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
  return std::sqrt(dx * dx + dy * dy);
}
bool pointInPoly(float px, float py, const float* xs, const float* ys, int n) {
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++)
    if (((ys[i] > py) != (ys[j] > py)) &&
        (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i]))
      in = !in;
  return in;
}

// Straight-alpha "src colour over dst", src covering a fraction sa of the
// pixel.  src.a is ignored - src is treated as an opaque colour.
RGBA over(RGBA dst, RGBA src, float sa) {
  sa = std::clamp(sa, 0.f, 1.f);
  float da = dst.a / 255.f;
  float oa = sa + da * (1.f - sa);
  if (oa <= 0.f) return RGBA{0, 0, 0, 0};
  auto ch = [&](float s, float d) {
    return uint8_t(std::clamp((s * sa + d * da * (1.f - sa)) / oa, 0.f, 255.f)
                   + 0.5f);
  };
  return RGBA{ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b),
              uint8_t(oa * 255.f + 0.5f)};
}

// Bilinear sample of the 8-bit 金 coverage bitmap, 0..1, 0 outside the bbox.
float sampleGold(float gx, float gy) {
  const GlyphInfo& g = KANJI_GLYPHS[4];          // 4 = gold general (金)
  int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
  float fx = gx - x0, fy = gy - y0;
  auto px = [&](int x, int y) -> float {
    if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0.f;
    return GLYPH_PIX[g.pix + size_t(y) * g.w + x] / 255.f;
  };
  float a = px(x0, y0)     * (1 - fx) + px(x0 + 1, y0)     * fx;
  float b = px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx;
  return a * (1 - fy) + b * fy;
}

// Render the gold-general tile at SxS, straight-alpha RGBA, transparent
// outside the pentagon.
std::vector<RGBA> renderTile(int S) {
  std::vector<RGBA> img(size_t(S) * S, RGBA{0, 0, 0, 0});

  // Up-pointing pentagon - ui.cpp buildTileTextures(), but with a slimmer
  // margin so the piece fills the icon frame.  border/aa are in tile pixels,
  // scaled from the 128px design size.
  const float border = 5.0f * S / 128.f;
  const float aa     = 1.3f * S / 128.f;
  const float mx = 0.070f * S, my = 0.055f * S;
  const float L = mx, R = S - mx, T = my, B = S - my;
  const float sh = (B - T) * 0.34f, in = (R - L) * 0.08f;
  const float xs[5] = {S * 0.5f, R, R - in, L + in, L};
  const float ys[5] = {T, T + sh, B, B, T + sh};

  const RGBA borderCol{62, 44, 18, 255};         // ui.cpp piece border
  const RGBA faceCol  {236, 226, 200, 255};      // ui.cpp brighter face
  const RGBA kanjiCol {38, 26, 12, 255};         // ui.cpp un-promoted kanji

  // 金 placement, proportional to the pentagon and sitting toward the wide
  // bottom end (like drawPiece()), but a little smaller so the strokes keep
  // well clear of the icon's edges.
  const GlyphInfo& g = KANJI_GLYPHS[4];
  const float gh = 0.57f * (B - T);
  const float gw = g.w * (gh / g.h);
  const float gcx = S * 0.5f, gcy = T + 0.56f * (B - T);

  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      float fx = x + 0.5f, fy = y + 0.5f, d = 1e9f;
      for (int e = 0; e < 5; ++e)
        d = std::min(d, segDist(fx, fy, xs[e], ys[e],
                                xs[(e + 1) % 5], ys[(e + 1) % 5]));
      float sd = pointInPoly(fx, fy, xs, ys, 5) ? d : -d;
      float oc = std::clamp(0.5f + sd / aa, 0.f, 1.f);          // pentagon
      float ic = std::clamp(0.5f + (sd - border) / aa, 0.f, 1.f); // inset face

      RGBA p{0, 0, 0, 0};
      p = over(p, borderCol, oc);
      p = over(p, faceCol, ic);

      // 金 glyph - clipped to the face (ic) so a stroke can never spill onto
      // the border or beyond the tile.
      float gx = (fx - (gcx - gw / 2)) / gw * g.w;
      float gy = (fy - (gcy - gh / 2)) / gh * g.h;
      p = over(p, kanjiCol, sampleGold(gx, gy) * ic);

      img[size_t(y) * S + x] = p;
    }
  return img;
}

// Box-average downscale in premultiplied alpha.  S must be a multiple of N.
std::vector<RGBA> downscale(const std::vector<RGBA>& src, int S, int N) {
  std::vector<RGBA> dst(size_t(N) * N);
  int b = S / N;
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float r = 0, g = 0, bl = 0, a = 0;
      for (int sy = 0; sy < b; ++sy)
        for (int sx = 0; sx < b; ++sx) {
          RGBA p = src[size_t(y * b + sy) * S + (x * b + sx)];
          float pa = p.a / 255.f;
          r += p.r * pa; g += p.g * pa; bl += p.b * pa; a += pa;
        }
      RGBA o{0, 0, 0, 0};
      if (a > 0) {
        o.r = uint8_t(std::clamp(r / a, 0.f, 255.f) + 0.5f);
        o.g = uint8_t(std::clamp(g / a, 0.f, 255.f) + 0.5f);
        o.b = uint8_t(std::clamp(bl / a, 0.f, 255.f) + 0.5f);
        o.a = uint8_t(std::clamp(a / (b * b) * 255.f, 0.f, 255.f) + 0.5f);
      }
      dst[size_t(y) * N + x] = o;
    }
  return dst;
}

// --- little-endian byte writers ---------------------------------------------
void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// One icon image as a 32-bpp BMP (DIB): BITMAPINFOHEADER, a bottom-up BGRA
// XOR bitmap, then a 1-bpp AND mask (transparent where alpha is low, so the
// shape is still right on the rare path that ignores the alpha channel).
std::vector<uint8_t> bmpImage(const std::vector<RGBA>& px, int N) {
  std::vector<uint8_t> v;
  put32(v, 40);                          // biSize
  put32(v, uint32_t(N));                 // biWidth
  put32(v, uint32_t(2 * N));             // biHeight = 2*N (XOR + AND)
  put16(v, 1);                           // biPlanes
  put16(v, 32);                          // biBitCount
  put32(v, 0);                           // biCompression = BI_RGB
  put32(v, 0);                           // biSizeImage
  put32(v, 0); put32(v, 0);              // X/Y pixels-per-metre
  put32(v, 0); put32(v, 0);              // biClrUsed / biClrImportant
  for (int y = N - 1; y >= 0; --y)       // XOR bitmap, bottom-up, BGRA
    for (int x = 0; x < N; ++x) {
      RGBA p = px[size_t(y) * N + x];
      v.push_back(p.b); v.push_back(p.g); v.push_back(p.r); v.push_back(p.a);
    }
  int stride = ((N + 31) / 32) * 4;      // AND mask, 1 bpp, 4-byte rows
  for (int y = N - 1; y >= 0; --y) {
    std::vector<uint8_t> row(size_t(stride), 0);
    for (int x = 0; x < N; ++x)
      if (px[size_t(y) * N + x].a < 128)
        row[x / 8] |= uint8_t(0x80 >> (x % 8));
    v.insert(v.end(), row.begin(), row.end());
  }
  return v;
}

void writeIco(const char* path, const std::vector<int>& sizes,
              const std::vector<std::vector<RGBA>>& icons) {
  std::vector<std::vector<uint8_t>> imgs;
  for (size_t i = 0; i < sizes.size(); ++i)
    imgs.push_back(bmpImage(icons[i], sizes[i]));

  std::vector<uint8_t> f;
  put16(f, 0); put16(f, 1);                          // reserved, type = icon
  put16(f, uint16_t(sizes.size()));                  // image count
  uint32_t offset = 6 + 16 * uint32_t(sizes.size());
  for (size_t i = 0; i < sizes.size(); ++i) {
    int n = sizes[i];
    f.push_back(n >= 256 ? 0 : uint8_t(n));           // bWidth  (0 == 256)
    f.push_back(n >= 256 ? 0 : uint8_t(n));           // bHeight
    f.push_back(0);                                   // bColorCount
    f.push_back(0);                                   // bReserved
    put16(f, 1);                                      // wPlanes
    put16(f, 32);                                     // wBitCount
    put32(f, uint32_t(imgs[i].size()));               // dwBytesInRes
    put32(f, offset);                                 // dwImageOffset
    offset += uint32_t(imgs[i].size());
  }
  for (const auto& im : imgs) f.insert(f.end(), im.begin(), im.end());

  FILE* fp = std::fopen(path, "wb");
  if (!fp) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
  std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  std::printf("%s  (%zu bytes, sizes", path, f.size());
  for (int n : sizes) std::printf(" %d", n);
  std::printf(")\n");
}

// --- macOS .icns ----------------------------------------------------------
// Modern icns is a container of typed entries; each entry holds a PNG-encoded
// icon for one specific size.  Reference: Apple Icon Image format.

// Encode an RGBA buffer as PNG into memory, via stb_image_write's callback.
static void pngAppend(void* user, void* data, int size) {
  auto* v = static_cast<std::vector<uint8_t>*>(user);
  uint8_t* p = static_cast<uint8_t*>(data);
  v->insert(v->end(), p, p + size);
}
std::vector<uint8_t> rgbaToPng(const std::vector<RGBA>& px, int N) {
  std::vector<uint8_t> out;
  stbi_write_png_to_func(&pngAppend, &out, N, N, 4,
                         reinterpret_cast<const void*>(px.data()), N * 4);
  return out;
}

// Big-endian 32-bit append (the icns header is all big-endian).
void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

// One {type-code, side-length} entry.  Both 1x and 2x retina codes are
// included for the sizes the OS actually asks for (32px upward); macOS picks
// the right one for the display it's drawing on.
struct IcnsEntry { const char* type; int size; };

void writeIcns(const char* path, const std::vector<IcnsEntry>& entries) {
  std::vector<uint8_t> f;
  f.push_back('i'); f.push_back('c'); f.push_back('n'); f.push_back('s');
  be32(f, 0);                                // total length, patched below

  for (const auto& e : entries) {
    int n = e.size;
    auto rgba = downscale(renderTile(2 * n), 2 * n, n);   // 2x supersample
    auto png  = rgbaToPng(rgba, n);
    f.push_back(e.type[0]); f.push_back(e.type[1]);
    f.push_back(e.type[2]); f.push_back(e.type[3]);
    be32(f, uint32_t(png.size() + 8));        // entry length includes header
    f.insert(f.end(), png.begin(), png.end());
  }

  uint32_t total = uint32_t(f.size());
  f[4] = uint8_t(total >> 24); f[5] = uint8_t(total >> 16);
  f[6] = uint8_t(total >> 8);  f[7] = uint8_t(total);

  FILE* fp = std::fopen(path, "wb");
  if (!fp) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
  std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  std::printf("%s  (%zu bytes, %zu entries)\n", path, f.size(), entries.size());
}

void writeIconHeader(const char* path, const std::vector<RGBA>& px, int N) {
  FILE* fp = std::fopen(path, "w");
  if (!fp) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
  std::fprintf(fp,
    "// icon.hpp - GENERATED by tools/genicon.cpp.  Do not edit.\n"
    "// %dx%d RGBA window icon (the gold-general piece), uploaded with\n"
    "// SDL_SetWindowIcon at startup.\n"
    "#pragma once\n#include <cstdint>\n\nnamespace shogi {\n\n"
    "constexpr int ICON_W = %d, ICON_H = %d;\n\n"
    "inline const uint8_t ICON_RGBA[%d * %d * 4] = {\n", N, N, N, N, N, N);
  size_t n = size_t(N) * N;
  for (size_t i = 0; i < n; ++i) {
    std::fprintf(fp, "%u,%u,%u,%u,", px[i].r, px[i].g, px[i].b, px[i].a);
    if ((i & 7) == 7) std::fprintf(fp, "\n");
  }
  std::fprintf(fp, "\n};\n\n}  // namespace shogi\n");
  std::fclose(fp);
  std::printf("%s  (%dx%d RGBA)\n", path, N, N);
}

}  // namespace

int main() {
  // Render each size at 2x and box-downscale it.  The 金 comes from a 60px
  // atlas bitmap; rendering near its native size (rather than blowing one
  // big master up) keeps the strokes crisp.

  // --- Windows .ico + embedded window icon ---
  const std::vector<int> winSizes = {16, 32, 48, 64};
  std::vector<std::vector<RGBA>> icons;
  for (int n : winSizes)
    icons.push_back(downscale(renderTile(2 * n), 2 * n, n));
  writeIco("src/shogi.ico", winSizes, icons);
  writeIconHeader("src/icon.hpp", icons.back(), 64);   // window icon = 64px

  // --- macOS .icns ---
  // Standard modern sizes.  ic07-ic10 are 1x; ic11-ic14 are 2x retina
  // variants of 16/32/128/256 respectively, and they're what the task
  // switcher actually picks up on a retina display.  icp4/icp5/icp6 are
  // older 1x small sizes still useful as fallbacks.
  writeIcns("src/shogi.icns", {
    {"icp4",   16}, {"icp5",   32}, {"icp6",  64},
    {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024},
    {"ic11",  32}, {"ic12",  64}, {"ic13", 256}, {"ic14", 512},
  });
  return 0;
}
