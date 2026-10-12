// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
namespace VideoCommon {
template class BufferCache<D3D12::BufferCacheParams>;
}
