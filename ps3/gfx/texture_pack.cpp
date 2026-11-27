// Texture packs: see texture_pack.h.
#include "texture_pack.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "miniz.h"

namespace n64gfx {
namespace {

constexpr char kMagic[8] = {'C', 'N', 'K', 'R', 'P', 'A', 'C', 'K'};
constexpr uint32_t kVersion = 2;
constexpr int32_t kHtcVersion = 0x08000000;
constexpr uint32_t kGlRgba8 = 0x8058, kGlCompressed = 0x80000000u, kGlUnsignedByte = 0x1401;

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[1] << 8 | p[0]); }

// Reads a gzip file's (first member's) data, with miniz's raw inflate.
class GzipReader {
public:
    explicit GzipReader(const char* path) : in_(1 << 16) {
        std::memset(&z_, 0, sizeof z_);
        f_ = std::fopen(path, "rb");
        if (!f_) return;
        std::fseek(f_, 0, SEEK_END);
        size_ = (uint64_t)std::ftell(f_);
        std::fseek(f_, 0, SEEK_SET);
        uint8_t h[10];
        if (std::fread(h, 1, 10, f_) != 10 || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) return;
        const uint8_t flags = h[3];
        if (flags & 0x04) { uint8_t n[2]; if (std::fread(n, 1, 2, f_) != 2) return; std::fseek(f_, n[0] | n[1] << 8, SEEK_CUR); }
        for (uint8_t bit : {(uint8_t)0x08, (uint8_t)0x10})  // name, comment
            if (flags & bit) { int c; while ((c = std::fgetc(f_)) != EOF && c != 0) {} }
        if (flags & 0x02) std::fseek(f_, 2, SEEK_CUR);
        ok_ = mz_inflateInit2(&z_, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
        inited_ = ok_;
    }
    ~GzipReader() { if (inited_) mz_inflateEnd(&z_); if (f_) std::fclose(f_); }
    // False at the end of the data or on an error (failed() tells which).
    bool read(void* dst, size_t n) {
        if (!ok_) return false;
        z_.next_out = (unsigned char*)dst;
        z_.avail_out = (unsigned int)n;
        while (z_.avail_out) {
            if (ended_) { if (z_.avail_out != n) ok_ = false; return false; }
            if (!z_.avail_in) {
                z_.next_in = in_.data();
                z_.avail_in = (unsigned int)std::fread(in_.data(), 1, in_.size(), f_);
                consumed_ += z_.avail_in;
                if (!z_.avail_in) { ok_ = false; return false; }
            }
            const int st = mz_inflate(&z_, MZ_NO_FLUSH);
            if (st == MZ_STREAM_END) ended_ = true;
            else if (st != MZ_OK) { ok_ = false; return false; }
        }
        return true;
    }
    bool failed() const { return !ok_; }
    float fraction() const { return size_ ? (float)((double)consumed_ / (double)size_) : 0.0f; }

private:
    std::FILE* f_ = nullptr;
    mz_stream z_;
    std::vector<uint8_t> in_;
    uint64_t size_ = 0, consumed_ = 0;
    bool ok_ = false, inited_ = false, ended_ = false;
};

}  // namespace

namespace {
// The RSX's swizzled layout: x and y bits interleaved (x first) as far as the smaller side goes, then
// the larger side's remaining bits.
inline uint32_t swizzle(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t off = 0, bit = 0;
    const uint32_t m = lw < lh ? lw : lh;
    for (uint32_t i = 0; i < m; i++) { off |= ((x >> i) & 1) << bit++; off |= ((y >> i) & 1) << bit++; }
    for (uint32_t i = m; i < lw; i++) off |= ((x >> i) & 1) << bit++;
    for (uint32_t i = m; i < lh; i++) off |= ((y >> i) & 1) << bit++;
    return off;
}
inline uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
inline bool pot(uint32_t v) { return v && !(v & (v - 1)); }
}  // namespace

