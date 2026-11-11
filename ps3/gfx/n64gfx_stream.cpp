// Draw stream recorder and player (see n64gfx_stream.h).
#include "n64gfx_stream.h"

#include <cstring>

namespace n64gfx {

namespace {
inline uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float bitsf(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
// Stream words are big-endian in memory.
inline uint32_t to_be(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return v;
#else
    return __builtin_bswap32(v);
#endif
}
inline uint32_t from_be(uint32_t v) { return to_be(v); }
}

// ------------------------------------------------------------------------------------ recorder
void Recorder::flush_buf() {
    if (!pos_) return;
    if (!overflow_ && !out_->flush(buf_, pos_)) overflow_ = true;
    words += pos_;
    pos_ = 0;
    cur_ ^= 1;
    buf_ = bufs_[cur_];
}
void Recorder::put_raw(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        if (pos_ == cap_) flush_buf();
        const uint32_t k = n < cap_ - pos_ ? n : cap_ - pos_;
        std::memcpy(buf_ + pos_, b, k * 4);
        pos_ += k; b += k * 4; n -= k;
    }
}

void Recorder::state_words(const DrawState& st, uint32_t* w) {
    uint32_t i = 0;
    auto put = [&](uint32_t v) { w[i++] = v; };
    auto putf = [&](float f) { w[i++] = fbits(f); };
    put(st.cimg_addr); put(st.cimg_width); put(st.cimg_fmt); put(st.cimg_siz); put(st.zimg_addr);
    put(st.geometry_mode); put(st.om_h); put(st.om_l); put(st.combine_hi); put(st.combine_lo);
    for (int k = 0; k < 4; k++) putf(st.prim[k]);
    for (int k = 0; k < 4; k++) putf(st.env[k]);
    for (int k = 0; k < 4; k++) putf(st.fog[k]);
    for (int k = 0; k < 4; k++) putf(st.blend[k]);
    putf(st.prim_lod_frac); putf(st.k5); put(st.fill_color); putf(st.prim_depth);
    putf(st.sc_ulx); putf(st.sc_uly); putf(st.sc_lrx); putf(st.sc_lry);
    for (int k = 0; k < 3; k++) putf(st.vp_scale[k]);
    for (int k = 0; k < 3; k++) putf(st.vp_trans[k]);
    for (int u = 0; u < 2; u++) {
        const TexMap& m = st.tmap[u];
        putf(m.shift_s); putf(m.shift_t); putf(m.offset_s); putf(m.offset_t);
        put((uint32_t)m.clamp_s | (uint32_t)m.clamp_t << 1 | (uint32_t)m.mirror_s << 2 | (uint32_t)m.mirror_t << 3);
        putf(m.clamp_max_s); putf(m.clamp_max_t);
    }
    put(st.rect_explicit);
}

void Recorder::maybe_state(const DrawState& st) {
    uint32_t w[kStateWords];
    state_words(st, w);
    if (have_state_ && std::memcmp(w, last_state_, sizeof w) == 0) return;
    std::memcpy(last_state_, w, sizeof w);
    have_state_ = true;
    put(kOpState << 24 | (kStateWords + 1));
    for (uint32_t i = 0; i < kStateWords; i++) put(w[i]);
}

void Recorder::put_texref(const Texture* t) {
    if (!t) { for (uint32_t i = 0; i < kTexRefWords; i++) put(0); return; }
    if (!t->fb_addr) {
        // The PPU's cache counts a texture as used when a reference to it is played.
        bool found;
        Sent* s = sent_slot(t->hash, &found);
        if (found) s->last = dl_;
    }
    put(t->fb_addr ? 2 : 1);
    put((uint32_t)(t->hash >> 32)); put((uint32_t)t->hash);
    put(t->fb_addr); put((uint32_t)t->fb_x); put((uint32_t)t->fb_y);
    put(t->width); put(t->height); put(t->fmt); put(t->siz);
}

void Recorder::put_mip(const DrawState& st) {
    if (!st.mip_levels) return;
    put(kOpMip << 24 | (2 + 2 * st.mip_levels));
    put(st.mip_levels);
    for (uint32_t k = 0; k < st.mip_levels; k++) {
        const Texture* t = st.mip[k];
        bool found;
        Sent* s = sent_slot(t->hash, &found);
        if (found) s->last = dl_;
        put((uint32_t)(t->hash >> 32)); put((uint32_t)t->hash);
    }
}

// The key's entry, or the slot to record it in (the caller checks the key): probes a few slots from the
// key's home and gives the first unused one, else the least recently used (if the PPU may have dropped
// that texture it's sent again anyway; else it's just sent again if it comes back). Slots are never
// emptied, so a key that's there is always found within the probe window.
Recorder::Sent* Recorder::sent_slot(uint64_t key, bool* found) {
    uint32_t i = (uint32_t)key & (kSent - 1);
    const uint32_t tag = (uint32_t)(key >> 32);
    Sent* spare = nullptr;
    for (uint32_t n = 0; n < kProbe; n++, i = (i + 1) & (kSent - 1)) {
        Sent& e = sent_[i];
        if (e.last && e.tag == tag) { *found = true; return &e; }
        if (!e.last) { *found = false; return &e; }
        if (!spare || e.last < spare->last) spare = &e;  // the least recently used (stale ones first)
    }
    *found = false;
    return spare;
}

const Texture* Recorder::get(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, bool copy_mode) {
    (void)copy_mode;
    const uint64_t key = tile_hash(tmem, tile, tlut_type);
    bool found;
    Sent* s = sent_slot(key, &found);
    if (!found || dl_ - s->last > 60) {
        // TEXDATA: the PPU decodes this one.
        put(kOpTexData << 24 | (4 + kTileWords + 1024));
        put((uint32_t)(key >> 32)); put((uint32_t)key); put(tlut_type);
        const uint32_t t[kTileWords] = {tile.fmt, tile.siz, tile.line, tile.tmem, tile.palette, tile.cms, tile.cmt, tile.masks,
                                        tile.maskt, tile.shifts, tile.shiftt, tile.uls, tile.ult, tile.lrs, tile.lrt, 0};
        for (uint32_t i = 0; i < kTileWords; i++) put(t[i]);
        put_raw(tmem, 1024);  // TMEM bytes in order are big-endian words
        textures_sent++;
        s->tag = (uint32_t)(key >> 32); s->last = dl_;
    }
    uint32_t slot = (uint32_t)(key ^ (key >> 29)) & (kPool - 1);
    for (uint32_t probe = 0;; probe++, slot = (slot + 1) & (kPool - 1)) {
        if (pool_dl_[slot] != dl_) break;                    // free this display list
        if (pool_[slot].hash == key) return &pool_[slot];    // already handed out
        if (probe == kPool / 2) { overflow_ = true; break; } // too many for one display list: reuse
    }
    pool_dl_[slot] = dl_;
    pool_n_++;
    Texture& tex = pool_[slot];
    tex = Texture{};
    tex.hash = key;
    tile_size(tile, &tex.width, &tex.height);
    tex.fmt = (uint8_t)tile.fmt; tex.siz = (uint8_t)tile.siz; tex.palette = (uint8_t)tile.palette; tex.tlut = (uint8_t)tlut_type;
    return &tex;
}

void Recorder::touch(const Texture* t) { (void)t; }  // counted when a reference is recorded

void Recorder::draw_triangles_mid(const DrawState& st, const OutVertex* v, const float (*mid)[4], size_t count) {
    maybe_state(st);
    put_mip(st);
    const uint32_t pad = tris_pad(words + pos_);
    put(kOpTrisMid << 24 | (uint32_t)(1 + 2 * kTexRefWords + pad + count * (kVertexWords + 4)));
    put_texref(st.tex[0]);
    put_texref(st.tex[1]);
    for (uint32_t i = 0; i < pad; i++) put(0);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    put_raw(v, (uint32_t)count * kVertexWords);
    put_raw(mid, (uint32_t)count * 4);
#else
    for (size_t i = 0; i < count; i++) {
        const OutVertex& o = v[i];
        putf(o.x); putf(o.y); putf(o.z); putf(o.w); putf(o.s); putf(o.t);
        put((uint32_t)o.c[0] << 24 | (uint32_t)o.c[1] << 16 | (uint32_t)o.c[2] << 8 | o.c[3]);
        put(0);
    }
    for (size_t i = 0; i < count; i++) for (int k = 0; k < 4; k++) putf(mid[i][k]);
#endif
}

void Recorder::draw_triangles(const DrawState& st, const OutVertex* v, size_t count) {
    maybe_state(st);
    put_mip(st);
    // The vertices start on a 16-byte boundary of the stream (so the player can hand them on in place).
    const uint32_t pad = tris_pad(words + pos_);
    put(kOpTris << 24 | (uint32_t)(1 + 2 * kTexRefWords + pad + count * kVertexWords));
    put_texref(st.tex[0]);
    put_texref(st.tex[1]);
    for (uint32_t i = 0; i < pad; i++) put(0);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    static_assert(sizeof(OutVertex) == kVertexWords * 4, "OutVertex is the stream's vertex record");
    put_raw(v, (uint32_t)count * kVertexWords);  // a big-endian OutVertex is the record
#else
    for (size_t i = 0; i < count; i++) {
        const OutVertex& o = v[i];
        putf(o.x); putf(o.y); putf(o.z); putf(o.w); putf(o.s); putf(o.t);
        put((uint32_t)o.c[0] << 24 | (uint32_t)o.c[1] << 16 | (uint32_t)o.c[2] << 8 | o.c[3]);
        put(0);
    }
#endif
}

void Recorder::fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) {
    maybe_state(st);
    put(kOpFill << 24 | 5);
    putf(ulx); putf(uly); putf(lrx); putf(lry);
}

