// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "common/assert.h"
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include <ranges>
#include "shader_recompiler/environment.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

constexpr std::array<char, 8> MAGIC_NUMBER{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};

constexpr size_t INST_SIZE = sizeof(u64);

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

static u64 MakeCbufKey(u32 index, u32 offset) {
    return (static_cast<u64>(index) << 32) | offset;
}

static Shader::TextureType ConvertTextureType(const Tegra::Texture::TICEntry& entry) {
    switch (entry.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return entry.normalized_coords ? Shader::TextureType::Color2D
                                       : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        LOG_ERROR(Shader, "Invalid texture_type={}. Falling back to texture_type={}", static_cast<int>(entry.texture_type.Value()), Shader::TextureType::Color2D);
        return Shader::TextureType::Color2D;
    }
}

static Shader::TexturePixelFormat ConvertTexturePixelFormat(const Tegra::Texture::TICEntry& entry) {
    return static_cast<Shader::TexturePixelFormat>(
        PixelFormatFromTextureInfo(entry.format, entry.r_type, entry.g_type, entry.b_type,
                                   entry.a_type, entry.srgb_conversion));
}

static std::string_view StageToPrefix(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB:
        return "vs";
    case Shader::Stage::TessellationControl:
        return "tc";
    case Shader::Stage::TessellationEval:
        return "te";
    case Shader::Stage::Geometry:
        return "gs";
    case Shader::Stage::Fragment:
        return "fs";
    case Shader::Stage::Compute:
        return "cs";
    case Shader::Stage::VertexA:
        return "va";
    default:
        return "uk";
    }
}

static void DumpImpl(u64 /*pipeline_hash*/, u64 shader_hash, std::span<const u64> code,
                     [[maybe_unused]] u32 read_highest, [[maybe_unused]] u32 read_lowest,
                     u32 initial_offset, Shader::Stage stage) {
    const auto dump_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir)};
    if (!Common::FS::CreateDir(dump_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create dump directory");
        return;
    }
    const auto prefix = StageToPrefix(stage);
    const auto name{dump_dir /
                    fmt::format("{:016x}_{:016x}_{}.ash",
                                Settings::GetCurrentProgramID(), shader_hash, prefix)};
    std::fstream shader_file(name, std::ios::out | std::ios::binary);
    ASSERT(initial_offset % sizeof(u64) == 0);
    const size_t jump_index = initial_offset / sizeof(u64);
    const size_t code_size = code.size_bytes() - initial_offset;
    shader_file.write(reinterpret_cast<const char*>(&code[jump_index]), code_size);

    // + 1 instruction, due to the fact that we skip the final self branch instruction in the code,
    // but we need to consider it for padding, otherwise nvdisasm rages.
    const size_t padding_needed = (32 - ((code_size + INST_SIZE) % 32)) % 32;
    for (size_t i = 0; i < INST_SIZE + padding_needed; i++) {
        shader_file.put(0);
    }
}

GenericEnvironment::GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : gpu_memory{&gpu_memory_}, program_base{program_base_} {
    start_address = start_address_;
}

GenericEnvironment::~GenericEnvironment() = default;

u32 GenericEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 GenericEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 GenericEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> GenericEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

u64 GenericEnvironment::ReadInstruction(u32 address) {
    read_lowest = (std::min)(read_lowest, address);
    read_highest = (std::max)(read_highest, address);

    if (address >= cached_lowest && address < cached_highest) {
        return code[(address - cached_lowest) / INST_SIZE];
    }
    has_unbound_instructions = true;
    return gpu_memory->Read<u64>(program_base + address);
}

std::optional<u64> GenericEnvironment::Analyze() {
    const std::optional<u64> size{TryFindSize()};
    if (!size) {
        return std::nullopt;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(*size);
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), *size);
}

void GenericEnvironment::SetCachedSize(size_t size_bytes) {
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    code.resize(CachedSizeWords());
    gpu_memory->ReadBlock(program_base + cached_lowest, code.data(), code.size() * sizeof(u64));
}

size_t GenericEnvironment::CachedSizeWords() const noexcept {
    return CachedSizeBytes() / INST_SIZE;
}

size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;
}