// The background thread and what it shares with the graphics thread.
struct TexturePack::Worker {
    struct Request { uint64_t hash; Job job; bool mipmaps, compress; };
    std::mutex m;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::unordered_map<uint64_t, Ready> ready;
    size_t waiting_bytes = 0;
    uint64_t working_on = 0;
    bool cancel_current = false, quit = false;
    std::thread thread;
    std::FILE* file = nullptr;
    uint32_t* prepared = nullptr;

    void run() {
        if (TexturePack::thread_setup) TexturePack::thread_setup();
        std::vector<uint8_t> rgba, scratch;
        for (;;) {
            Request r;
            {
                std::unique_lock<std::mutex> lock(m);
                wake.wait(lock, [&] { return quit || (!queue.empty() && waiting_bytes < kMaxWaiting); });
                if (quit) return;
                r = queue.front();
                queue.pop_front();
                working_on = r.hash;
                cancel_current = false;
            }
            Ready out;
            const bool ok = TexturePack::read(file, r.job, rgba, scratch);
            if (ok && r.job.alpha != kAlphaKeep) {
                const size_t n = (size_t)r.job.w * r.job.h * 4;
                // Solid in the game, but more than 5% clear in the picture: cut out on purpose by the pack's
                // maker, so kept as it is.
                size_t clear = 0;
                if (r.job.alpha == kAlphaSolid) for (size_t i = 3; i < n; i += 4) clear += rgba[i] < 128;
                if (r.job.alpha == kAlphaSolid && clear * 20 > n / 4) {}
                else if (r.job.alpha == kAlphaSolid) for (size_t i = 3; i < n; i += 4) rgba[i] = 255;
                else for (size_t i = 3; i < n; i += 4) rgba[i] = rgba[i] >= 128 ? 255 : 0;
            }
            if (ok) TexturePack::prepare(rgba, r.job.w, r.job.h, r.mipmaps, r.compress, &out);
            std::lock_guard<std::mutex> lock(m);
            working_on = 0;
            if (cancel_current) continue;
            // A failed read leaves an empty picture: take() gives it, and the game's own texture stays.
            waiting_bytes += out.data.size();
            ready[r.hash] = std::move(out);
            if (prepared) ++*prepared;
        }
    }
};

TexturePack::TexturePack() {}

TexturePack::~TexturePack() {
    if (!worker_) return;
    { std::lock_guard<std::mutex> lock(worker_->m); worker_->quit = true; }
    worker_->wake.notify_all();
    if (worker_->thread.joinable()) worker_->thread.join();
    if (worker_->file) std::fclose(worker_->file);
    delete worker_;
}

void TexturePack::request(uint64_t hash, const Job& job, bool mipmaps, bool compress) {
    if (!worker_) {
        worker_ = new Worker();
        worker_->file = std::fopen(path_.c_str(), "rb");
        worker_->prepared = &prepared;
        worker_->thread = std::thread([w = worker_] { w->run(); });
    }
    { std::lock_guard<std::mutex> lock(worker_->m); worker_->queue.push_back({hash, job, mipmaps, compress}); }
    worker_->wake.notify_one();
}

bool TexturePack::take(uint64_t hash, Ready* out) {
    if (!worker_) return false;
    std::lock_guard<std::mutex> lock(worker_->m);
    auto it = worker_->ready.find(hash);
    if (it == worker_->ready.end()) return false;
    *out = std::move(it->second);
    worker_->waiting_bytes -= out->data.size();
    worker_->ready.erase(it);
    worker_->wake.notify_one();
    return true;
}

bool TexturePack::take_any(uint64_t* hash, Ready* out) {
    if (!worker_) return false;
    std::lock_guard<std::mutex> lock(worker_->m);
    auto it = worker_->ready.begin();
    if (it == worker_->ready.end()) return false;
    *hash = it->first;
    *out = std::move(it->second);
    worker_->waiting_bytes -= out->data.size();
    worker_->ready.erase(it);
    worker_->wake.notify_one();
    return true;
}