void Recorder::tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry, float s, float t, float dsdx, float dtdy, bool flip) {
    maybe_state(st);
    put(kOpTexRect << 24 | (2 + 2 * kTexRefWords + 8));
    put_texref(st.tex[0]);
    put_texref(st.tex[1]);
    putf(ulx); putf(uly); putf(lrx); putf(lry); putf(s); putf(t); putf(dsdx); putf(dtdy);
    put(flip);
}

void Recorder::clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) {
    put(kOpClearZ << 24 | 6);
    put(zimg_addr); putf(ulx); putf(uly); putf(lrx); putf(lry);
}

void Recorder::begin_dl() {
    pool_n_ = 0;
    have_state_ = false;
    put(kOpBeginDl << 24 | 1);
}

void Recorder::end_dl() {
    put(kOpEndDl << 24 | 1);
    dl_++;
}

void Recorder::finish() {
    put((overflow_ ? kOpOverflow : kOpEnd) << 24 | 1);
    flush_buf();
}

// -------------------------------------------------------------------------------------- player
#if !defined(__SPU__)
const Texture* StreamPlayer::texref(const uint32_t* p, int unit) {
    const uint32_t kind = from_be(p[0]);
    if (kind == 0) return nullptr;
    const uint64_t key = (uint64_t)from_be(p[1]) << 32 | from_be(p[2]);
    if (kind == 2) {
        Texture& t = fb_tex_[unit];
        t = Texture{};
        t.fb_addr = from_be(p[3]); t.fb_x = (int32_t)from_be(p[4]); t.fb_y = (int32_t)from_be(p[5]);
        t.width = from_be(p[6]); t.height = from_be(p[7]); t.fmt = (uint8_t)from_be(p[8]); t.siz = (uint8_t)from_be(p[9]);
        return &t;
    }
    const Texture* t = cache_.find(key);
    if (!t) missing_textures++;
    return t;
}

