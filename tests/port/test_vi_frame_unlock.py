# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production unlock policy, composer retention and VI event cadence."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_vi_vblank as vblank

ROOT = Path(__file__).resolve().parents[2]

MOCK = r"""
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using f32 = float;
namespace boost::container {
template<class K, class V> using flat_map = std::map<K, V>;
template<class T, unsigned N> using small_vector = std::vector<T>;
}
namespace Service::android {
struct Fence { static Fence NoFence() { return {}; } };
enum class BufferTransformFlags : u32 {};
struct GraphicBuffer {
    u32 BufferId() const { return 42; }
    u32 Offset() const { return 0; }
    u32 ExternalFormat() const { return 7; }
    u32 Width() const { return 1280; }
    u32 Height() const { return 720; }
    u32 Stride() const { return 1280; }
};
struct BufferItem {
    s32 swap_interval = 1;
    std::shared_ptr<GraphicBuffer> graphic_buffer = std::make_shared<GraphicBuffer>();
    u32 transform = 0, crop = 123;
    Fence fence;
};
enum class Status { NoError };
struct BufferItemConsumer {
    s32 next_interval = 1;
    unsigned acquired = 0, released = 0;
    Status AcquireBuffer(BufferItem* item, Fence, bool) {
        item->swap_interval = next_interval; ++acquired; return Status::NoError;
    }
    void ReleaseBuffer(const BufferItem&, Fence) { ++released; }
};
}
namespace Service::Nvnflinger {
struct HwcLayer {
    u32 buffer_handle, offset, format, width, height, stride;
    s32 z_index;
    u32 blending;
    android::BufferTransformFlags transform;
    u32 crop_rect;
    android::Fence acquire_fence;
    u32 layer_stack_mask;
};
struct Layer {
    s32 consumer_id = 1;
    bool is_overlay = false, visible = true;
    s32 z_index = 0;
    u32 blending = 0, layer_stack_mask = 15;
    std::shared_ptr<android::BufferItemConsumer> buffer_item_consumer =
        std::make_shared<android::BufferItemConsumer>();
};
struct Stack {
    std::vector<std::shared_ptr<Layer>> layers;
    auto FindLayer(s32 id) {
        for (auto layer : layers) if (layer->consumer_id == id) return layer;
        return std::shared_ptr<Layer>{};
    }
};
struct Display { Stack stack; };
}
namespace Service::Nvidia::Devices {
class nvdisp_disp0 {
public:
    unsigned presents = 0;
    void WaitForComposite() {}
    void Composite(const std::vector<Service::Nvnflinger::HwcLayer>& stack) {
        assert(stack.size() == 1);
        const auto& item = stack.front();
        assert(item.buffer_handle == 42 && item.width == 1280 && item.height == 720);
        assert(item.crop_rect == 123 && item.layer_stack_mask == 15);
        ++presents;
    }
};
}
"""

HARNESS = r"""
#include <array>
#include <cstdlib>
#include <iostream>
#include "core/hle/service/vi/frame_unlock.h"
#include "core/hle/service/nvnflinger/hardware_composer.h"
int main() {
    using namespace Service;
    using VI::ForceSwapInterval;
    const std::array<std::uint64_t, 1> defaults{42};
    assert(ForceSwapInterval(42, nullptr, defaults));
    assert(!ForceSwapInterval(43, nullptr, defaults));
    assert(!ForceSwapInterval(42, "0", defaults));
    assert(!ForceSwapInterval(42, "10", defaults));
    assert(ForceSwapInterval(43, "1", defaults));
    assert(!ForceSwapInterval(42, nullptr)); // shipped table is empty
    const char* option = std::getenv("NXBOX_FORCE_SWAP_INTERVAL");
    bool enabled = option && std::string_view(option) == "1";
#ifndef NXBOX_UWP
    enabled = false;
#endif
    VI::FrameUnlock policy;
    auto first = policy.Observe(42, 2);
    assert(first.changed && first.effective == (enabled ? 1 : 2));
    assert(!policy.Observe(42, 2).changed);
    assert(policy.Observe(42, 1).changed);
    assert(policy.Observe(42, 2).changed); // Wonder dynamically returns to interval 2
    assert(policy.Observe(43, 2).changed); // new title must be identified even if interval matches
    for (int interval : {-2, -1, 0, 1, 2, 3, 4, 5, 100, 200}) {
        const auto effective = policy.Observe(43, interval).effective;
        assert(effective == (enabled && interval >= 2 && interval <= 4 ? 1 : interval));
    }
    for (int interval : {1, 2, 3, 4}) {
        Nvnflinger::HardwareComposer composer;
        Nvnflinger::Display display;
        auto layer = std::make_shared<Nvnflinger::Layer>();
        display.stack.layers.push_back(layer);
        auto& queue = *layer->buffer_item_consumer;
        queue.next_interval = policy.Observe(43, interval).effective;
        Nvidia::Devices::nvdisp_disp0 output;
        for (unsigned tick = 0; tick < 120; ++tick) {
            float scale = 0;
            assert(composer.ComposeLocked(&scale, display, output) == 1);
            assert(scale == 1.f);
        }
        unsigned expected = 120 / (enabled ? 1 : interval);
        assert(queue.acquired == expected && queue.released == expected);
        assert(output.presents == expected);
        std::cout << "requested=" << interval << " effective=" << queue.next_interval
                  << " presents/120-vblanks=" << output.presents << '\n';
    }
}
"""


