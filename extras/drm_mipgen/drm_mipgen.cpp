// drm_mipgen: rebuilds mip chains for texture packs whose DRM files were
// repacked without them, and optionally caps texture resolution.
//
// Legend DRM layout (version 14, little endian):
//   u32 version, u32 sectionCount
//   section headers [sectionCount], 20 bytes each:
//     u32 size, u8 type, u8 pad, u16 versionId, u32 packed (relocCount << 8),
//     u32 id, u32 specMask
//   then, per section in order: relocCount * 8 bytes of relocations, then
//   `size` bytes of data. No padding.
// Relocations address (section index, offset in section), so resizing one
// section leaves every other section's data valid.
//
// Texture sections (type 5) are a 24-byte PCD9 header followed by the levels:
//   char magic[4] "PCD9", u32 d3dFormat, u32 dataSize, u32 unknown,
//   u16 width, u16 height, u8 unknown, u8 mipCount (levels - 1), u16 flags
//
// The reference table (one line per texture id: "id fmtHex w h mips flags")
// comes from the unmodded game's bigfiles. A texture gets a mip chain only if
// its original had one, so textures the game shipped without mips (UI and the
// like) keep a single level.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t FMT_DXT1 = 0x31545844;
constexpr uint32_t FMT_DXT5 = 0x35545844;
constexpr uint32_t SECTION_TEXTURE = 5;
constexpr size_t PCD9_HEADER = 24;

struct RefTex { uint32_t fmt; int w, h, mips; };

struct Options {
    int maxSize = 0;          // 0 = no cap
    std::map<uint32_t, RefTex> ref;
};

// ---------------------------------------------------------------- images

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;    // RGBA, 0..255
    float* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

// 2x2 box filter. For punch-through alpha the colour is weighted by alpha so
// the black under transparent texels doesn't bleed into the edges.
Image downsample(const Image& src, bool alphaWeighted) {
    Image dst;
    dst.w = std::max(1, src.w / 2);
    dst.h = std::max(1, src.h / 2);
    dst.px.resize(size_t(dst.w) * dst.h * 4);
    for (int y = 0; y < dst.h; y++) {
        for (int x = 0; x < dst.w; x++) {
            int x0 = std::min(2 * x, src.w - 1), x1 = std::min(2 * x + 1, src.w - 1);
            int y0 = std::min(2 * y, src.h - 1), y1 = std::min(2 * y + 1, src.h - 1);
            const float* s[4] = { src.at(x0, y0), src.at(x1, y0), src.at(x0, y1), src.at(x1, y1) };
            float* d = dst.at(x, y);
            float a = 0, wsum = 0, c[3] = {};
            for (auto* p : s) {
                float wt = alphaWeighted ? p[3] : 1.0f;
                for (int k = 0; k < 3; k++) c[k] += p[k] * wt;
                wsum += wt;
                a += p[3];
            }
            if (wsum <= 0) {
                for (int k = 0; k < 3; k++) c[k] = (s[0][k] + s[1][k] + s[2][k] + s[3][k]);
                wsum = 4;
            }
            for (int k = 0; k < 3; k++) d[k] = c[k] / wsum;
            d[3] = a / 4;
        }
    }
    return dst;
}

// ---------------------------------------------------------------- BC decode

void unpack565(uint16_t c, int out[3]) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

void colorPalette(uint16_t c0, uint16_t c1, bool allowThreeColor, int pal[4][4]) {
    unpack565(c0, pal[0]);
    unpack565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !allowThreeColor) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
}

void decodeColorBlock(const uint8_t* b, bool dxt1, uint8_t out[16][4]) {
    uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    int pal[4][4];
    colorPalette(c0, c1, dxt1, pal);
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) {
        int* p = pal[(idx >> (2 * i)) & 3];
        for (int k = 0; k < 4; k++) out[i][k] = uint8_t(p[k]);
    }
}

void alphaPalette(int a0, int a1, int pal[8]) {
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; i++) pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (int i = 2; i < 6; i++) pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
}

void decodeAlphaBlock(const uint8_t* b, uint8_t out[16][4]) {
    int pal[8];
    alphaPalette(b[0], b[1], pal);
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i][3] = uint8_t(pal[(bits >> (3 * i)) & 7]);
}