void TexturePack::forget(uint64_t hash) {
    if (!worker_) return;
    std::lock_guard<std::mutex> lock(worker_->m);
    for (auto it = worker_->queue.begin(); it != worker_->queue.end();) it = it->hash == hash ? worker_->queue.erase(it) : it + 1;
    if (worker_->working_on == hash) worker_->cancel_current = true;
    auto it = worker_->ready.find(hash);
    if (it != worker_->ready.end()) { worker_->waiting_bytes -= it->second.data.size(); worker_->ready.erase(it); worker_->wake.notify_one(); }
}

namespace {
// One 4x4 block of RGBA pixels (rows of 4, `stride` bytes apart; a level smaller than 4 repeats its edge)
// as DXT: the colours' bounding box, drawn in a little, gives the two end colours (the fast method of
// "Real-Time DXT Compression", J.M.P. van Waveren, 2006); see-through pixels don't count for it.
inline uint16_t to565(const int c[3]) { return (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3)); }
// DXT1's three-colour mode with see-through (cutout: alpha only 0 or 255): c0 <= c1, index 3 see-through.
void dxt1_cutout_block(const uint8_t* px[16], uint8_t* out) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, seen = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < 128) continue;
        seen++;
        for (int c = 0; c < 3; c++) { lo[c] = std::min<int>(lo[c], px[i][c]); hi[c] = std::max<int>(hi[c], px[i][c]); }
    }
    uint16_t c0 = 0, c1 = 0;
    int pal[3][3] = {};
    if (seen) {
        c0 = to565(lo); c1 = to565(hi);
        if (c0 > c1) std::swap(c0, c1);
        const uint16_t cc[2] = {c0, c1};
        for (int k = 0; k < 2; k++) {
            const int r = cc[k] >> 11, g = (cc[k] >> 5) & 63, b = cc[k] & 31;
            pal[k][0] = r << 3 | r >> 2; pal[k][1] = g << 2 | g >> 4; pal[k][2] = b << 3 | b >> 2;
        }
        for (int c = 0; c < 3; c++) pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 3;
        if (px[i][3] >= 128) {
            int bestd = 1 << 30;
            for (int k = 0; k < 3; k++) {
                const int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
                const int d = dr * dr + dg * dg + db * db;
                if (d < bestd) { bestd = d; best = k; }
            }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)idx; out[5] = (uint8_t)(idx >> 8); out[6] = (uint8_t)(idx >> 16); out[7] = (uint8_t)(idx >> 24);
}

void dxt_block(const uint8_t* px[16], bool dxt5, uint8_t* out) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, alo = 255, ahi = 0, seen = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t* p = px[i];
        alo = std::min<int>(alo, p[3]); ahi = std::max<int>(ahi, p[3]);
        if (dxt5 && p[3] == 0) continue;
        seen++;
        for (int c = 0; c < 3; c++) { lo[c] = std::min<int>(lo[c], p[c]); hi[c] = std::max<int>(hi[c], p[c]); }
    }
    if (!seen) for (int c = 0; c < 3; c++) lo[c] = hi[c] = 0;
    for (int c = 0; c < 3; c++) { const int inset = (hi[c] - lo[c]) >> 4; lo[c] += inset; hi[c] -= inset; }
    if (dxt5) {
        // Alpha: 8 values from ahi down to alo; index 0 is ahi, 1 is alo, 2..7 the six between.
        out[0] = (uint8_t)ahi; out[1] = (uint8_t)alo;
        uint64_t bits = 0;
        const int range = ahi - alo;
        for (int i = 0; i < 16; i++) {
            int idx = 0;
            if (range > 0) {
                const int t = ((ahi - px[i][3]) * 7 + range / 2) / range;  // 0 (ahi) .. 7 (alo)
                idx = t == 0 ? 0 : t == 7 ? 1 : t + 1;
            }
            bits |= (uint64_t)idx << (3 * i);
        }
        for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
        out += 8;
    }
    uint16_t c0 = to565(hi), c1 = to565(lo);
    uint32_t idx = 0;
    if (c0 != c1) {
        if (c0 < c1) std::swap(c0, c1);  // c0 > c1: the four-colour mode
        int pal[4][3];
        const uint16_t cc[2] = {c0, c1};
        for (int k = 0; k < 2; k++) {
            const int r = cc[k] >> 11, g = (cc[k] >> 5) & 63, b = cc[k] & 31;
            pal[k][0] = r << 3 | r >> 2; pal[k][1] = g << 2 | g >> 4; pal[k][2] = b << 3 | b >> 2;
        }
        for (int c = 0; c < 3; c++) { pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3; pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3; }
        for (int i = 0; i < 16; i++) {
            int best = 0, bestd = 1 << 30;
            for (int k = 0; k < 4; k++) {
                const int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
                const int d = dr * dr + dg * dg + db * db;
                if (d < bestd) { bestd = d; best = k; }
            }
            idx |= (uint32_t)best << (2 * i);
        }
    }
    // (c0 == c1 with DXT1 means "three colours and see-through", but index 0 is that colour either way.)
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)idx; out[5] = (uint8_t)(idx >> 8); out[6] = (uint8_t)(idx >> 16); out[7] = (uint8_t)(idx >> 24);
}