class FrameUnlockTests(unittest.TestCase):
    def compile(self, directory, sources, uwp=True, extra=()):
        executable = directory / "test"
        subprocess.run(
            [
                shutil.which("clang++"),
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-pthread",
                *(["-DNXBOX_UWP"] if uwp else []),
                "-I",
                str(directory),
                "-I",
                str(ROOT / "src"),
                *extra,
                *map(str, sources),
                "-o",
                str(executable),
            ],
            check=True,
        )
        return executable

    def test_policy_and_production_composer(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-unlock-") as name:
            directory = Path(name)
            (directory / "mock.h").write_text(MOCK)
            for header in (
                "boost/container/flat_map.hpp",
                "boost/container/small_vector.hpp",
                "core/hle/service/nvnflinger/buffer_item.h",
                "core/hle/service/nvnflinger/buffer_item_consumer.h",
                "core/hle/service/nvnflinger/display.h",
                "core/hle/service/nvnflinger/hwc_layer.h",
                "core/hle/service/nvnflinger/ui/graphic_buffer.h",
                "core/hle/service/nvdrv/devices/nvdisp_disp0.h",
            ):
                path = directory / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "mock.h"\n')
            harness = directory / "main.cpp"
            harness.write_text(HARNESS)
            for uwp in (True, False):
                executable = self.compile(
                    directory,
                    [harness, ROOT / "src/core/hle/service/nvnflinger/hardware_composer.cpp"],
                    uwp=uwp,
                )
                for option in (None, "0", "1", "10"):
                    environment = dict(os.environ)
                    environment.pop("NXBOX_FORCE_SWAP_INTERVAL", None)
                    if option is not None:
                        environment["NXBOX_FORCE_SWAP_INTERVAL"] = option
                    with self.subTest(uwp=uwp, option=option):
                        subprocess.run([str(executable)], env=environment, check=True, timeout=10)

    def test_queue_keeps_original_request_and_applies_policy_before_enqueue(self):
        source = (ROOT / "src/core/hle/service/nvnflinger/buffer_queue_producer.cpp").read_text()
        queue = source[source.index("Status BufferQueueProducer::QueueBuffer(") :]
        accepted = queue.index("slots[slot].buffer_state = BufferState::Queued;")
        observe = queue.index("frame_unlock.Observe(program_id, swap_interval)")
        effective = queue.index("item.swap_interval = observation.effective;")
        enqueue = queue.index("core->queue.push_back(item);")
        self.assertLess(accepted, observe)
        self.assertLess(observe, effective)
        self.assertLess(effective, enqueue)
        self.assertIn("GetApplicationProcessProgramID()", queue)
        self.assertIn("if (observation.changed)", queue)
        self.assertIn("requested={} effective={}", queue)
        self.assertIn("VI::vblank_timing.RecordSwapInterval(swap_interval);", queue)

    def test_production_vsync_events_remain_60hz_with_unlock_on_and_off(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-unlock-vblank-") as name:
            directory = Path(name)
            for header, content in vblank.STUBS.items():
                path = directory / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            harness = directory / "main.cpp"
            cadence_harness = vblank.HARNESS.replace(
                "        tick();\n        tick();\n",
                "        for (int i = 0; i < 59; ++i) tick();\n",
            )
            cadence_harness = cadence_harness.replace("3u : 0u", "60u : 0u")
            cadence_harness = cadence_harness.replace("4u : 0u", "61u : 0u")
            cadence_harness = cadence_harness.replace("sample.ticks == 4", "sample.ticks == 61")
            harness.write_text(cadence_harness)
            executable = self.compile(
                directory,
                [
                    harness,
                    ROOT / "src/core/hle/service/vi/conductor.cpp",
                    ROOT / "src/core/hle/service/vi/vsync_manager.cpp",
                ],
            )
            for unlock in ("0", "1"):
                for independent in ("0", "1"):
                    for single in (False, True):
                        subprocess.run(
                            [str(executable)] + (["single"] if single else []),
                            env={
                                **os.environ,
                                "NXBOX_FORCE_SWAP_INTERVAL": unlock,
                                "NXBOX_VI_INDEPENDENT_VSYNC": independent,
                                "NXBOX_THREAD_PROFILE": "1",
                            },
                            check=True,
                            timeout=10,
                        )


if __name__ == "__main__":
    unittest.main()
