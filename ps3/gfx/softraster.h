// Software rasterizer backend for n64gfx: renders frames into RGBA8 images on the workstation, so the
// display list interpreter can be checked by looking at the pictures. Not used on the PS3.
#pragma once
#include <map>
#include <vector>

#include "n64gfx.h"
#include "widescreen.h"

namespace n64gfx {

class SoftRaster : public Backend {
public:
    struct Target {
        uint32_t width = 320, height = 480, bpp = 2;
        uint32_t max_y = 0;  // lowest row drawn + 1
        // Widescreen (widescreen.h): `margin` extra columns each side; a row is stride = width + 2 * margin
        // pixels, N64 x at column x + margin.
        uint32_t margin = 0, stride = 320;
        std::vector<uint8_t> rgba;  // stride * height * 4
        size_t index(int x, int y) const { return (size_t)y * stride + (size_t)(x + (int)margin); }
    };

    void draw_triangles(const DrawState& st, const OutVertex* v, size_t count) override;
    void begin_dl() override { film_.next_list(); views_.next_list(); }
    void fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) override;
    void tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry,
                  float s, float t, float dsdx, float dtdy, bool flip) override;
    void clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) override;

    // The color buffer that contains this address (e.g. the VI origin), or null.
    const Target* find(uint32_t addr, uint32_t* row_offset = nullptr) const;
    std::map<uint32_t, Target> targets;
    std::map<uint32_t, std::vector<float>> depths;  // by depth image address (laid out as its color target)
    std::map<uint32_t, uint32_t> depth_margin_;
    bool trace = false;  // print every rectangle with its state
    bool mipmaps = false;  // sample DrawState::mip by each pixel's texel density (as the RSX does), not level 0 alone
    float pix_lod_ = 0;    // that density's log2, for the pixel being drawn
    int draw_id_ = 0;      // trace: triangle draws so far, and which one last wrote each depth
    std::map<const std::vector<float>*, std::vector<int>> zwho_;
    bool traced_tris_ = false;

private:
    SplitViews views_;  // split-screen views, for rectangles drawn over them (widescreen.h)
    WideFilm film_;  // the old-film overlay spread across a widened picture (widescreen.h)
    // A copy of st whose frame textures (Texture::fb_addr) are real images cut from the color image.
    const DrawState& with_frame_textures(const DrawState& st);
    DrawState fb_state_;
    Texture fb_tex_[2];
    std::vector<uint8_t> fb_pixels_[2];
    Target& target(const DrawState& st);
    std::vector<float>& depth(const DrawState& st, const Target& t);
    void shade_pixel(const DrawState& st, Target& t, std::vector<float>* z, int x, int y, float depth,
                     const float shade[4], float s, float tt, bool use_z);
};

// The combiner as the software rasterizer evaluates it (cycles in use), for checking generated programs.
void reference_combine(const DrawState& st, const float tex[2][4], const float shade[4], float out[4]);

// Writes an RGBA8 image as a PNG file (miniz).
bool write_png(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace n64gfx