size_t GenericEnvironment::ReadSizeBytes() const noexcept {
    return read_highest - read_lowest + INST_SIZE;
}

bool GenericEnvironment::CanBeSerialized() const noexcept {
    return !has_unbound_instructions;
}

u64 GenericEnvironment::CalculateHash() const {
    const size_t size{ReadSizeBytes()};
    const auto data{std::make_unique<char[]>(size)};
    gpu_memory->ReadBlock(program_base + read_lowest, data.get(), size);
    return Common::CityHash64(data.get(), size);
}

#ifdef NXBOX_UWP
std::vector<u64> GenericEnvironment::GuestCode() const {
    if (!has_unbound_instructions) {
        const auto words = (std::min)(code.size(), CachedSizeWords());
        return {code.begin(), code.begin() + words};
    }
    // CFG translation can read beyond the initial linear scan. Capture that range too.
    const auto begin = GuestCodeAddress();
    const auto end = (std::max)(cached_highest, read_highest);
    std::vector<u64> snapshot((static_cast<std::size_t>(end) - begin) / INST_SIZE + 1);
    gpu_memory->ReadBlock(program_base + begin, snapshot.data(), snapshot.size() * INST_SIZE);
    return snapshot;
}
#endif

void GenericEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

void GenericEnvironment::Serialize(std::ofstream& file) const {
    const u64 code_size{static_cast<u64>(CachedSizeBytes())};
    const u64 num_texture_types{static_cast<u64>(texture_types.size())};
    const u64 num_texture_pixel_formats{static_cast<u64>(texture_pixel_formats.size())};
    const u64 num_cbuf_values{static_cast<u64>(cbuf_values.size())};
    const u64 num_cbuf_replacement_values{static_cast<u64>(cbuf_replacements.size())};

    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size))
        .write(reinterpret_cast<const char*>(&num_texture_types), sizeof(num_texture_types))
        .write(reinterpret_cast<const char*>(&num_texture_pixel_formats),
               sizeof(num_texture_pixel_formats))
        .write(reinterpret_cast<const char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .write(reinterpret_cast<const char*>(&num_cbuf_replacement_values),
               sizeof(num_cbuf_replacement_values))
        .write(reinterpret_cast<const char*>(&local_memory_size), sizeof(local_memory_size))
        .write(reinterpret_cast<const char*>(&texture_bound), sizeof(texture_bound))
        .write(reinterpret_cast<const char*>(&start_address), sizeof(start_address))
        .write(reinterpret_cast<const char*>(&cached_lowest), sizeof(cached_lowest))
        .write(reinterpret_cast<const char*>(&cached_highest), sizeof(cached_highest))
        .write(reinterpret_cast<const char*>(&viewport_transform_state),
               sizeof(viewport_transform_state))
        .write(reinterpret_cast<const char*>(&stage), sizeof(stage))
        .write(reinterpret_cast<const char*>(code.data()), code_size);
    for (const auto& [key, type] : texture_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, format] : texture_pixel_formats) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&format), sizeof(format));
    }
    for (const auto& [key, type] : cbuf_values) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : cbuf_replacements) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    if (stage == Shader::Stage::Compute) {
        file.write(reinterpret_cast<const char*>(&workgroup_size), sizeof(workgroup_size))
            .write(reinterpret_cast<const char*>(&shared_memory_size), sizeof(shared_memory_size));
    } else {
        file.write(reinterpret_cast<const char*>(&sph), sizeof(sph));
        if (stage == Shader::Stage::Geometry) {
            file.write(reinterpret_cast<const char*>(&gp_passthrough_mask),
                       sizeof(gp_passthrough_mask));
        }
    }
}

