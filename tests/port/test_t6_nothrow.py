# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fault injection at T6 boundaries; host stubs replace platform dependencies only."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NoThrowTests(unittest.TestCase):
    def run_cpp(self, source, stubs=None, defines=()):
        with tempfile.TemporaryDirectory(prefix="nxbox-t6-") as directory:
            directory = Path(directory)
            for name, content in (stubs or {}).items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            cpp = directory / "test.cpp"
            cpp.write_text(source)
            binary = directory / "test"
            result = subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "clang++")),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    *[f"-D{define}" for define in defines],
                    f"-I{directory}",
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_worker_survives_tasks_initialization_and_logger_failures(self):
        stubs = {
            "common/logging.h": "#define LOG_CRITICAL(category, ...) TestLog(__VA_ARGS__)\n",
            "common/thread.h": """
#pragma once
namespace Common {
enum class ThreadPlacement { Default, Efficiency, Background };
enum class ThreadPriority { Low };
inline void SetCurrentThreadName(const char*) {}
inline void SetCurrentThreadPriority(ThreadPriority) {}
inline void SetCurrentThreadToEfficiencyCores() {}
inline void SetCurrentThreadToBackgroundWork() {}
inline void SetCurrentThreadToAllCores() {}
}
""",
        }
        source = r"""
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <string_view>
std::atomic<int> logs{}, crashes{}, successes{};
std::atomic<bool> saw_reason{};
template<class... Args> void TestLog(Args&&...) { ++logs; throw std::bad_alloc{}; }
#include "common/thread_worker.h"
namespace EdenXbox {
void CrashRendererException(const char*, std::string_view reason) noexcept {
    ++crashes;
    if (reason == "task reason") saw_reason = true;
}
}
struct State {
    int value = 42;
    State() = default;
    State(const State&) = delete;
    State(State&&) = delete;
};
int main() {
    {
        Common::ThreadWorker worker{1, "shader worker"};
        worker.QueueWork([] { throw std::runtime_error("task reason"); });
        worker.QueueWork([] { throw 19; });
        worker.QueueWork([] { ++successes; });
        worker.WaitForRequests();
        assert(successes == 1 && logs == 2);
    }
    {
        int attempts = 0;
        Common::StatefulThreadWorker<State> worker{1, "state worker", [&] {
            if (++attempts == 1) throw std::runtime_error("init reason");
            if (attempts == 2) throw 7;
            return State{};
        }};
        // First task retries initialization and fails, next task gets a valid state.
        worker.QueueWork([](State*) { assert(false); });
        worker.QueueWork([](State* state) { assert(state->value == 42); ++successes; });
        worker.QueueWork([](State*) { throw std::runtime_error("state task reason"); });
        worker.QueueWork([](State*) { ++successes; });
        worker.WaitForRequests();
        assert(attempts == 3 && successes == 3 && logs == 5);
    }
#ifdef NXBOX_UWP
    assert(crashes == 10 && saw_reason);
#else
    assert(crashes == 0);
#endif
}
"""
        for defines in ((), ("NXBOX_UWP",)):
            with self.subTest(defines=defines):
                self.run_cpp(source, stubs, defines)

    def test_macro_allocation_failure_propagates(self):
        header = (ROOT / "src/video_core/macro.h").read_text()
        add_code = re.search(r"inline void AddCode\([^}]+\}", header).group()
        clear_code = re.search(r"inline void ClearCode\([^}]+\}", header).group()
        self.assertNotIn("noexcept", clear_code)
        self.run_cpp(
            r"""
#include <cassert>
#include <new>
#include <vector>
using u32 = unsigned;
struct Uploads {
    std::vector<u32> code;
    bool fail = true;
    std::vector<u32>& operator[](u32) {
        if (fail) throw std::bad_alloc{};
        return code;
    }
};
struct Engine {
    Uploads uploaded_macro_code;
"""
            + add_code
            + r"""
};
int main() {
    Engine engine;
    static_assert(!noexcept(engine.AddCode(0, 1)));
    try { engine.AddCode(0, 1); assert(false); } catch (const std::bad_alloc&) {}
    engine.uploaded_macro_code.fail = false;
    engine.AddCode(0, 2);
    assert(engine.uploaded_macro_code.code[0] == 2);
}
"""
        )

    def test_jit_error_callback_defers_and_exits_generated_frame(self):
        source = (ROOT / "src/video_core/macro.cpp").read_text()
        thunk = source[
            source.index("static void MacroJIT_ErrorThunk(") : source.index(
                "Xbyak::Reg32 MacroJITx64Impl::Compile_FetchParameter()"
            )
        ]
        self.assertIn("noexcept", thunk)
        self.assertIn("state->failure.Invoke", thunk)
        fetch = source[source.index("Xbyak::Reg32 MacroJITx64Impl::Compile_FetchParameter() {") :]
        self.assertLess(
            fetch.index("jmp(end_of_code, T_NEAR)"), fetch.index("mov(eax, dword[PARAMETERS])")
        )
        self.assertIn("state.failure.Rethrow();", source)
        # Execute the production thunk with only its formatting dependency replaced.
        self.run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "common/deferred_exception.h"
namespace fmt {
template<class... Args> std::string format(const char*, Args...) { return "invalid parameter"; }
}
struct MacroJITx64Impl { struct JITState { Common::DeferredException failure; }; };
"""
            + thunk
            + r"""
