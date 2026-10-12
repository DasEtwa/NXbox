// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdint>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_clear.h"
using namespace D3D12::ClearOperations;
int main() {
    // Every color/depth/stencil enable combination and all stencil write masks.
    for (std::uint32_t bits = 0; bits < 64; ++bits)
        for (std::uint32_t mask = 0; mask < 256; ++mask)
            for (std::uint32_t respect = 0; respect < 2; ++respect) {
                const auto state = Decode(bits | (9u << 6) | (317u << 10), respect, mask);
                assert(state.color_mask == (bits >> 2));
                assert(state.depth == bool(bits & 1));
                assert(state.stencil_mask == (respect ? mask : 255));
                assert(state.stencil == bool((bits & 2) && (!respect || mask)));
                assert(state.target == 9 && state.layer == 317);
            }
    for (std::uint32_t rt = 0; rt < 16; ++rt)
        for (auto layer : {0u, 1u, 65535u}) {
            const auto selection = Decode(63u | (rt << 6) | (layer << 10), 0, 0);
            assert(selection.target == rt && selection.layer == layer);
        }
    assert(Decode(2, 1, 0xffffff00).stencil == false);
    assert(Decode(2, 1, 0xffffff7f).stencil_mask == 127);
    const Rect area{0, 0, 100, 80};
    for (auto control : {0u, 1u, 16u, 256u, 257u, 4096u, UINT32_MAX}) {
        for (bool enabled : {false, true}) {
            const auto resolved = ResolveScissor(area, control, enabled, {10, 20, 30, 40});
            const bool active = (control & 256) && enabled;
            assert(resolved.left == (active ? 10 : 0));
            assert(resolved.top == (active ? 20 : 0));
            assert(resolved.right == (active ? 30 : 100));
            assert(resolved.bottom == (active ? 40 : 80));
            assert(ResolveScissor(area, control, enabled, {10, 10, 0, 0}).Empty() == active);
        }
    }
    const auto full = Intersect(area, {-10, -20, 200, 300});
    assert(full.left == 0 && full.top == 0 && full.right == 100 && full.bottom == 80);
    const auto partial = Intersect(area, {20, -20, 40, 30});
    assert(partial.left == 20 && partial.top == 0 && partial.right == 40 && partial.bottom == 30);
    assert(!partial.Empty());
    for (auto rect : {Rect{5, 5, 5, 6}, Rect{10, 10, 5, 5}, Rect{-30, -30, -1, -1},
                      Rect{100, 0, 200, 80}, Rect{0, 80, 100, 90}})
        assert(Intersect(area, rect).Empty());
    const auto huge = Intersect(area, {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    assert(huge.left == 0 && huge.top == 0 && huge.right == 100 && huge.bottom == 80);
    // Exhaust all small signed rectangle coordinates, including inverted bounds.
    for (int l = -2; l <= 6; ++l)
        for (int t = -2; t <= 6; ++t)
            for (int r = -2; r <= 6; ++r)
                for (int b = -2; b <= 6; ++b) {
                    const auto clipped = Intersect({0, 0, 4, 4}, {l, t, r, b});
                    for (int x = 0; x < 4; ++x)
                        for (int y = 0; y < 4; ++y) {
                            const bool expected = x >= l && x < r && y >= t && y < b;
                            const bool actual = x >= clipped.left && x < clipped.right &&
                                                y >= clipped.top && y < clipped.bottom;
                            assert(actual == expected);
                        }
                }
}