// A level's blocks, appended.
void dxt_level(const uint8_t* rgba, uint32_t w, uint32_t h, bool dxt5, bool cutout, std::vector<uint8_t>& out) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = dxt5 ? 16 : 8;
    size_t at = out.size();
    out.resize(at + (size_t)bw * bh * bs);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, at += bs) {
            const uint8_t* px[16];
            for (uint32_t j = 0; j < 4; j++)
                for (uint32_t i = 0; i < 4; i++)
                    px[j * 4 + i] = rgba + ((size_t)std::min(by * 4 + j, h - 1) * w + std::min(bx * 4 + i, w - 1)) * 4;
            if (cutout) dxt1_cutout_block(px, &out[at]);
            else dxt_block(px, dxt5, &out[at]);
        }
}

// The next mipmap level: each pixel the average of the 2x2 (or 2x1) above, colours weighted by alpha.
void halve(const std::vector<uint8_t>& cur, uint32_t lw, uint32_t lh, std::vector<uint8_t>& next) {
    const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2), sx = lw > 1 ? 2 : 1, sy = lh > 1 ? 2 : 1;
    next.resize((size_t)nw * nh * 4);
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t acc[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, a = 0;
            for (uint32_t j = 0; j < sy; j++)
                for (uint32_t i = 0; i < sx; i++) {
                    const uint8_t* q = &cur[((size_t)(y * sy + j) * lw + x * sx + i) * 4];
                    for (int c = 0; c < 3; c++) { acc[c] += q[c] * q[3]; plain[c] += q[c]; }
                    a += q[3];
                }
            uint8_t* o = &next[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(a ? acc[c] / a : plain[c] / (sx * sy));
            o[3] = (uint8_t)(a / (sx * sy));
        }
}
}  // namespace