size_t blockBytes(uint32_t fmt) { return fmt == FMT_DXT1 ? 8 : 16; }

size_t levelBytes(uint32_t fmt, int w, int h) {
    return size_t(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * blockBytes(fmt);
}

Image decodeLevel(const uint8_t* data, uint32_t fmt, int w, int h) {
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(size_t(w) * h * 4);
    int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    const uint8_t* p = data;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t texels[16][4];
            if (fmt == FMT_DXT1) {
                decodeColorBlock(p, true, texels);
            } else {
                decodeColorBlock(p + 8, false, texels);
                decodeAlphaBlock(p, texels);
            }
            p += blockBytes(fmt);
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h)
                    for (int k = 0; k < 4; k++) img.at(x, y)[k] = texels[i][k];
            }
        }
    }
    return img;
}

// ---------------------------------------------------------------- BC encode

uint16_t pack565(const float c[3]) {
    int r = std::clamp(int(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
    int g = std::clamp(int(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
    int b = std::clamp(int(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
    return uint16_t((r << 11) | (g << 5) | b);
}

float colorDist(const float a[3], const int b[3]) {
    // Rough luminance weighting; green errors are the most visible.
    float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
}

// Chooses indices for fixed endpoints. Returns the total error and fills idx.
float fitIndices(const float px[16][4], const bool use[16], uint16_t c0, uint16_t c1,
                 bool threeColor, uint32_t& idx) {
    int pal[4][4];
    colorPalette(c0, c1, true, pal);
    int count = threeColor ? 3 : 4;
    float err = 0;
    idx = 0;
    for (int i = 0; i < 16; i++) {
        if (!use[i]) {
            idx |= 3u << (2 * i);   // transparent (three-colour mode) or ignored padding
            continue;
        }
        float best = 1e30f;
        int bi = 0;
        for (int j = 0; j < count; j++) {
            float d = colorDist(px[i], pal[j]);
            if (d < best) { best = d; bi = j; }
        }
        idx |= uint32_t(bi) << (2 * i);
        err += best;
    }
    return err;
}

void writeColorBlock(uint8_t* out, uint16_t c0, uint16_t c1, uint32_t idx) {
    out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
    for (int i = 0; i < 4; i++) out[4 + i] = uint8_t(idx >> (8 * i));
}

// Principal-axis endpoint fit followed by least-squares refinement.
// `opaque[i]` marks texels that must be matched; with `transparent` set the
// rest are encoded as the three-colour mode's transparent index.
void encodeColorBlock(const float px[16][4], const bool opaque[16], bool transparent,
                      bool padded[16], uint8_t* out) {
    bool use[16];
    int n = 0;
    float mean[3] = {};
    for (int i = 0; i < 16; i++) {
        use[i] = opaque[i] && !padded[i];
        if (use[i]) { n++; for (int k = 0; k < 3; k++) mean[k] += px[i][k]; }
    }
    if (n == 0) {
        writeColorBlock(out, 0, 0, 0xFFFFFFFFu);   // all transparent / empty
        return;
    }
    for (int k = 0; k < 3; k++) mean[k] /= n;

    float cov[6] = {};
    for (int i = 0; i < 16; i++) {
        if (!use[i]) continue;
        float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    float axis[3] = { 1, 1, 1 };
    for (int it = 0; it < 8; it++) {
        float v[3] = {
            cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
            cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
            cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2],
        };
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-6f) break;
        for (int k = 0; k < 3; k++) axis[k] = v[k] / len;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; i++) {
        if (!use[i]) continue;
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) {
        e0[k] = std::clamp(mean[k] + axis[k] * hi, 0.0f, 255.0f);
        e1[k] = std::clamp(mean[k] + axis[k] * lo, 0.0f, 255.0f);
    }

    uint16_t bestC0 = 0, bestC1 = 0;
    uint32_t bestIdx = 0;
    float bestErr = 1e30f;
    // Three-colour mode needs c0 <= c1; four-colour needs c0 > c1.
    auto tryEndpoints = [&](const float a[3], const float b[3]) {
        uint16_t p = pack565(a), q = pack565(b);
        uint16_t c0, c1;
        if (transparent) { c0 = std::min(p, q); c1 = std::max(p, q); }
        else {
            if (p == q) {
                // Equal endpoints can't express four-colour mode; nudge one.
                if (q > 0) q--; else p++;
            }
            c0 = std::max(p, q); c1 = std::min(p, q);
        }
        uint32_t idx;
        float err = fitIndices(px, use, c0, c1, transparent, idx);
        if (err < bestErr) { bestErr = err; bestC0 = c0; bestC1 = c1; bestIdx = idx; }
    };
    tryEndpoints(e0, e1);

    for (int iter = 0; iter < 3; iter++) {
        // Solve for endpoints given the current indices (weights per index).
        static const float w4[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
        static const float w3[4] = { 1.0f, 0.0f, 0.5f, 0.0f };
        const float* wt = transparent ? w3 : w4;
        float aa = 0, ab = 0, bb = 0, ax[3] = {}, bx[3] = {};
        for (int i = 0; i < 16; i++) {
            if (!use[i]) continue;
            int j = (bestIdx >> (2 * i)) & 3;
            float a = wt[j], b = 1.0f - a;
            aa += a * a; ab += a * b; bb += b * b;
            for (int k = 0; k < 3; k++) { ax[k] += a * px[i][k]; bx[k] += b * px[i][k]; }
        }
        float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        float n0[3], n1[3];
        for (int k = 0; k < 3; k++) {
            n0[k] = std::clamp((ax[k] * bb - bx[k] * ab) / det, 0.0f, 255.0f);
            n1[k] = std::clamp((bx[k] * aa - ax[k] * ab) / det, 0.0f, 255.0f);
        }
        float before = bestErr;
        tryEndpoints(n0, n1);
        if (bestErr >= before) break;
    }
    writeColorBlock(out, bestC0, bestC1, bestIdx);
}

void encodeAlphaBlock(const float px[16][4], const bool padded[16], uint8_t* out) {
    int vals[16], lo = 255, hi = 0, loInner = 255, hiInner = 0;
    for (int i = 0; i < 16; i++) {
        vals[i] = std::clamp(int(std::lround(px[i][3])), 0, 255);
        if (padded[i]) continue;
        lo = std::min(lo, vals[i]); hi = std::max(hi, vals[i]);
        if (vals[i] > 0) loInner = std::min(loInner, vals[i]);
        if (vals[i] < 255) hiInner = std::max(hiInner, vals[i]);
    }
    if (lo > hi) lo = hi = 255;

    auto fit = [&](int a0, int a1, uint64_t& bits) {
        int pal[8];
        alphaPalette(a0, a1, pal);
        int err = 0;
        bits = 0;
        for (int i = 0; i < 16; i++) {
            int best = 1 << 30, bi = 0;
            for (int j = 0; j < 8; j++) {
                int d = std::abs(vals[i] - pal[j]);
                if (d < best) { best = d; bi = j; }
            }
            bits |= uint64_t(bi) << (3 * i);
            if (!padded[i]) err += best * best;
        }
        return err;
    };
    uint64_t bits8, bits6 = 0;
    // Eight-value mode (a0 > a1); a flat block falls into six-value mode with
    // every index on a0, which is still exact.
    int a0 = hi, a1 = lo;
    int err8 = fit(a0, a1, bits8);
    int err6 = 1 << 30;
    int b0 = 0, b1 = 0;
    if (loInner <= hiInner) {
        // Six-value mode with exact 0 and 255 available.
        b0 = loInner; b1 = hiInner;
        if (b0 == b1) { if (b1 < 255) b1++; else b0--; }
        err6 = fit(std::min(b0, b1), std::max(b0, b1), bits6);
    }
    uint64_t bits;
    if (err6 < err8) { out[0] = uint8_t(std::min(b0, b1)); out[1] = uint8_t(std::max(b0, b1)); bits = bits6; }
    else { out[0] = uint8_t(a0); out[1] = uint8_t(a1); bits = bits8; }
    for (int i = 0; i < 6; i++) out[2 + i] = uint8_t(bits >> (8 * i));
}

std::vector<uint8_t> encodeLevel(const Image& img, uint32_t fmt) {
    int bw = std::max(1, (img.w + 3) / 4), bh = std::max(1, (img.h + 3) / 4);
    std::vector<uint8_t> out(size_t(bw) * bh * blockBytes(fmt));
    uint8_t* p = out.data();
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            float px[16][4];
            bool padded[16], opaque[16];
            bool anyTransparent = false;
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                padded[i] = x >= img.w || y >= img.h;
                const float* s = img.at(std::min(x, img.w - 1), std::min(y, img.h - 1));
                for (int k = 0; k < 4; k++) px[i][k] = s[k];
                opaque[i] = fmt != FMT_DXT1 || s[3] >= 128.0f;
                if (!opaque[i] && !padded[i]) anyTransparent = true;
            }
            if (fmt == FMT_DXT1) {
                encodeColorBlock(px, opaque, anyTransparent, padded, p);
            } else {
                encodeAlphaBlock(px, padded, p);
                bool all[16];
                std::fill(all, all + 16, true);
                encodeColorBlock(px, all, false, padded, p + 8);
            }
            p += blockBytes(fmt);
        }
    }
    return out;
}

// ---------------------------------------------------------------- textures

struct Stats {
    std::atomic<int> rebuilt{0}, capped{0}, sliced{0}, untouched{0};
    std::atomic<long long> bytesIn{0}, bytesOut{0};
};

int fullChainLevels(int w, int h) {
    int levels = 1;
    while (w > 1 || h > 1) { w = std::max(1, w / 2); h = std::max(1, h / 2); levels++; }
    return levels;
}

// Returns the new section data, or an empty vector to keep the original.
std::vector<uint8_t> processTexture(const uint8_t* sec, size_t size, uint32_t id,
                                    const Options& opt, Stats& st) {
    if (size < PCD9_HEADER || memcmp(sec, "PCD9", 4) != 0) return {};
    uint32_t fmt, dataSize;
    uint16_t w, h;
    memcpy(&fmt, sec + 4, 4);
    memcpy(&dataSize, sec + 8, 4);
    memcpy(&w, sec + 16, 2);
    memcpy(&h, sec + 18, 2);
    int mips = sec[21];
    if (fmt != FMT_DXT1 && fmt != FMT_DXT5) { st.untouched++; return {}; }
    if (PCD9_HEADER + dataSize > size || w == 0 || h == 0) { st.untouched++; return {}; }

    bool wantMips = mips > 0;
    auto r = opt.ref.find(id);
    if (r != opt.ref.end()) wantMips = r->second.mips > 0;

    // Count the levels actually present, then decide how many to drop.
    int present = mips + 1;
    int skip = 0, tw = w, th = h;
    while (opt.maxSize > 0 && std::max(tw, th) > opt.maxSize && std::max(tw, th) > 4) {
        tw = std::max(1, tw / 2); th = std::max(1, th / 2); skip++;
    }
    bool hasChain = present > 1;
    if (skip == 0 && (!wantMips || hasChain)) { st.untouched++; return {}; }

    std::vector<uint8_t> levels;
    int outLevels = 0;
    if (hasChain && skip < present) {
        // Existing chain: drop the top levels, keep the rest as-is.
        size_t off = 0;
        int lw = w, lh = h;
        for (int i = 0; i < skip; i++) {
            off += levelBytes(fmt, lw, lh);
            lw = std::max(1, lw / 2); lh = std::max(1, lh / 2);
        }
        levels.assign(sec + PCD9_HEADER + off, sec + PCD9_HEADER + dataSize);
        outLevels = present - skip;
        st.sliced++;
    } else {
        Image img = decodeLevel(sec + PCD9_HEADER, fmt, w, h);
        bool alphaWeighted = fmt == FMT_DXT1;
        for (int i = 0; i < skip; i++) img = downsample(img, alphaWeighted);
        if (skip == 0) {
            levels.assign(sec + PCD9_HEADER, sec + PCD9_HEADER + levelBytes(fmt, w, h));
        } else {
            levels = encodeLevel(img, fmt);
        }
        outLevels = 1;
        if (wantMips) {
            int total = fullChainLevels(img.w, img.h);
            for (int i = 1; i < total; i++) {
                img = downsample(img, alphaWeighted);
                std::vector<uint8_t> enc = encodeLevel(img, fmt);
                levels.insert(levels.end(), enc.begin(), enc.end());
            }
            outLevels = total;
        }
        if (skip) st.capped++;
        if (wantMips) st.rebuilt++;
    }

    std::vector<uint8_t> out(PCD9_HEADER + levels.size());
    memcpy(out.data(), sec, PCD9_HEADER);
    uint32_t newSize = uint32_t(levels.size());
    uint16_t nw = uint16_t(tw), nh = uint16_t(th);
    memcpy(&out[8], &newSize, 4);
    memcpy(&out[16], &nw, 2);
    memcpy(&out[18], &nh, 2);
    out[21] = uint8_t(outLevels - 1);
    memcpy(out.data() + PCD9_HEADER, levels.data(), levels.size());
    st.bytesIn += static_cast<long long>(size);
    st.bytesOut += static_cast<long long>(out.size());
    return out;
}

// ---------------------------------------------------------------- DRM files

struct Section {
    uint32_t size, packed, id;
    uint8_t type;
    size_t relocOff, dataOff;
};

// Paths are printed as UTF-8 (main switches the console to that code page), so
// folders with non-ASCII characters in them read correctly.
std::string u8(const fs::path& p) { return p.u8string(); }   // C++17: u8string is a std::string

bool readFile(const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    data.resize(size_t(f.tellg()));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(data.data()), data.size()));
}

bool processDrm(const fs::path& inPath, const fs::path& outPath, const Options& opt, Stats& st) {
    std::vector<uint8_t> d;
    if (!readFile(inPath, d)) { fprintf(stderr, "  cannot read %s\n", u8(inPath).c_str()); return false; }
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; };
    if (d.size() < 8 || u32(0) != 14) { fprintf(stderr, "  %s: not a v14 DRM\n", u8(inPath).c_str()); return false; }
    uint32_t n = u32(4);
    if (8 + size_t(n) * 20 > d.size()) { fprintf(stderr, "  %s: bad section count\n", u8(inPath).c_str()); return false; }

    std::vector<Section> secs(n);
    size_t off = 8 + size_t(n) * 20;
    for (uint32_t i = 0; i < n; i++) {
        size_t h = 8 + size_t(i) * 20;
        Section& s = secs[i];
        s.size = u32(h);
        s.type = d[h + 4];
        s.packed = u32(h + 8);
        s.id = u32(h + 12);
        s.relocOff = off;
        off += size_t(s.packed >> 8) * 8;
        s.dataOff = off;
        off += s.size;
    }
    if (off != d.size()) {
        fprintf(stderr, "  %s: layout mismatch (%zu vs %zu bytes), skipped\n", u8(inPath).c_str(), off, d.size());
        return false;
    }

    std::vector<std::vector<uint8_t>> replaced(n);
    std::vector<uint32_t> jobs;
    for (uint32_t i = 0; i < n; i++)
        if (secs[i].type == SECTION_TEXTURE) jobs.push_back(i);

    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t j; (j = next++) < jobs.size();) {
            const Section& s = secs[jobs[j]];
            replaced[jobs[j]] = processTexture(&d[s.dataOff], s.size, s.id, opt, st);
        }
    };
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    std::ofstream o(outPath, std::ios::binary);
    if (!o) { fprintf(stderr, "  cannot write %s\n", u8(outPath).c_str()); return false; }
    std::vector<uint8_t> header(d.begin(), d.begin() + 8 + size_t(n) * 20);
    for (uint32_t i = 0; i < n; i++) {
        if (replaced[i].empty()) continue;
        uint32_t sz = uint32_t(replaced[i].size());
        memcpy(&header[8 + size_t(i) * 20], &sz, 4);
    }
    o.write(reinterpret_cast<const char*>(header.data()), header.size());
    for (uint32_t i = 0; i < n; i++) {
        const Section& s = secs[i];
        o.write(reinterpret_cast<const char*>(&d[s.relocOff]), s.dataOff - s.relocOff);
        if (replaced[i].empty())
            o.write(reinterpret_cast<const char*>(&d[s.dataOff]), s.size);
        else
            o.write(reinterpret_cast<const char*>(replaced[i].data()), replaced[i].size());
    }
    // Close before checking so a failure while flushing (disk full) is seen.
    o.close();
    if (!o) { fprintf(stderr, "  write failed for %s (disk full?)\n", u8(outPath).c_str()); return false; }
    return true;
}

