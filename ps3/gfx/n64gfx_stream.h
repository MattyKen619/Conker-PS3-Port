// The draw stream: the display list interpreter can run on an SPU, where it records what it would
// draw (Recorder, a Backend and TextureSource); the PPU plays the stream into the RSX backend
// (StreamPlayer). The stream is a sequence of big-endian 32-bit words:
//
//   record header: op << 24 | length in words (header included)
//   STATE    DrawState without its texture pointers (sent when it changed)
//   TEXDATA  key (2 words), TLUT type, tile (16 words), TMEM (1024 words): a texture the PPU may not
//            have decoded recently; it decodes it under the key
//   TRIS     2 texture references (kTexRefWords each), 0-3 zero words so the vertices start on a
//            16-byte boundary of the stream (tris_pad), then vertices (8 words each, OutVertex)
//   FILL     rectangle (4 floats)
//   TEXRECT  2 texture references, rectangle, s, t, dsdx, dtdy (8 floats), flip
//   CLEARZ   depth image, rectangle
//   BEGINDL / ENDDL
//   END      the end of the stream; OVERFLOW instead: the recorder ran out of room (the PPU then
//            interprets the display list itself)
// A texture reference: kind (0 none, 1 decoded texture by key, 2 piece of a drawn frame), key,
// frame address, x, y, width, height, format, size.
#pragma once
#include <cstdint>

#include "n64gfx.h"

namespace n64gfx {

enum StreamOp : uint32_t {
    kOpState = 1, kOpTexData = 2, kOpTris = 3, kOpFill = 4, kOpTexRect = 5, kOpClearZ = 6,
    kOpBeginDl = 7, kOpEndDl = 8, kOpEnd = 9, kOpOverflow = 10,
    kOpTrisMid = 11,  // TRIS, then each vertex's in-between frame position (4 words each)
    kOpMip = 12,      // before a draw: TEXEL0's smaller versions (count, then each texture's key)
};
constexpr uint32_t kStateWords = 55;
constexpr uint32_t kTexRefWords = 10;
constexpr uint32_t kTileWords = 16;
constexpr uint32_t kVertexWords = 8;
// Padding words in a TRIS record that starts at word `at` of the stream.
inline uint32_t tris_pad(uint32_t at) { return (4 - ((at + 1 + 2 * kTexRefWords) & 3)) & 3; }

// Where the recorder's words go: a buffer, emptied by flush() when full and at the end.
class StreamOut {
public:
    virtual ~StreamOut() = default;
    // Takes `words` words from buf (the recorder's buffer); false if there's no room (overflow). Every
    // flush but the last of a stream is a whole buffer. With two buffers, the recorder fills the other
    // one next, so flush may return before buf has been sent (and must wait for it on the next call).
    virtual bool flush(const uint32_t* buf, uint32_t words) = 0;
};

class Recorder : public Backend, public TextureSource {
public:
    // buf: the recorder's own buffer (big-endian words), flushed to `out` when full; buf2: optionally a
    // second one of the same size, used alternately.
    Recorder(uint32_t* buf, uint32_t buf_words, StreamOut* out, uint32_t* buf2 = nullptr)
        : buf_(buf), cap_(buf_words), out_(out), bufs_{buf, buf2 ? buf2 : buf} {}

    // Backend
    void draw_triangles(const DrawState& st, const OutVertex* v, size_t count) override;
    void draw_triangles_mid(const DrawState& st, const OutVertex* v, const float (*mid)[4], size_t count) override;
    void fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) override;
    void tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry,
                  float s, float t, float dsdx, float dtdy, bool flip) override;
    void clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) override;
    void begin_dl() override;
    void end_dl() override;
    // TextureSource
    const Texture* get(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, bool copy_mode) override;
    void touch(const Texture* t) override;

    // Starts a new stream (the one before was finished).
    void start() { pos_ = 0; overflow_ = false; words = 0; textures_sent = 0; }
    // Ends the stream (END, or OVERFLOW if anything didn't fit) and flushes it.
    void finish();
    // The player's texture cache was emptied (or never got a stream): send every texture again.
    void forget_sent() { for (auto& e : sent_) e = Sent{}; }
    bool overflowed() const { return overflow_; }
    uint32_t textures_sent = 0, words = 0;

private:
    static uint32_t to_be(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return v;
#else
        return __builtin_bswap32(v);
#endif
    }
    void put(uint32_t w) {
        if (__builtin_expect(pos_ == cap_, 0)) flush_buf();
        buf_[pos_++] = to_be(w);
    }
    void putf(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); put(u); }
    // Words already in stream order (big-endian) in memory.
    void put_raw(const void* p, uint32_t n);
    static void state_words(const DrawState& st, uint32_t* w);
    void put_texref(const Texture* t);
    void put_mip(const DrawState& st);
    void maybe_state(const DrawState& st);
    void flush_buf();

    uint32_t* buf_;
    uint32_t cap_, pos_ = 0;
    StreamOut* out_;
    uint32_t* bufs_[2];
    uint32_t cur_ = 0;
    bool overflow_ = false;
    // The last state sent (as words), to send STATE only on changes.
    uint32_t last_state_[kStateWords];
    bool have_state_ = false;
    // Textures handed to the interpreter this display list (it keeps pointers until the next), one per
    // key: an open-addressing table (more distinct textures than it holds overflows the stream).
    static constexpr uint32_t kPool = 512;
    Texture pool_[kPool];
    uint32_t pool_n_ = 0;
    uint32_t pool_dl_[kPool] = {};  // the display list number the slot was filled in (else it's free)
    // Keys whose TMEM was sent, with the display list number they were last used in: the PPU's cache
    // keeps a texture 120 display lists after its last use, so TMEM is sent again after 60.
    // An entry is the key's top 32 bits (the bottom bits pick its home slot) and the display list it was
    // last used in (0: unused); 16 KB.
    static constexpr uint32_t kSent = 2048, kProbe = 16;
    struct Sent { uint32_t tag = 0, last = 0; } sent_[kSent];
    uint32_t dl_ = 1;
    Sent* sent_slot(uint64_t key, bool* found);
};

#if !defined(__SPU__)
class StreamPlayer {
public:
    explicit StreamPlayer(TextureCache& cache) : cache_(cache) {}
    // Plays a stream into a backend. Returns false (with a reason) if it was malformed or the recorder
    // overflowed (nothing is drawn then).
    bool play(const uint32_t* stream, uint32_t words, Backend& be, const char** error);
    // Frame interpolation: draw the in-between frame (TRISMID records' second positions) instead.
    bool mid_pass = false;
    // Playing a stream while it's being written: begin(), then play_more() with the words that have
    // arrived so far (the same stream pointer each time) until it returns kDone (END played) or
    // kFailed (malformed, or OVERFLOW: what was recorded before it has been drawn).
    enum Result { kMore, kDone, kFailed };
    void begin() { pos_ = 0; mip_armed_ = false; st_.mip_levels = 0; }
    Result play_more(const uint32_t* stream, uint32_t words, Backend& be, const char** error);
    uint32_t position() const { return pos_; }
    uint32_t missing_textures = 0;
    bool mip_armed_ = false;

private:
    const Texture* texref(const uint32_t* p, int unit);
    TextureCache& cache_;
    uint32_t pos_ = 0;
    DrawState st_;
    OutVertex mid_verts_[3 * 512];
    Texture fb_tex_[2];
};
#endif

}  // namespace n64gfx