std::optional<u64> GenericEnvironment::TryFindSize() {
    static constexpr size_t BLOCK_SIZE = 0x1000;
    static constexpr size_t MAXIMUM_SIZE = 0x100000;

    static constexpr u64 SELF_BRANCH_A = 0xE2400FFFFF87000FULL;
    static constexpr u64 SELF_BRANCH_B = 0xE2400FFFFF07000FULL;

    static constexpr u64 EXIT_VALUE = 0xE30000000007000FULL;

    code.resize(MAXIMUM_SIZE / INST_SIZE);

    GPUVAddr guest_addr{program_base + start_address};
    size_t offset{0};
    size_t size{BLOCK_SIZE};
    while (size <= MAXIMUM_SIZE) {
        u64* const data = code.data() + offset / INST_SIZE;
        gpu_memory->ReadBlock(guest_addr, data, BLOCK_SIZE);
        for (size_t index = 0; index < BLOCK_SIZE; index += INST_SIZE) {
            const u64 inst = data[index / INST_SIZE];
            if (inst == SELF_BRANCH_A || inst == SELF_BRANCH_B) {
                return offset + index;
            }
            if (!is_proprietary_driver && inst == EXIT_VALUE) {
                return offset + index + INST_SIZE;
            }
        }
        guest_addr += BLOCK_SIZE;
        size += BLOCK_SIZE;
        offset += BLOCK_SIZE;
    }
    return std::nullopt;
}

Tegra::Texture::TICEntry GenericEnvironment::ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                                             bool via_header_index, u32 raw) {
    const auto handle{Tegra::Texture::TexturePair(raw, via_header_index)};
    ASSERT(handle.first <= tic_limit);
    const GPUVAddr descriptor_addr{tic_addr + handle.first * sizeof(Tegra::Texture::TICEntry)};
    Tegra::Texture::TICEntry entry;
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

GraphicsEnvironment::GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                         Tegra::MemoryManager& gpu_memory_,
                                         Maxwell::ShaderType program, GPUVAddr program_base_,
                                         u32 start_address_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, maxwell3d{&maxwell3d_} {
    gpu_memory->ReadBlock(program_base + start_address, &sph, sizeof(sph));
    initial_offset = sizeof(sph);
    gp_passthrough_mask = maxwell3d->regs.post_vtg_shader_attrib_skip_mask;
    switch (program) {
    case Maxwell::ShaderType::VertexA:
        stage = Shader::Stage::VertexA;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::VertexB:
        stage = Shader::Stage::VertexB;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::TessellationInit:
        stage = Shader::Stage::TessellationControl;
        stage_index = 1;
        break;
    case Maxwell::ShaderType::Tessellation:
        stage = Shader::Stage::TessellationEval;
        stage_index = 2;
        break;
    case Maxwell::ShaderType::Geometry:
        stage = Shader::Stage::Geometry;
        stage_index = 3;
        break;
    case Maxwell::ShaderType::Pixel:
        stage = Shader::Stage::Fragment;
        stage_index = 4;
        break;
    default:
        ASSERT_MSG(false, "Invalid program={}", program);
        break;
    }
    const u64 local_size{sph.LocalMemorySize()};
    ASSERT(local_size <= (std::numeric_limits<u32>::max)());
    local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    texture_bound = maxwell3d->regs.bindless_texture_const_buffer_slot;
    is_proprietary_driver = texture_bound == 2;
    has_hle_engine_state =
        maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
}

u32 GraphicsEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
    ASSERT(cbuf.enabled);
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.address + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

std::optional<Shader::ReplaceConstant> GraphicsEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    if (!has_hle_engine_state) {
        return std::nullopt;
    }
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = maxwell3d->replace_table.find(key);
    if (it == maxwell3d->replace_table.end()) {
        return std::nullopt;
    }
    const auto converted_value = [](Tegra::Engines::Maxwell3D::HLEReplacementAttributeType name) {
        switch (name) {
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseVertex:
            return Shader::ReplaceConstant::BaseVertex;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseInstance:
            return Shader::ReplaceConstant::BaseInstance;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::DrawID:
            return Shader::ReplaceConstant::DrawID;
        default:
            UNREACHABLE();
        }
    }(it->second);
    cbuf_replacements.emplace(key, converted_value);
    return converted_value;
}

