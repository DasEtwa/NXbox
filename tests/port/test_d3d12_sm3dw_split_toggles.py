"""SM3DW diagnostics separate strict DXIL and replace only HLSL cube samples."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"
HLSL = ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl_image.cpp"


class Sm3dwSplitToggles(unittest.TestCase):
    def test_strict_toggle_is_read_once_and_defaults_off(self):
        source = (D3D12 / "d3d12_context.cpp").read_text()
        block = source[source.index("bool DxcStrictEnabled()") :]
        block = block[: block.index("bool AsyncPsoEnabled()")]
        self.assertIn('static const bool enabled = BisectEnabled("NXBOX_D3D12_DXC_STRICT");', block)
        self.assertIn("return enabled;", block)
        bisect = source[
            source.index("bool BisectEnabled(") : source.index("bool DxcStrictEnabled()")
        ]
        self.assertIn("value[0] == '1'", bisect)
        self.assertIn("GetEnvironmentVariableA(name, value, sizeof(value)) == 1", bisect)

    def test_strict_dxc_argument_is_conditional(self):
        source = (D3D12 / "d3d12_shader.cpp").read_text()
        block = source[source.index("std::vector<u8> ShaderCompiler::Compile") :]
        self.assertIn('if (DxcStrictEnabled())\n            arguments.push_back(L"-Gis");', block)
        self.assertIn("arguments.data(), static_cast<UINT32>(arguments.size())", block)
        self.assertEqual(block.count('L"-Gis"'), 1)

    def test_strict_cache_hash_is_salted_before_lookup_and_store(self):
        source = (D3D12 / "d3d12_dxc.cpp").read_text()
        block = source[source.index("DxilBlob DxcCache::Compile") :]
        self.assertIn("if (DxcStrictEnabled()) {", block)
        self.assertRegex(block, r"constexpr u64 strict_salt = 0x[0-9A-Fa-f]+ULL;")
        salt = "hash = Pso::HashBytes(&strict_salt, sizeof(strict_salt), hash);"
        self.assertIn(salt, block)
        for lookup in (
            "entries.equal_range(hash)",
            "disk->FindDxil(hash)",
            "disk->StoreDxil(hash, *dxil)",
        ):
            self.assertLess(block.index(salt), block.index(lookup))
        self.assertNotIn("FindDxil(pipeline_hash)", block)
        self.assertNotIn("StoreDxil(pipeline_hash", block)

    def test_cube_toggle_is_read_once_into_default_off_profile(self):
        profile = (ROOT / "src/shader_recompiler/profile.h").read_text()
        self.assertIn("bool hlsl_cube_one{};", profile)
        source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        block = source[source.index("explicit TranslationState(") :]
        block = block[: block.index("Shader::RuntimeInfo GraphicsRuntime")]
        self.assertIn('static const bool cube_one = BisectEnabled("NXBOX_D3D12_CUBE_ONE");', block)
        self.assertIn("profile.hlsl_cube_one = cube_one;", block)
        self.assertIn("EmitHLSL(translation.profile, runtime, program, bindings)", source)
        self.assertIn("EmitHLSL(translation.profile, program)", source)

    def test_cube_override_preserves_assignment_types_and_other_textures(self):
        source = HLSL.read_text()
        block = source[source.index("bool EmitCubeOne(") : source.index("void Sample(")]
        self.assertIn("!ctx.profile.hlsl_cube_one", block)
        self.assertIn(
            "info.type != TextureType::ColorCube && info.type != TextureType::ColorArrayCube", block
        )
        self.assertIn("return false;", block)
        self.assertIn('ctx.AddF32x4("{} = float4(1, 1, 1, 1);", inst);', block)
        self.assertIn('ctx.AddF32("{} = 1.0f;", inst);', block)
        self.assertIn("return true;", block)
        self.assertNotIn("getenv", source)
        self.assertNotIn("BisectEnabled", source)

    def test_all_cube_sampling_paths_short_circuit_before_sampling(self):
        source = HLSL.read_text()
        for start, end, guard in (
            ("void Sample(", "void Gather(", "EmitCubeOne(ctx, inst, info, !dref.empty())"),
            ("void Gather(", "u32 ImageComponents(", "EmitCubeOne(ctx, inst, info)"),
            ("void EmitImageGradient(", "void EmitImageRead(", "EmitCubeOne(ctx, inst, info)"),
        ):
            with self.subTest(path=start):
                block = source[source.index(start) : source.index(end)]
                self.assertIn(f"if ({guard})\n        return;", block)
                self.assertLess(block.index(guard), block.index("Texture(ctx, info, index)"))
        self.assertIn('method = "SampleLevel";', source)
        self.assertIn('method = "Sample";', source)
        self.assertIn('ctx.AddF32x4("{} = {}.{}({});", inst, tex, method, arguments);', source)
        self.assertIn(".SampleGrad(", source)


if __name__ == "__main__":
    unittest.main()