void TexturePack::prepare(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, bool mipmaps, bool compress, Ready* out) {
    out->w = w; out->h = h; out->levels = 1; out->swizzled = false; out->dxt = 0;
    if (compress && pot(w) && pot(h) && w >= 4 && h >= 4) {
        bool opaque = true, binary = true;
        for (size_t i = 3, n = (size_t)w * h * 4; i < n; i += 4) {
            if (rgba[i] != 255) opaque = false;
            if (rgba[i] != 0 && rgba[i] != 255) { binary = false; break; }
        }
        // Only 0 and 255 (a cutout): DXT1 with its one-bit alpha, each smaller level cut at half again.
        const bool cutout = !opaque && binary;
        if (cutout) opaque = true;  // DXT1's size from here on
        out->dxt = opaque ? 1 : 5;
        out->data.clear();
        out->data.reserve((size_t)w * h * (opaque ? 1 : 2) * 2 / 3);
        std::vector<uint8_t> cur(rgba.begin(), rgba.begin() + (size_t)w * h * 4), next;
        uint32_t levels = 0;
        for (uint32_t lw = w, lh = h;;) {
            if (cutout && levels) for (size_t i = 3, n = (size_t)lw * lh * 4; i < n; i += 4) cur[i] = cur[i] >= 128 ? 255 : 0;
            dxt_level(cur.data(), lw, lh, !opaque, cutout, out->data);
            levels++;
            if (!mipmaps || (lw == 1 && lh == 1)) break;
            halve(cur, lw, lh, next);
            cur.swap(next);
            lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
        }
        out->levels = levels;
        return;
    }
    if (!mipmaps || !pot(w) || !pot(h) || (w == 1 && h == 1)) {
        out->data.resize((size_t)w * h * 4);
        const uint8_t* s = rgba.data();
        uint8_t* d = out->data.data();
        for (size_t i = 0, n = (size_t)w * h; i < n; i++, s += 4, d += 4) { d[0] = s[3]; d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; }
        return;
    }
    size_t bytes = 0;
    uint32_t levels = 0;
    for (uint32_t lw = w, lh = h;; lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) {
        bytes += (size_t)lw * lh * 4;
        levels++;
        if (lw == 1 && lh == 1) break;
    }
    out->data.resize(bytes);
    out->levels = levels;
    out->swizzled = true;
    std::vector<uint8_t> cur(rgba.begin(), rgba.begin() + (size_t)w * h * 4), next;
    uint8_t* d = out->data.data();
    for (uint32_t lw = w, lh = h;;) {
        const uint32_t bw = log2u(lw), bh = log2u(lh);
        // Swizzled offsets split into an x part and a y part, so each is worked out once per column / row.
        std::vector<uint32_t> xoff(lw);
        for (uint32_t x = 0; x < lw; x++) xoff[x] = swizzle(x, 0, bw, bh) * 4;
        for (uint32_t y = 0; y < lh; y++) {
            const uint32_t yoff = swizzle(0, y, bw, bh) * 4;
            const uint8_t* s = &cur[(size_t)y * lw * 4];
            for (uint32_t x = 0; x < lw; x++, s += 4) {
                uint8_t* p = d + (yoff | xoff[x]);
                p[0] = s[3]; p[1] = s[0]; p[2] = s[1]; p[3] = s[2];
            }
        }
        d += (size_t)lw * lh * 4;
        if (lw == 1 && lh == 1) break;
        halve(cur, lw, lh, next);
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        cur.swap(next);
        lw = nw; lh = nh;
    }
}