bool loadRef(const fs::path& path, Options& opt) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    unsigned id, fmt, flags;
    int w, h, mips;
    while (fscanf(f, "%u %x %d %d %d %u", &id, &fmt, &w, &h, &mips, &flags) == 6) {
        // The table stores the format as the bytes in file order.
        uint32_t le = ((fmt & 0xFF) << 24) | ((fmt & 0xFF00) << 8) | ((fmt >> 8) & 0xFF00) | (fmt >> 24);
        opt.ref[id] = { le, w, h, mips };
    }
    fclose(f);
    return true;
}

void printHelp() {
    printf(
        "drm_mipgen - adds the missing mipmaps to an HD texture pack's .drm files\n"
        "so distant textures stop shimmering. It can also cap the texture size to\n"
        "save memory. The original files are never changed; converted copies are\n"
        "written to the output folder.\n"
        "\n"
        "Usage:\n"
        "  drm_mipgen --out <folder> [--max <px>] [--ref <origtex.txt>] [--force] <input>...\n"
        "\n"
        "  <input>   a .drm file, or a folder (searched, including subfolders, for .drm files)\n"
        "  --out     folder for the converted files (created if missing, must differ from the inputs)\n"
        "  --max     optional size cap in pixels, e.g. 2048 shrinks anything larger than that\n"
        "  --ref     texture table listing which textures originally had mips\n"
        "            (default: origtex.txt in the same folder as this program)\n"
        "  --force   convert again even if the output file already exists\n"
        "\n"
        "Files already converted in the output folder are skipped, so an interrupted\n"
        "run can simply be started again.\n"
        "\n"
        "Example:\n"
        "  drm_mipgen --out converted --max 2048 \"C:\\Games\\Tomb Raider Legend\\mods\"\n"
        "\n"
        "Easier: \"Fix HD texture pack.bat\" next to this program does all of this for you.\n");
}

