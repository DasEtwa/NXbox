# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native ordered comparisons omit redundant per-lane NaN predicates when opted in."""

import unittest

from test_d3d12_draw_bookkeeping import compile_run
from test_d3d12_subresource_feedback import ROOT, function

HLSL = ROOT / "src/shader_recompiler/backend/hlsl"


class NativeCompareTests(unittest.TestCase):
    def test_bisect_requires_exact_one(self):
        helper = function(
            (HLSL / "emit_hlsl_floating_point.cpp").read_text(), "bool BisectEnabled("
        )
        compile_run(
            "#include <cassert>\n#include <cstdlib>\n#include <string_view>\n"
            + helper
            + r"""
int main() {
    constexpr auto name = "NXBOX_HLSL_NATIVE_ORDERED_COMPARE";
    unsetenv(name);
    assert(!BisectEnabled(name));
    for (const char* value : {"", "0", "10", "true"}) {
        setenv(name, value, 1);
        assert(!BisectEnabled(name));
    }
    setenv(name, "1", 1);
    assert(BisectEnabled(name));
}
"""
        )

    def test_comparison_emission_preserves_unordered_not_equal_and_fp64(self):
        method = function((HLSL / "emit_hlsl_floating_point.cpp").read_text(), "void Compare(")
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                compile_run(
                    "constexpr bool native_enabled = "
                    + str(enabled).lower()
                    + ";\n"
                    + r"""
#include <array>
#include <cassert>
#include <string>
#include <string_view>
namespace IR {
enum class Type { F16, F32, F64 };
struct Value { IR::Type type; IR::Type Type() const { return type; } };
struct Inst { Value value; Value Arg(unsigned) const { return value; } };
}
[[maybe_unused]] bool BisectEnabled(const char* name) {
    assert(std::string_view{name} == "NXBOX_HLSL_NATIVE_ORDERED_COMPARE");
    return native_enabled;
}
struct EmitContext {
    std::string code;
    template<class... Args>
    void AddU1(std::string_view format, IR::Inst&, Args... args) {
        const std::array<std::string_view, sizeof...(Args)> values{args...};
        code = format;
        code.replace(code.find("{}"), 2, "result");
        for (const auto value : values)
            code.replace(code.find("{}"), 2, value);
    }
};
"""
                    + method
                    + r"""
int main() {
    EmitContext context;
    for (auto type : {IR::Type::F16, IR::Type::F32, IR::Type::F64}) {
        IR::Inst instruction{{type}};
        for (bool ordered : {false, true}) {
            for (std::string_view op : {"<", ">", "<=", ">=", "==", "!="}) {
                Compare(context, instruction, "lhs", "rhs", op, ordered);
                const bool simple = native_enabled && ordered && op != "!=" &&
                                    type != IR::Type::F64;
                assert((context.code.find("isnan") == std::string::npos) == simple);
                if (simple)
                    assert(context.code == std::string("result=lhs") + std::string(op) + "rhs;");
                else
                    assert(context.code.find(ordered ? "&&!" : "||") != std::string::npos);
            }
        }
    }
}
"""
                )


if __name__ == "__main__":
    unittest.main()