bool TexturePack::convert(const char* htc_path, const char* pack_path, const std::function<void(float)>& progress, std::string* error) {
    GzipReader gz(htc_path);
    uint8_t word[4];
    if (!gz.read(word, 4)) { *error = "it isn't a GLideN64 texture cache (.htc)"; return false; }
    const bool fresh = (int32_t)le32(word) == kHtcVersion;  // new caches: a version, then the config; old ones the config alone
    if (fresh && !gz.read(word, 4)) { *error = "the texture cache is cut short"; return false; }
    std::FILE* out = std::fopen(pack_path, "wb");
    if (!out) { *error = "the pack file can't be written"; return false; }
    uint8_t header[24] = {};
    bool ok = std::fwrite(header, 1, sizeof header, out) == sizeof header;
    std::vector<Entry> index;
    std::vector<uint8_t> data, plain;
    uint64_t offset = sizeof header;
    const size_t head_bytes = 8 + 4 * 3 + 2 + 2 + 1 + (fresh ? 2 : 0) + 4;
    uint32_t n = 0;
    while (ok) {
        uint8_t h[40];
        if (!gz.read(h, head_bytes)) break;
        const uint64_t key = (uint64_t)le32(h + 4) << 32 | le32(h);
        const uint32_t w = le32(h + 8), ht = le32(h + 12);
        uint32_t format = le32(h + 16);
        const uint16_t pixel_type = le16(h + 22);
        const uint32_t size = le32(h + head_bytes - 4);
        if (size > (64u << 20) || w > 8192 || ht > 8192) { *error = "the texture cache isn't in a format this reads"; ok = false; break; }
        data.resize(size);
        if (size && !gz.read(data.data(), size)) { *error = "the texture cache is cut short"; ok = false; break; }
        const uint8_t* pixels = data.data();
        size_t pixel_bytes = size;
        if (format & kGlCompressed) {
            format &= ~kGlCompressed;
            plain.resize((size_t)w * ht * 4);
            mz_ulong len = (mz_ulong)plain.size();
            if (mz_uncompress(plain.data(), &len, data.data(), size) != MZ_OK) continue;
            pixels = plain.data();
            pixel_bytes = len;
        }
        if (format != kGlRgba8 || pixel_type != kGlUnsignedByte || pixel_bytes != (size_t)w * ht * 4 || !pixel_bytes) continue;
        ok = std::fwrite(pixels, 1, pixel_bytes, out) == pixel_bytes;
        index.push_back({key, offset, w, ht});
        offset += pixel_bytes;
        if ((++n & 31) == 0 && progress) progress(gz.fraction());
    }
    if (ok && gz.failed()) { *error = "the texture cache is damaged"; ok = false; }
    if (ok && index.empty()) { *error = "the texture cache has no textures this can use (RGBA8)"; ok = false; }
    if (ok) {
        std::sort(index.begin(), index.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
        std::vector<uint8_t> raw(index.size() * 24);
        for (size_t i = 0; i < index.size(); i++) {
            uint8_t* p = &raw[i * 24];
            put64(p, index[i].key); put64(p + 8, index[i].offset); put32(p + 16, index[i].width); put32(p + 20, index[i].height);
        }
        ok = std::fwrite(raw.data(), 1, raw.size(), out) == raw.size();
        std::memcpy(header, kMagic, 8);
        put32(header + 8, kVersion); put32(header + 12, (uint32_t)index.size()); put64(header + 16, offset);
        ok = ok && std::fseek(out, 0, SEEK_SET) == 0 && std::fwrite(header, 1, sizeof header, out) == sizeof header;
        if (!ok) *error = "the pack file couldn't be written (is the disk full?)";
    } else if (error->empty()) {
        *error = "the pack file couldn't be written (is the disk full?)";
    }
    ok = (std::fclose(out) == 0) && ok;
    if (!ok) std::remove(pack_path);
    else if (progress) progress(1.0f);
    return ok;
}

bool TexturePack::open(const char* pack_path) {
    path_.clear();
    index_.clear();
    std::FILE* f = std::fopen(pack_path, "rb");
    if (!f) return false;
    uint8_t header[24];
    bool ok = std::fread(header, 1, sizeof header, f) == sizeof header && std::memcmp(header, kMagic, 8) == 0 && be32(header + 8) == kVersion;
    const uint32_t count = ok ? be32(header + 12) : 0;
    std::vector<uint8_t> raw((size_t)count * 24);
    ok = ok && count && count < (1u << 20) && std::fseek(f, (long)be64(header + 16), SEEK_SET) == 0 &&
         std::fread(raw.data(), 1, raw.size(), f) == raw.size();
    std::fclose(f);
    if (!ok) return false;
    index_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = &raw[(size_t)i * 24];
        index_[i] = {be64(p), be64(p + 8), be32(p + 16), be32(p + 20)};
    }
    path_ = pack_path;
    return true;
}

const TexturePack::Entry* TexturePack::find_key(uint64_t key) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), key, [](const Entry& e, uint64_t k) { return e.key < k; });
    return it != index_.end() && it->key == key ? &*it : nullptr;
}