int main() {
    MacroJITx64Impl::JITState state;
    MacroJIT_ErrorThunk(&state, 0, 0);
    assert(state.failure.HasError());
    try { state.failure.Rethrow(); assert(false); } catch (const std::out_of_range&) {}
    assert(!state.failure.HasError());
}
"""
        )

    def test_blit_subresource_signedness(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_blit_image.cpp").read_text()
        validate = source[
            source.index("void Validate(const BlitSurface& surface)") : source.index(
                "void ValidateRegion("
            )
        ]
        self.run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include <stdexcept>
using u32 = std::uint32_t;
enum Dimension { D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_RESOURCE_DIMENSION_TEXTURE3D,
                 D3D12_RESOURCE_DIMENSION_TEXTURE2D };
struct Desc { Dimension Dimension; std::uint16_t MipLevels, DepthOrArraySize; };
struct Handle { Desc desc; auto GetDesc() { return desc; } };
struct Resource { Handle* handle; };
struct Format { bool Supported() const { return true; } };
struct BlitSurface { Resource* resource; Format format; u32 level, layer; };
"""
            + validate
            + r"""
int main() {
    Handle handle{{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 2, 4}};
    Resource resource{&handle};
    BlitSurface surface{&resource, {}, 1, 3};
    Validate(surface);
    surface.layer = 4;
    try { Validate(surface); assert(false); } catch (const std::out_of_range&) {}
    handle.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    surface.layer = 0;
    Validate(surface);
    surface.layer = 1;
    try { Validate(surface); assert(false); } catch (const std::out_of_range&) {}
}
"""
        )

    def test_renderer_fallback_and_context_lifetimes(self):
        source = (ROOT / "src/video_core/video_core.cpp").read_text()
        factory = source[
            source.index("std::unique_ptr<VideoCore::RendererBase> CreateRenderer(") : source.index(
                "} // Anonymous namespace"
            )
        ]
        self.run_cpp(
            r"""
#include <cassert>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#define LOG_CRITICAL(...) ((void)0)
namespace Settings {
enum class RendererBackend { OpenGL_GLSL, OpenGL_GLASM, OpenGL_SPIRV, Vulkan, D3D12, Null };
struct Setting {
    RendererBackend value = RendererBackend::D3D12;
    auto GetValue() { return value; }
    void SetValue(RendererBackend v) { value = v; }
};
struct Values { Setting renderer_backend; } values;
}
int destroyed = 0, released = 0, created = 0, mode = 0, reports = 0;
namespace EdenXbox {
void CrashRendererException(const char*, std::string_view reason) noexcept {
    assert(!reason.empty()); ++reports;
}
}
namespace Core::Frontend {
struct GraphicsContext {
    bool alive = true;
    ~GraphicsContext() { alive = false; ++destroyed; }
    void DoneCurrent() { assert(alive); ++released; }
    struct Scoped {
        GraphicsContext& context;
        bool active = true;
        ~Scoped() { if (active) context.DoneCurrent(); }
        void Cancel() { active = false; }
    };
    auto Acquire() { return Scoped{*this}; }
};
struct EmuWindow {
    auto CreateSharedContext() { ++created; return std::make_unique<GraphicsContext>(); }
};
}
namespace Core {
struct Host { int memory = 0; int& MemoryManager() { return memory; } };
struct System { Host host; Host& Host1x() { return host; } };
}
namespace Tegra { struct GPU {}; }
namespace VideoCore {
struct RendererBase {
    std::unique_ptr<Core::Frontend::GraphicsContext> context;
    int backend;
    RendererBase(std::unique_ptr<Core::Frontend::GraphicsContext> c, int b) : context(std::move(c)), backend(b) {}
    virtual ~RendererBase() = default;
    auto& Context() { return *context; }
};
}
#define RENDERER(NS, NAME, VALUE) namespace NS { struct NAME : VideoCore::RendererBase { \
NAME(Core::Frontend::EmuWindow&, int&, Tegra::GPU&, std::unique_ptr<Core::Frontend::GraphicsContext> c) \
: RendererBase(std::move(c), VALUE) {} }; }
RENDERER(OpenGL, RendererOpenGL, 1)
RENDERER(Vulkan, RendererVulkan, 2)
namespace Null { struct RendererNull : VideoCore::RendererBase {
RendererNull(Core::Frontend::EmuWindow&, Tegra::GPU&, std::unique_ptr<Core::Frontend::GraphicsContext> c)
: RendererBase(std::move(c), 3) {} }; }
namespace D3D12 { struct RendererD3D12 : VideoCore::RendererBase {
RendererD3D12(Core::Frontend::EmuWindow&, int&, Tegra::GPU&, std::unique_ptr<Core::Frontend::GraphicsContext> c)
: RendererBase(std::move(c), 4) {
    if (mode == 1) throw std::runtime_error("device creation HRESULT reason");
    if (mode == 2) throw std::bad_alloc{};
    if (mode == 3) throw 42;
}
}; }
"""
            + factory
            + r"""
int main() {
    Core::System system;
    Core::Frontend::EmuWindow window;
    Tegra::GPU gpu;
    for (mode = 0; mode <= 3; ++mode) {
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::D3D12);
        auto context = window.CreateSharedContext();
        auto scope = context->Acquire();
        auto renderer = CreateRenderer(system, window, gpu, scope, std::move(context));
        assert(renderer->backend == (mode == 0 ? 4 : 1));
        assert(scope.active == (mode == 0));
        // Scope must finish before the renderer owning its context is destroyed.
        if (scope.active) { renderer->Context().DoneCurrent(); scope.Cancel(); }
    }
    assert(created == 7 && destroyed == 7 && released == 4 && reports == 3);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::OpenGL_GLSL);
    auto context = window.CreateSharedContext();
    auto scope = context->Acquire();
    auto renderer = CreateRenderer(system, window, gpu, scope, std::move(context));
    assert(renderer->backend == 1 && scope.active);
    renderer->Context().DoneCurrent(); scope.Cancel();
}
""",
            defines=("HAS_D3D12", "HAS_OPENGL", "NXBOX_UWP"),
        )


if __name__ == "__main__":
    unittest.main()
