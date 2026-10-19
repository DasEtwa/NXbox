// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
using namespace Shader::Backend::HLSL;
int main() {
    static_assert(IsDirectCbufferAddress(0u, 0u, true));
    static_assert(IsDirectCbufferAddress(17u, 65532u, true));
    static_assert(!IsDirectCbufferAddress(0u, 65536u, true));
    static_assert(!IsDirectCbufferAddress(0u, 1u, true));
    static_assert(!IsDirectCbufferAddress(0u, 2u, true));
    static_assert(!IsDirectCbufferAddress(0u, 3u, true));
    static_assert(!IsDirectCbufferAddress(0u, 0u, false));
    static_assert(!IsDirectCbufferAddress(std::nullopt, 0u, true));
    static_assert(!IsDirectCbufferAddress(0u, std::nullopt, true));
    static_assert(IsDirectCbufferAddress(0u, 28u, true, 2));
    static_assert(!IsDirectCbufferAddress(0u, 32u, true, 2));
    static_assert(!IsDirectCbufferAddress(0u, 0u, true, 0));
    const std::string declarations = "cbuffer C { uint4 data[4]; };\nstruct Input { float x; };\n";
    const std::string helpers = "uint leaf(uint x) { return x; }\n"
                                "float leaf(float x) { return x; }\n"
                                "uint live(uint x) { return leaf(x); }\n";
    const std::string entry = "[numthreads(1, 1, 1)]\nvoid main() { uint x = live(1u); }\n";
    const auto stripped = StripUncalledHelpers(
        declarations + helpers + "uint dead() { return 0u; }\nuint cycle() { return cycle(); }\n" +
        entry);
    assert(stripped == declarations + helpers + entry);
    assert(StripUncalledHelpers(stripped) == stripped);
    assert(StripUncalledHelpers("uint dead() { return 0u; }") == "uint dead() { return 0u; }");
    assert(StripUncalledHelpers("void dead() {}\nvoid main() {}\n") == "\nvoid main() {}\n");
}