bool TexturePack::find(uint32_t tex_w, uint32_t tex_h, uint32_t hashed_w, uint32_t hashed_h, uint32_t crc, uint32_t palette_crc,
                       bool has_palette, Job* job) {
    if (path_.empty() || !tex_w || !tex_h || !hashed_w || !hashed_h) return false;
    looked_up++;
    const Entry* e = has_palette ? find_key((uint64_t)palette_crc << 32 | crc) : nullptr;
    if (!e) e = find_key(crc);
    if (!e) return false;
    // Worth it only if larger; drawn at 2 or 4 times the texture's size (most packs' pictures are
    // somewhere between: 2.8 times is common), whichever holds the picture's detail.
    if (e->width <= hashed_w && e->height <= hashed_h) return false;
    const uint32_t scale = (e->width > hashed_w * 2 || e->height > hashed_h * 2) ? 4 : 2;
    if (tex_w * scale > 2048 || tex_h * scale > 2048) return false;
    *job = Job{e->offset, e->width, e->height, tex_w * scale, tex_h * scale, hashed_w * scale, hashed_h * scale};
    attached++;
    return true;
}

bool TexturePack::read(std::FILE* f, const Job& job, std::vector<uint8_t>& rgba, std::vector<uint8_t>& scratch) {
    const uint32_t pw = job.pack_w, ph = job.pack_h, w = job.w, h = job.h;
    scratch.resize((size_t)pw * ph * 4);
    if (!f || std::fseek(f, (long)job.offset, SEEK_SET) != 0 || std::fread(scratch.data(), 1, scratch.size(), f) != scratch.size()) return false;
    rgba.resize((size_t)w * h * 4);
    // Bilinear, pixel centres aligned (the edge pixels are held past the picture's edge), in 16.16 fixed
    // point; each column's two source pixels and weight are worked out once.
    std::vector<uint32_t> cx0(w), cx1(w), cwx(w);
    const uint32_t cw = job.cover_w ? job.cover_w : w, ch = job.cover_h ? job.cover_h : h;  // the part the picture covers
    const uint32_t stepx = (uint32_t)(((uint64_t)pw << 16) / cw), stepy = (uint32_t)(((uint64_t)ph << 16) / ch);
    for (uint32_t x = 0; x < w; x++) {
        int64_t sx = (int64_t)std::min(x, cw - 1) * stepx + stepx / 2 - 0x8000;
        if (sx < 0) sx = 0;
        uint32_t x0 = (uint32_t)(sx >> 16);
        if (x0 > pw - 1) x0 = pw - 1;
        cx0[x] = x0 * 4; cx1[x] = std::min(x0 + 1, pw - 1) * 4; cwx[x] = (uint32_t)(sx & 0xFFFF) >> 8;
    }
    for (uint32_t y = 0; y < h; y++) {
        int64_t sy = (int64_t)std::min(y, ch - 1) * stepy + stepy / 2 - 0x8000;
        if (sy < 0) sy = 0;
        uint32_t y0 = (uint32_t)(sy >> 16);
        if (y0 > ph - 1) y0 = ph - 1;
        const uint32_t y1 = std::min(y0 + 1, ph - 1), wy = (uint32_t)(sy & 0xFFFF) >> 8;
        const uint8_t* r0 = &scratch[(size_t)y0 * pw * 4];
        const uint8_t* r1 = &scratch[(size_t)y1 * pw * 4];
        uint8_t* d = &rgba[(size_t)y * w * 4];
        for (uint32_t x = 0; x < w; x++, d += 4) {
            const uint8_t *a = r0 + cx0[x], *b = r0 + cx1[x], *c = r1 + cx0[x], *e = r1 + cx1[x];
            const uint32_t wx = cwx[x];
            for (int k = 0; k < 4; k++) {
                const uint32_t top = a[k] * (256 - wx) + b[k] * wx, bot = c[k] * (256 - wx) + e[k] * wx;
                d[k] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
    return true;
}

}  // namespace n64gfx
