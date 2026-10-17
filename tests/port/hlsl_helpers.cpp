// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <iostream>
#include <string_view>
#include "shader_recompiler/backend/hlsl/hlsl_helpers.h"
using namespace Shader::Backend::HLSL;
void Require(bool value, std::string_view message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    constexpr RootConstants defaults;
    Require(defaults.resolution_down_factor == 1 && defaults.y_direction == 1,
            "Identity root state");
    Require(sizeof(defaults) == 64 && offsetof(RootConstants, draw_id) == 40, "Root ABI");
    Require(RegisterDeclaration("RWByteAddressBuffer", "storage", 'u', 3) ==
                "RWByteAddressBuffer storage : register(u3, space0);\n",
            "UAV register declaration");
    Require(RegisterDeclaration("Texture2D<float4>", "texture", 't', 4, 3) ==
                "Texture2D<float4> texture[3] : register(t4, space0);\n",
            "Array register declaration");
    Require(ROOT_CONSTANT_DECLARATION.find("register(b0, space1)") != std::string_view::npos,
            "Root register space");
    Require(WAVE_HELPERS.find("WaveActiveBallot(predicate)[WaveGetLaneIndex() >> 5]") !=
                std::string_view::npos,
            "Partition host wave into 32-lane ballots");
    Require(WAVE_HELPERS.find("WaveReadLaneAt(value, host_source)") != std::string_view::npos,
            "Shuffle host lane");
}
