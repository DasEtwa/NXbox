// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
using namespace Shader::Backend::HLSL;
int main() {
    assert(FormatFloatImmediate(0u) == "0.0f");
    assert(FormatFloatImmediate(0x3f800000u) == "1.0f");
    assert(FormatFloatImmediate(0xbf000000u) == "(-0.5f)");
    for (const auto bits : {0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0xffa12345u, 1u,
                            0x007fffffu, 0x80000001u}) {
        const auto literal = FormatFloatImmediate(bits);
        assert(literal.starts_with("asfloat(0x") && literal.ends_with("u)"));
    }
    // Cover rounding boundaries and both signs without depending on locale or fmt.
    std::uint32_t random = 12345;
    for (size_t i = 0; i < 100000; ++i) {
        random = random * 1664525u + 1013904223u;
        auto literal = FormatFloatImmediate(random);
        if (literal.starts_with("asfloat"))
            continue;
        if (literal.front() == '(')
            literal = literal.substr(1, literal.size() - 2);
        literal.pop_back();
        float value{};
        const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), value);
        assert(parsed.ec == std::errc{} && parsed.ptr == literal.data() + literal.size());
        assert(std::bit_cast<std::uint32_t>(value) == random);
    }
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
