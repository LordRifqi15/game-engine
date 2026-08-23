#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine {

// CPU Hi-Z pyramid built from a downsampled depth buffer (previous frame).
// Each level halves resolution and stores the MAX depth of its children —
// conservative: occlusion is only reported when guaranteed.
struct HiZBuffer {
    // mip 0 = downsampled source; each next level = half res, max-pooled.
    std::vector<std::vector<float>> mips;
    std::vector<std::pair<uint32_t, uint32_t>> dims; // per-mip {w, h}

    bool empty() const { return mips.empty(); }

    // Build from raw depth floats (row-major, Vulkan range 0..1, 1 = far).
    void build(const std::vector<float>& depth, uint32_t w, uint32_t h) {
        mips.clear();
        dims.clear();
        if (depth.empty() || w == 0 || h == 0) return;

        auto pool = [](const std::vector<float>& src, uint32_t sw, uint32_t sh,
                       uint32_t dw, uint32_t dh) {
            std::vector<float> dst(dw * dh, 1.0f);
            for (uint32_t y = 0; y < dh; ++y)
                for (uint32_t x = 0; x < dw; ++x) {
                    float m = 0.0f;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            uint32_t sx = x * 2 + dx, sy = y * 2 + dy;
                            if (sx < sw && sy < sh)
                                m = std::max(m, src[sy * sw + sx]);
                        }
                    dst[y * dw + x] = m;
                }
            return dst;
        };

        // Mip 0: half resolution of the source.
        uint32_t lw = std::max(1u, w / 2), lh = std::max(1u, h / 2);
        mips.push_back(pool(depth, w, h, lw, lh));
        dims.emplace_back(lw, lh);

        // Halve until ~4x4.
        while (dims.back().first > 2 && dims.back().second > 2) {
            uint32_t nw = std::max(1u, dims.back().first / 2);
            uint32_t nh = std::max(1u, dims.back().second / 2);
            mips.push_back(pool(mips.back(), dims.back().first, dims.back().second, nw, nh));
            dims.emplace_back(nw, nh);
        }
    }

    // Conservative test. Screen-space pixel rect (x0,y0)-(x1,y1) + object's
    // nearest depth (Vulkan range). Occluded only when max stored depth over
    // the covered cells is nearer than the object's closest point.
    bool isOccluded(float x0, float y0, float x1, float y1, float nearestDepth,
                    uint32_t screenW, uint32_t screenH) const {
        if (empty() || nearestDepth >= 1.0f || screenW == 0 || screenH == 0)
            return false;

        // Pick mip so one cell ≈ the object's screen footprint (min side).
        const float minExtent = std::min(x1 - x0, y1 - y0);
        size_t mip = 0;
        while (mip + 1 < mips.size()) {
            float cellW = static_cast<float>(screenW) / dims[mip + 1].first;
            float cellH = static_cast<float>(screenH) / dims[mip + 1].second;
            if (cellW <= minExtent && cellH <= minExtent) ++mip;   // finer or equal
            else break;
        }
        // Walk back down if cells are coarser than the object (too conservative).
        while (mip > 0) {
            float cellW = static_cast<float>(screenW) / dims[mip].first;
            if (cellW <= minExtent) break;
            --mip;
        }

        const auto& m = mips[mip];
        const auto [mw, mh] = dims[mip];
        float scaleX = static_cast<float>(mw) / screenW;
        float scaleY = static_cast<float>(mh) / screenH;

        int cx0 = std::clamp(static_cast<int>(x0 * scaleX), 0, static_cast<int>(mw) - 1);
        int cy0 = std::clamp(static_cast<int>(y0 * scaleY), 0, static_cast<int>(mh) - 1);
        int cx1 = std::clamp(static_cast<int>(x1 * scaleX), 0, static_cast<int>(mw) - 1);
        int cy1 = std::clamp(static_cast<int>(y1 * scaleY), 0, static_cast<int>(mh) - 1);

        float hizMax = 0.0f;
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x)
                hizMax = std::max(hizMax, m[static_cast<size_t>(y) * mw + x]);

        return nearestDepth >= hizMax;
    }
};

} // namespace engine
