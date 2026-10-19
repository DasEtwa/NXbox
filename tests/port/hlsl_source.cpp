// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
using namespace Shader::Backend::HLSL;
int main() {
    {
        const std::vector<InlineCandidate> candidates{{"v0", "uint"}, {"v1", "uint"}};
        const auto inlined =
            InlineSingleUseBlock("v0 = 1u;\nv1 = v0 + 2u;\noutput.x = v1;\n", candidates);
        assert(inlined.eliminated.size() == 2);
        assert(inlined.source == "output.x = (uint((uint(1u)) + 2u));\n");
        for (const auto& text :
             {"v0 = 1u;\noutput.x = v0 + v0;\n", "v0 = 1u;\nv0 = 2u;\noutput.x = v0;\n",
              "v0 = 1u;\n{\noutput.x = v0;\n}\n", "v0 = 1u;\noutput.x = true ? v0 : 0u;\n",
              "output.x = v0;\nv0 = 1u;\n", "v0 = 1u;\nInterlockedAdd(buffer, v0);\n"})
            assert(InlineSingleUseBlock(text, candidates).eliminated.empty());
        assert(InlineSingleUseBlock("v0 = 1u;\noutput.x = v0;\n", {{"v0", "precise float"}})
                   .eliminated.empty());
        assert(
            InlineSingleUseBlock("v0 = 1u;\noutput.x = v0;\n", candidates, 1).eliminated.empty());
        assert(InlineSingleUseBlock("v0 = 1u;\noutput.x = v0;\n", candidates, 600, 10)
                   .eliminated.empty());
        assert(InlineSingleUseBlock("v0 = 1u;\noutput.x = v10;\n", candidates).eliminated.empty());
    }
    assert(InlineSingleUseBlock("v0 = nx_cbuf_load(0u, v2);\noutput.x = v0;\n", {{"v0", "uint"}})
               .eliminated.empty());
    // "(float(float(v2)))*2.0f" reads as a cast of "*2.0f" to the function type float(float):
    // DXC rejects it, so a nested cast stays a variable when an operand could follow it.
    {
        const std::vector<InlineCandidate> casts{{"v0", "float"}, {"v1", "float", true}};
        for (const auto& text :
             {"v0 = float(v2);\noutput.x = v0*2.0f;\n", "v0 = float(v2);\noutput.x = v0 + 1.0f;\n",
              "v0 = float(v2);\noutput.x = v0-v3;\n", "v1 = float(int((cb[0][1])));\noutput.x = v1*v3;\n"})
            assert(InlineSingleUseBlock(text, casts).eliminated.empty());
        assert(InlineSingleUseBlock("v0 = float(v2);\noutput.x = 1.0f-(v0);\n", casts).source ==
               "output.x = 1.0f-((float(float(v2))));\n");
        assert(InlineSingleUseBlock("v1 = float(v2);\noutput.x = v1/v3;\n", casts).source ==
               "output.x = (float(v2))/v3;\n");
        assert(InlineSingleUseBlock("v0 = v2 + 1.0f;\noutput.x = v0*2.0f;\n", casts).source ==
               "output.x = (float(v2 + 1.0f))*2.0f;\n");
    }
    static_assert(IsHlslTypeName("float") && IsHlslTypeName("uint4") && IsHlslTypeName("float3x4"));
    static_assert(!IsHlslTypeName("floatx") && !IsHlslTypeName("v12") && !IsHlslTypeName("float5"));
    assert(StartsWithNestedCast("float( (uint(v1)))") && !StartsWithNestedCast("float(v1)"));
    assert(!StartsWithNestedCast("asfloat(uint(v1))") && !StartsWithNestedCast("(float(uint(v1)))"));
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