Shader::TextureType GraphicsEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{maxwell3d->regs};
    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};
    auto entry =
        ReadTextureInfo(regs.tex_header.Address(), regs.tex_header.limit, via_header_index, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat GraphicsEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{maxwell3d->regs};
    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};
    auto entry =
        ReadTextureInfo(regs.tex_header.Address(), regs.tex_header.limit, via_header_index, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool GraphicsEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 GraphicsEnvironment::ReadViewportTransformState() {
    const auto& regs{maxwell3d->regs};
    viewport_transform_state = regs.viewport_scale_offset_enabled;
    return viewport_transform_state;
}

ComputeEnvironment::ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                       Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, kepler_compute{
                                                                          &kepler_compute_} {
    const auto& qmd{kepler_compute->launch_description};
    stage = Shader::Stage::Compute;
    local_memory_size = qmd.local_pos_alloc + qmd.local_crs_alloc;
    texture_bound = kepler_compute->regs.tex_cb_index;
    is_proprietary_driver = texture_bound == 2;
    shared_memory_size = qmd.shared_alloc;
    workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z};
}

u32 ComputeEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& qmd{kepler_compute->launch_description};
    ASSERT(((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0);
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.Address() + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

Shader::TextureType ComputeEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat ComputeEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool ComputeEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 ComputeEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

// Immutable compilation inputs: no guest memory, engine or command-list pointers survive.
class FrozenEnvironment final : public Shader::Environment {
public:
    explicit FrozenEnvironment(GenericEnvironment& source) {
        sph = source.SPH();
        gp_passthrough_mask = source.GpPassthroughMask();
        stage = source.ShaderStage();
        start_address = source.StartAddress();
        is_proprietary_driver = source.IsProprietaryDriver();
        local = source.LocalMemorySize();
        shared = source.SharedMemorySize();
        bound = source.TextureBoundBuffer();
        workgroup = source.WorkgroupSize();
        viewport = source.ReadViewportTransformState();
        hle = source.HasHLEMacroState();
        code.resize(source.CachedSizeWords());
        for (size_t i = 0; i < code.size(); ++i)
            code[i] = source.ReadInstruction(start_address + static_cast<u32>(i * sizeof(u64)));
    }
    u64 ReadInstruction(u32 address) override {
        if (address < start_address || (address - start_address) % sizeof(u64))
            throw std::runtime_error(fmt::format(
                "Unaligned/out-of-range frozen shader instruction {:#x} (code starts at "
                "{:#x}, stage {})",
                address, start_address, StageToPrefix(stage)));
        const size_t word = (address - start_address) / sizeof(u64);
        // The live environment falls back to guest memory beyond its cached range, which a
        // snapshot cannot do. Say so instead of surfacing std::vector::at's bare message.
        if (word >= code.size())
            throw std::out_of_range(fmt::format(
                "Frozen shader code window exhausted: instruction {:#x} is word {} of a {}-word "
                "snapshot taken from {:#x} (stage {}, bindless cbuf slot {}, proprietary driver "
                "{})",
                address, word, code.size(), start_address, StageToPrefix(stage), bound,
                is_proprietary_driver));
        return code[word];
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        const auto& bytes = cbufs.at(bank);
        u32 value{};
        if (offset < bytes.size()) {
            if (bytes.size() - offset < sizeof(value))
                throw std::runtime_error("Truncated frozen constant buffer word");
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        }
        return value;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        const auto* entry = Descriptor(handle);
        // Same fallback ConvertTextureType takes for a descriptor it cannot classify.
        return entry ? ConvertTextureType(*entry) : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        const auto* entry = Descriptor(handle);
        // What the format lookup returns for an all-zero descriptor.
        return entry ? ConvertTexturePixelFormat(*entry)
                     : static_cast<Shader::TexturePixelFormat>(
                           VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM);
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return VideoCore::Surface::IsPixelFormatInteger(
            static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
    }
    u32 ReadViewportTransformState() override {
        return viewport;
    }
    u32 TextureBoundBuffer() const override {
        return bound;
    }
    u32 LocalMemorySize() const override {
        return local;
    }
    u32 SharedMemorySize() const override {
        return shared;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return workgroup;
    }
    bool HasHLEMacroState() const override {
        return hle;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        const auto it = replacements.find(MakeCbufKey(bank, offset));
        return it == replacements.end() ? std::nullopt : std::optional{it->second};
    }
    void Dump(u64, u64) override {}
    void CopyTextures(Tegra::MemoryManager& memory, GPUVAddr address, u32 limit, bool linked_) {
        // Hardware TIC indices are 20 bits. Reject corrupt limits before allocation.
        if (limit >= (1U << 20))
            throw std::runtime_error("Frozen texture descriptor limit exceeds hardware range");
        linked = linked_;
        textures =
            std::make_shared<std::vector<Tegra::Texture::TICEntry>>(static_cast<size_t>(limit) + 1);
        memory.ReadBlock(address, textures->data(),
                         textures->size() * sizeof(Tegra::Texture::TICEntry));
    }
    void CopyCbuf(Tegra::MemoryManager& memory, u32 bank, GPUVAddr address, u32 size) {
        if (size > 65536)
            throw std::runtime_error("Frozen constant buffer exceeds hardware range");
        auto& bytes = cbufs.at(bank);
        bytes.resize(size);
        if (size)
            memory.ReadBlock(address, bytes.data(), size);
    }
    std::unordered_map<u64, Shader::ReplaceConstant> replacements;

private:
    // A handle beyond the pool names no descriptor. The live environment only soft-asserts and
    // decodes whatever guest memory lies there; throwing here instead failed the whole pipeline
    // for a lookup the live path survives. Report it (a few times per run) and treat the handle
    // as an invalid descriptor, as the hardware does.
    const Tegra::Texture::TICEntry* Descriptor(u32 handle) {
        const u32 index = Tegra::Texture::TexturePair(handle, linked).first;
        if (textures && index < textures->size())
            return &(*textures)[index];
        static std::atomic<u32> reports{};
        if (!warned_pool_overflow && reports.fetch_add(1) < 16) {
            LOG_WARNING(Shader,
                        "Frozen {} shader reads texture handle {:#x} (descriptor {}) beyond a pool "
                        "of {} descriptors; treating it as invalid",
                        StageToPrefix(stage), handle, index, textures ? textures->size() : 0);
        }
        warned_pool_overflow = true;
        return nullptr;
    }
    std::vector<u64> code;
    std::array<std::vector<u8>, 18> cbufs;
    bool warned_pool_overflow{};

public:
    std::shared_ptr<std::vector<Tegra::Texture::TICEntry>> textures;
    bool linked{};

private:
    std::array<u32, 3> workgroup{};
    u32 local{}, shared{}, bound{}, viewport{};
    bool hle{};
};

std::unique_ptr<Shader::Environment> GraphicsEnvironment::Snapshot(
    Shader::Environment* texture_source) {
    auto result = std::make_unique<FrozenEnvironment>(*this);
    const auto& buffers = maxwell3d->state.shader_stages[stage_index].const_buffers;
    for (u32 bank = 0; bank < buffers.size(); ++bank) {
        const auto& buffer = buffers[bank];
        if (buffer.enabled)
            result->CopyCbuf(*gpu_memory, bank, buffer.address, buffer.size);
    }
    const auto& regs = maxwell3d->regs;
    if (texture_source) {
        // Only Snapshot() results are accepted here; the caller owns their concrete type.
        const auto& frozen = static_cast<const FrozenEnvironment&>(*texture_source);
        result->textures = frozen.textures;
        result->linked = frozen.linked;
    } else {
        result->CopyTextures(*gpu_memory, regs.tex_header.Address(), regs.tex_header.limit,
                             regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding);
    }
    if (has_hle_engine_state) {
        for (const auto& [key, attribute] : maxwell3d->replace_table) {
            (void)attribute;
            const auto replacement =
                GetReplaceConstBuffer(static_cast<u32>(key >> 32), static_cast<u32>(key));
            if (replacement)
                result->replacements.emplace(key, *replacement);
        }
    }
    return result;
}

std::unique_ptr<Shader::Environment> ComputeEnvironment::Snapshot() {
    auto result = std::make_unique<FrozenEnvironment>(*this);
    const auto& launch = kepler_compute->launch_description;
    for (u32 bank = 0; bank < launch.const_buffer_config.size(); ++bank) {
        if ((launch.const_buffer_enable_mask.Value() >> bank) & 1) {
            const auto& buffer = launch.const_buffer_config[bank];
            result->CopyCbuf(*gpu_memory, bank, buffer.Address(), buffer.size);
        }
    }
    const auto& regs = kepler_compute->regs;
    result->CopyTextures(*gpu_memory, regs.tic.Address(), regs.tic.limit, launch.linked_tsc != 0);
    return result;
}

void FileEnvironment::Deserialize(std::ifstream& file) {
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_cbuf_values{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    for (size_t i = 0; i < num_texture_types; ++i) {
        u32 key;
        Shader::TextureType type;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&type), sizeof(type));
        texture_types.emplace(key, type);
    }
    for (size_t i = 0; i < num_texture_pixel_formats; ++i) {
        u32 key;
        Shader::TexturePixelFormat format;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&format), sizeof(format));
        texture_pixel_formats.emplace(key, format);
    }
    for (size_t i = 0; i < num_cbuf_values; ++i) {
        u64 key;
        u32 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_values.emplace(key, value);
    }
    for (size_t i = 0; i < num_cbuf_replacement_values; ++i) {
        u64 key;
        Shader::ReplaceConstant value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_replacements.emplace(key, value);
    }
    if (stage == Shader::Stage::Compute) {
        file.read(reinterpret_cast<char*>(&workgroup_size), sizeof(workgroup_size))
            .read(reinterpret_cast<char*>(&shared_memory_size), sizeof(shared_memory_size));
        initial_offset = 0;
    } else {
        file.read(reinterpret_cast<char*>(&sph), sizeof(sph));
        initial_offset = sizeof(sph);
        if (stage == Shader::Stage::Geometry) {
            file.read(reinterpret_cast<char*>(&gp_passthrough_mask), sizeof(gp_passthrough_mask));
        }
    }
    is_proprietary_driver = texture_bound == 2;
}

void FileEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

u64 FileEnvironment::ReadInstruction(u32 address) {
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}

u32 FileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto it{cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset))};
    if (it == cbuf_values.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TextureType FileEnvironment::ReadTextureType(u32 handle) {
    const auto it{texture_types.find(handle)};
    if (it == texture_types.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TexturePixelFormat FileEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto it{texture_pixel_formats.find(handle)};
    if (it == texture_pixel_formats.end()) {
        throw Shader::LogicError("Uncached read texture pixel format");
    }
    return it->second;
}

bool FileEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 FileEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

u32 FileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 FileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

u32 FileEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

std::array<u32, 3> FileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

std::optional<Shader::ReplaceConstant> FileEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                              u32 offset) {
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = cbuf_replacements.find(key);
    if (it == cbuf_replacements.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version) try {
    std::ofstream file(filename, std::ios::binary | std::ios::ate | std::ios::app);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    if (file.tellp() == 0) {
        // Write header
        file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
    }
    if (!std::ranges::all_of(envs, &GenericEnvironment::CanBeSerialized)) {
        return;
    }
    const u32 num_envs{static_cast<u32>(envs.size())};
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    for (const GenericEnvironment* const env : envs) {
        env->Serialize(file);
    }
    file.write(key.data(), key.size_bytes());

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics) try {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(0, std::ios::beg);

    std::array<char, 8> magic_number;
    u32 cache_version;
    file.read(magic_number.data(), magic_number.size())
        .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
    if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
        file.close();
        if (Common::FS::RemoveFile(filename)) {
            if (magic_number != MAGIC_NUMBER) {
                LOG_ERROR(Common_Filesystem, "Invalid pipeline cache file");
            }
            if (cache_version != expected_cache_version) {
                LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
            }
        } else {
            LOG_ERROR(Common_Filesystem,
                      "Invalid pipeline cache file and failed to delete it in \"{}\"",
                      Common::FS::PathToUTF8String(filename));
        }
        return;
    }
    while (file.tellg() != end) {
        if (stop_loading.stop_requested()) {
            return;
        }
        u32 num_envs{};
        file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        std::vector<FileEnvironment> envs(num_envs);
        for (FileEnvironment& env : envs) {
            env.Deserialize(file);
        }
        if (envs.front().ShaderStage() == Shader::Stage::Compute) {
            load_compute(file, std::move(envs.front()));
        } else {
            load_graphics(file, std::move(envs));
        }
    }

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

} // namespace VideoCommon