// True when this process owns its console alone, i.e. it was started by a
// double-click and the window would vanish the moment we return.
bool ownsConsole() {
    DWORD pid;
    return GetConsoleProcessList(&pid, 1) == 1;
}

void waitForEnter() {
    if (!ownsConsole()) return;
    printf("\nPress Enter to close this window...");
    fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
}

int helpAndExit() {
    printHelp();
    waitForEnter();
    return 2;
}

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return fs::path();
    return fs::path(buf).parent_path();
}

std::wstring lowerName(const fs::path& p) {
    std::wstring n = p.filename().wstring();
    for (auto& c : n) c = wchar_t(towlower(c));
    return n;
}

bool sameLocation(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Wide entry point so paths with non-ASCII characters survive intact.
    SetConsoleOutputCP(CP_UTF8);

    Options opt;
    fs::path refPath, outDir;
    std::vector<fs::path> inputs;
    bool force = false;
    if (argc < 2) return helpAndExit();
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"-h" || a == L"--help" || a == L"?" || a == L"/?") return helpAndExit();
        else if (a == L"--max" && i + 1 < argc) opt.maxSize = _wtoi(argv[++i]);
        else if (a == L"--ref" && i + 1 < argc) refPath = argv[++i];
        else if (a == L"--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == L"--force") force = true;
        else inputs.push_back(a);
    }
    if (outDir.empty() || inputs.empty()) {
        fprintf(stderr, "Both --out and at least one input file or folder are needed.\n\n");
        return helpAndExit();
    }

    // The reference table is required: it says which textures the unmodded game
    // shipped with mips. Without it no texture would get a mip chain.
    if (refPath.empty()) {
        fs::path exeRef = exeDir() / L"origtex.txt";
        std::error_code ec;
        if (!fs::is_regular_file(exeRef, ec)) {
            fprintf(stderr, "Cannot find the reference table origtex.txt.\n"
                            "Put it in the same folder as drm_mipgen.exe (%s)\n"
                            "or pass its location with --ref <file>.\n", u8(exeRef.parent_path()).c_str());
            waitForEnter();
            return 1;
        }
        refPath = exeRef;
    }
    if (!loadRef(refPath, opt) || opt.ref.empty()) {
        fprintf(stderr, "Cannot read a valid reference table from %s\n", u8(refPath).c_str());
        waitForEnter();
        return 1;
    }

    // The output must not be an input folder (or hold an input file), or the
    // originals would be overwritten.
    std::error_code ec;
    for (const fs::path& in : inputs) {
        fs::path dir = fs::is_directory(in, ec) ? in : in.parent_path();
        if (dir.empty()) dir = fs::path(L".");
        if (sameLocation(dir, outDir)) {
            fprintf(stderr, "The output folder (%s) is the same as the input folder (%s).\n"
                            "That would overwrite the original files. Choose a different --out folder.\n",
                    u8(outDir).c_str(), u8(dir).c_str());
            waitForEnter();
            return 2;
        }
    }

    // Expand folders (recursively, any case of .drm) into a flat list. Files that
    // already sit in the output folder are left out so reruns don't convert results.
    std::vector<fs::path> files;
    std::map<std::wstring, fs::path> seen;
    for (const fs::path& in : inputs) {
        std::vector<fs::path> found;
        if (fs::is_directory(in, ec)) {
            for (fs::recursive_directory_iterator it(in, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::wstring ext = it->path().extension().wstring();
                for (auto& c : ext) c = wchar_t(towlower(c));
                if (ext != L".drm") continue;
                if (sameLocation(it->path().parent_path(), outDir)) continue;
                found.push_back(it->path());
            }
            ec.clear();
            std::sort(found.begin(), found.end());
            if (found.empty()) fprintf(stderr, "warning: no .drm files found in %s\n", u8(in).c_str());
        } else if (fs::is_regular_file(in, ec)) {
            found.push_back(in);
        } else {
            fprintf(stderr, "warning: %s does not exist, ignored\n", u8(in).c_str());
        }
        for (const fs::path& f : found) {
            auto ins = seen.emplace(lowerName(f), f);
            if (!ins.second) {
                fprintf(stderr, "warning: %s has the same file name as %s, only the first is processed\n",
                        u8(f).c_str(), u8(ins.first->second).c_str());
                continue;
            }
            files.push_back(f);
        }
    }
    if (files.empty()) {
        fprintf(stderr, "Nothing to do.\n");
        waitForEnter();
        return 1;
    }

    fs::create_directories(outDir, ec);
    if (ec || !fs::is_directory(outDir, ec)) {
        fprintf(stderr, "Cannot create the output folder %s\n", u8(outDir).c_str());
        waitForEnter();
        return 1;
    }

    Stats st;
    int converted = 0, skipped = 0, failed = 0;
    const size_t total = files.size();
    for (size_t n = 0; n < total; n++) {
        const fs::path& in = files[n];
        fs::path name = in.filename();
        fs::path out = outDir / name;
        fs::path part = outDir / (name.wstring() + L".part");
        printf("[%zu/%zu] %s", n + 1, total, u8(name).c_str());

        // A leftover .part is from an interrupted run and is never trusted.
        fs::remove(part, ec);

        // Only a non-empty final file counts as done; it only appears after a
        // complete write (see the rename below).
        if (!force && fs::is_regular_file(out, ec) && fs::file_size(out, ec) > 0) {
            printf(": skipped (already done)\n");
            fflush(stdout);
            skipped++;
            continue;
        }
        printf("\n");
        fflush(stdout);

        bool ok = processDrm(in, part, opt, st);
        if (ok) {
            // Rename only after a successful write so an interrupted run never
            // leaves a half file under the real name.
            fs::remove(out, ec);
            fs::rename(part, out, ec);
            if (ec) {
                fprintf(stderr, "  cannot rename %s to %s\n", u8(part).c_str(), u8(out).c_str());
                ok = false;
            }
        }
        if (ok) converted++;
        else {
            failed++;
            fs::remove(part, ec);
            fprintf(stderr, "  FAILED: %s\n", u8(name).c_str());
        }
        fflush(stdout);
    }
    printf("files: %d converted, %d skipped, %d failed\n", converted, skipped, failed);
    printf("textures: %d mip chains built, %d capped, %d existing chains trimmed, %d left as-is\n",
           st.rebuilt.load(), st.capped.load(), st.sliced.load(), st.untouched.load());
    printf("texture data: %.1f MB -> %.1f MB\n", st.bytesIn / 1048576.0, st.bytesOut / 1048576.0);
    waitForEnter();
    return failed ? 1 : 0;
}