bool StreamPlayer::play(const uint32_t* s, uint32_t words, Backend& be, const char** error) {
    const char* dummy;
    if (!error) error = &dummy;
    // Nothing is drawn unless the stream is complete (it ends with END, not OVERFLOW).
    if (words == 0 || from_be(s[words - 1]) != (kOpEnd << 24 | 1)) {
        *error = words && (from_be(s[words - 1]) >> 24) == kOpOverflow ? "the recorder ran out of room" : "the stream doesn't end with END";
        return false;
    }
    begin();
    return play_more(s, words, be, error) == kDone;
}

StreamPlayer::Result StreamPlayer::play_more(const uint32_t* s, uint32_t words, Backend& be, const char** error) {
    const char* dummy;
    if (!error) error = &dummy;
    uint32_t& pos = pos_;
    while (pos < words) {
        const uint32_t hdr = from_be(s[pos]), op = hdr >> 24, len = hdr & 0xFFFFFF;
        if (len == 0) { *error = "a record of length 0"; return kFailed; }
        if (pos + len > words) return kMore;  // the rest of it hasn't arrived
        const uint32_t* p = s + pos + 1;
        // A MIP record is for the record after it only.
        if (op != kOpMip) {
            if (mip_armed_) mip_armed_ = false;
            else st_.mip_levels = 0;
        }
        switch (op) {
        case kOpMip: {
            const uint32_t n = len >= 2 ? from_be(p[0]) : 0;
            if (n > DrawState::kMaxMips || len != 2 + 2 * n) { *error = "bad MIP"; return kFailed; }
            st_.mip_levels = 0;
            for (uint32_t k = 0; k < n; k++) {
                const Texture* t = cache_.find((uint64_t)from_be(p[1 + 2 * k]) << 32 | from_be(p[2 + 2 * k]));
                if (!t) break;  // only the levels down to a missing one
                st_.mip[st_.mip_levels++] = t;
            }
            mip_armed_ = true;
            break;
        }
        case kOpState: {
            if (len != kStateWords + 1) { *error = "bad STATE"; return kFailed; }
            uint32_t i = 0;
            auto u = [&]() { return from_be(p[i++]); };
            auto f = [&]() { return bitsf(from_be(p[i++])); };
            DrawState& st = st_;
            st.cimg_addr = u(); st.cimg_width = u(); st.cimg_fmt = u(); st.cimg_siz = u(); st.zimg_addr = u();
            st.geometry_mode = u(); st.om_h = u(); st.om_l = u(); st.combine_hi = u(); st.combine_lo = u();
            for (int k = 0; k < 4; k++) st.prim[k] = f();
            for (int k = 0; k < 4; k++) st.env[k] = f();
            for (int k = 0; k < 4; k++) st.fog[k] = f();
            for (int k = 0; k < 4; k++) st.blend[k] = f();
            st.prim_lod_frac = f(); st.k5 = f(); st.fill_color = u(); st.prim_depth = f();
            st.sc_ulx = f(); st.sc_uly = f(); st.sc_lrx = f(); st.sc_lry = f();
            for (int k = 0; k < 3; k++) st.vp_scale[k] = f();
            for (int k = 0; k < 3; k++) st.vp_trans[k] = f();
            for (int k = 0; k < 2; k++) {
                TexMap& m = st.tmap[k];
                m.shift_s = f(); m.shift_t = f(); m.offset_s = f(); m.offset_t = f();
                const uint32_t b = u();
                m.clamp_s = b & 1; m.clamp_t = (b >> 1) & 1; m.mirror_s = (b >> 2) & 1; m.mirror_t = (b >> 3) & 1;
                m.clamp_max_s = f(); m.clamp_max_t = f();
            }
            st.rect_explicit = u();
            break;
        }
        case kOpTexData: {
            if (len != 4 + kTileWords + 1024) { *error = "bad TEXDATA"; return kFailed; }
            const uint64_t key = (uint64_t)from_be(p[0]) << 32 | from_be(p[1]);
            const uint32_t tlut = from_be(p[2]);
            Tile t;
            uint32_t* tf[] = {&t.fmt, &t.siz, &t.line, &t.tmem, &t.palette, &t.cms, &t.cmt, &t.masks, &t.maskt, &t.shifts, &t.shiftt,
                              &t.uls, &t.ult, &t.lrs, &t.lrt};
            for (int k = 0; k < 15; k++) *tf[k] = from_be(p[3 + k]);
            uint8_t tmem[4096];
            for (int k = 0; k < 1024; k++) {
                const uint32_t w = from_be(p[3 + kTileWords + k]);
                tmem[k * 4] = (uint8_t)(w >> 24); tmem[k * 4 + 1] = (uint8_t)(w >> 16); tmem[k * 4 + 2] = (uint8_t)(w >> 8); tmem[k * 4 + 3] = (uint8_t)w;
            }
            cache_.get_hashed(key, tmem, t, tlut);
            break;
        }
        case kOpTris: case kOpTrisMid: {
            const uint32_t pad = tris_pad(pos);
            if (len < 1 + 2 * kTexRefWords + pad) { *error = "bad TRIS"; return kFailed; }
            const uint32_t n = (len - 1 - 2 * kTexRefWords - pad) / (op == kOpTrisMid ? kVertexWords + 4 : kVertexWords);
            // In the in-between frame, images that aren't frame-sized (a shadow's silhouette, drawn
            // from its own camera) are drawn as in the frame itself: the ground it's projected onto has
            // this frame's texture coordinates, which match only this frame's silhouette.
            if (op == kOpTrisMid && mid_pass && st_.cimg_width >= 256) {
                // The in-between frame: each vertex with its second position.
                st_.tex[0] = texref(p, 0);
                st_.tex[1] = texref(p + kTexRefWords, 1);
                const uint32_t* vp = p + 2 * kTexRefWords + pad;
                const uint32_t* mp = vp + n * kVertexWords;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                if (((uintptr_t)vp & 15) == 0) {  // the records are OutVertex, then the positions (4 floats)
                    be.draw_triangles_pos(st_, reinterpret_cast<const OutVertex*>(vp), reinterpret_cast<const float (*)[4]>(mp), n);
                    break;
                }
#endif
                for (uint32_t done = 0; done < n;) {
                    const uint32_t m = (n - done) < 3 * 512 ? (n - done) : 3 * 512;
                    for (uint32_t k = 0; k < m; k++) {
                        const uint32_t* q = vp + (done + k) * kVertexWords;
                        const uint32_t* r = mp + (done + k) * 4;
                        OutVertex& o = mid_verts_[k];
                        o.x = bitsf(from_be(r[0])); o.y = bitsf(from_be(r[1])); o.z = bitsf(from_be(r[2])); o.w = bitsf(from_be(r[3]));
                        o.s = bitsf(from_be(q[4])); o.t = bitsf(from_be(q[5]));
                        const uint32_t c = from_be(q[6]);
                        o.c[0] = (uint8_t)(c >> 24); o.c[1] = (uint8_t)(c >> 16); o.c[2] = (uint8_t)(c >> 8); o.c[3] = (uint8_t)c;
                        o.pad = 0;
                    }
                    be.draw_triangles(st_, mid_verts_, m);
                    done += m;
                }
                break;
            }
            st_.tex[0] = texref(p, 0);
            st_.tex[1] = texref(p + kTexRefWords, 1);
            const uint32_t* vp = p + 2 * kTexRefWords + pad;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            if (((uintptr_t)vp & 15) == 0) {
                be.draw_triangles(st_, reinterpret_cast<const OutVertex*>(vp), n);  // the records are OutVertex
                break;
            }
#endif
            OutVertex verts[96];
            for (uint32_t done = 0; done < n;) {
                const uint32_t m = (n - done) < 96 ? (n - done) : 96;  // whole triangles (96 = 3 * 32)
                for (uint32_t k = 0; k < m; k++) {
                    const uint32_t* q = vp + (done + k) * kVertexWords;
                    OutVertex& o = verts[k];
                    o.x = bitsf(from_be(q[0])); o.y = bitsf(from_be(q[1])); o.z = bitsf(from_be(q[2])); o.w = bitsf(from_be(q[3]));
                    o.s = bitsf(from_be(q[4])); o.t = bitsf(from_be(q[5]));
                    const uint32_t c = from_be(q[6]);
                    o.c[0] = (uint8_t)(c >> 24); o.c[1] = (uint8_t)(c >> 16); o.c[2] = (uint8_t)(c >> 8); o.c[3] = (uint8_t)c;
                }
                be.draw_triangles(st_, verts, m);
                done += m;
            }
            break;
        }
        case kOpFill:
            st_.tex[0] = st_.tex[1] = nullptr;
            be.fill_rect(st_, bitsf(from_be(p[0])), bitsf(from_be(p[1])), bitsf(from_be(p[2])), bitsf(from_be(p[3])));
            break;
        case kOpTexRect: {
            st_.tex[0] = texref(p, 0);
            st_.tex[1] = texref(p + kTexRefWords, 1);
            const uint32_t* q = p + 2 * kTexRefWords;
            be.tex_rect(st_, bitsf(from_be(q[0])), bitsf(from_be(q[1])), bitsf(from_be(q[2])), bitsf(from_be(q[3])),
                        bitsf(from_be(q[4])), bitsf(from_be(q[5])), bitsf(from_be(q[6])), bitsf(from_be(q[7])), from_be(q[8]) != 0);
            break;
        }
        case kOpClearZ:
            be.clear_depth(from_be(p[0]), bitsf(from_be(p[1])), bitsf(from_be(p[2])), bitsf(from_be(p[3])), bitsf(from_be(p[4])));
            break;
        case kOpBeginDl: be.begin_dl(); break;
        case kOpEndDl: be.end_dl(); break;
        case kOpEnd: pos += len; return kDone;
        case kOpOverflow: *error = "the recorder ran out of room"; return kFailed;
        default: *error = "unknown record"; return kFailed;
        }
        pos += len;
    }
    return kMore;
}
#endif

}  // namespace n64gfx
