// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/texture_cache/texture_cache.h"
namespace VideoCommon {
template class TextureCache<D3D12::TextureCacheParams>;
}
