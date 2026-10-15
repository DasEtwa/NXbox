// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_transform_feedback.h"

#include <cstring>
#include <stdexcept>

namespace D3D12 {
TransformFeedback::TransformFeedback(Context& context)
    : context{context}, upload{context, D3D12_HEAP_TYPE_UPLOAD},
      readback{context, D3D12_HEAP_TYPE_READBACK} {}
void TransformFeedback::EnsureCounters() {
    if (counters)
        return;
    counters = context.CreateBuffer(32, D3D12_HEAP_TYPE_DEFAULT);
    context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    context.Profile().Count(FrameCount::BufferUploadBytes, 32);
    context.Profile().Count(FrameCount::StorageUploadBytes, 32);
    context.Profile().Count(FrameCount::StagingCopyBytes, 32);
    auto initial = upload.Request(32);
    std::memset(initial.mapped_span.data(), 0, 32);
    context.Transition(counters, D3D12_RESOURCE_STATE_COPY_DEST);
    context.Retain(initial.buffer);
    context.Retain(counters);
    context.MarkWork("stream-output counter initialization");
    context.BeforeCopyWrite(counters, 0, 32);
    context.Commands()->CopyBufferRegion(counters->handle.Get(), 0, initial.buffer->handle.Get(),
                                         initial.offset, 32);
}
void TransformFeedback::Configure(const Shader::RuntimeInfo& info, SemanticResolver resolver) {
    if (info.xfb_count > info.xfb_varyings.size()) {
        throw std::invalid_argument("Invalid transform feedback varying count");
    }
    std::vector<StreamOutputVarying> varyings;
    for (u32 attribute = 0; attribute < info.xfb_count; ++attribute) {
        const auto& v = info.xfb_varyings[attribute];
        if (v.components) {
            varyings.push_back({attribute, v.buffer, v.stream, v.stride, v.offset, v.components});
        }
    }
    auto new_layout = MakeStreamOutputLayout(varyings);
    std::deque<std::string> new_semantics;
    std::vector<D3D12_SO_DECLARATION_ENTRY> new_entries;
    for (const auto& e : new_layout.elements) {
        const char* name = nullptr;
        u32 index{};
        if (e.attribute != StreamOutputElement::Gap) {
            Semantic semantic;
            if (resolver) {
                semantic = resolver(e.attribute);
            } else if (e.attribute / 4 == 7) {
                semantic = {"NX_XFB_POSITION", 0};
            } else if (e.attribute >= 32 && e.attribute < 160) {
                semantic = {"TEXCOORD", (e.attribute - 32) / 4};
            } else {
                throw std::invalid_argument("Unsupported stream output semantic");
            }
            if (semantic.name.empty()) {
                throw std::invalid_argument("Missing stream output semantic");
            }
            index = semantic.index;
            new_semantics.push_back(std::move(semantic.name));
            name = new_semantics.back().c_str();
        }
        // D3D12 gaps use a null semantic, at most 4 components per entry.
        u32 remaining = e.components;
        do {
            const auto components = std::min(remaining, 4U);
            new_entries.push_back({e.stream, name, index, static_cast<BYTE>(e.start_component),
                                   static_cast<BYTE>(components), static_cast<BYTE>(e.buffer)});
            remaining -= components;
        } while (remaining);
    }
    layout = std::move(new_layout);
    semantics.swap(new_semantics);
    entries = std::move(new_entries);
}
D3D12_STREAM_OUTPUT_DESC TransformFeedback::Declaration(bool rasterize, u32 stream) const {
    if (stream >= 4) {
        throw std::out_of_range("Invalid rasterized stream");
    }
    u32 count{};
    for (u32 i = 0; i < 4; ++i) {
        if (layout.strides[i]) {
            count = i + 1;
        }
    }
    return {entries.data(), static_cast<UINT>(entries.size()), layout.strides.data(), count,
            rasterize ? stream : D3D12_SO_NO_RASTERIZED_STREAM};
}
void TransformFeedback::BindBuffer(u32 slot, ResourcePtr buffer, u64 offset, u64 size,
                                   std::optional<u64> filled_size) {
    if (slot >= 4) {
        throw std::out_of_range("Invalid stream output slot");
    }
    if (!buffer || !size) {
        buffers[slot].reset();
        views[slot] = {};
        return;
    }
    const auto desc = buffer->handle->GetDesc();
    if (buffer == counters || buffer->heap != D3D12_HEAP_TYPE_DEFAULT ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || (offset | size) % 4 ||
        offset > desc.Width || size > desc.Width - offset ||
        (filled_size && (*filled_size > size || *filled_size % 4))) {
        throw std::invalid_argument("Invalid stream output buffer range");
    }
    EnsureCounters();
    if (!filled_size &&
        (reset_pending[slot] || buffers[slot] != buffer ||
         views[slot].BufferLocation != buffer->handle->GetGPUVirtualAddress() + offset))
        filled_size = 0;
    if (filled_size) {
        context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        context.Profile().Count(FrameCount::BufferUploadBytes, 8);
        context.Profile().Count(FrameCount::StorageUploadBytes, 8);
        context.Profile().Count(FrameCount::StagingCopyBytes, 8);
        auto reset = upload.Request(8);
        std::memcpy(reset.mapped_span.data(), &*filled_size, 8);
        context.Transition(counters, D3D12_RESOURCE_STATE_COPY_DEST);
        context.Retain(reset.buffer);
        context.MarkWork("upload/copy");
        context.BeforeCopyWrite(counters, u64{slot} * 8, 8);
        context.Commands()->CopyBufferRegion(counters->handle.Get(), slot * 8,
                                             reset.buffer->handle.Get(), reset.offset, 8);
    }
    reset_pending[slot] = false;
    buffers[slot] = std::move(buffer);
    views[slot] = {buffers[slot]->handle->GetGPUVirtualAddress() + offset, size,
                   counters->handle->GetGPUVirtualAddress() + slot * 8};
}
void TransformFeedback::Bind(bool cache_draw_state) {
    EnsureCounters();
    std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, 4> active_views{};
    for (u32 i = 0; i < 4; ++i) {
        if (layout.strides[i] && !buffers[i]) {
            throw std::invalid_argument("Missing stream output buffer");
        }
        if (buffers[i] && layout.strides[i]) {
            context.Retain(buffers[i]);
            context.Transition(buffers[i], D3D12_RESOURCE_STATE_STREAM_OUT);
            active_views[i] = views[i];
        }
    }
    context.Retain(counters);
    context.Transition(counters, D3D12_RESOURCE_STATE_STREAM_OUT);
    auto* commands = context.Commands();
    if (!cache_draw_state)
        context.InvalidateRecordedState();
    if (context.RecordState(
            context.DrawState().stream_output.Update(active_views.data(), active_views.size())))
        commands->SOSetTargets(0, 4, active_views.data());
    bound = true;
}
void TransformFeedback::Unbind(bool reset_cursor, bool cache_draw_state) {
    const std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, 4> empty{};
    auto* commands = context.Commands();
    if (!cache_draw_state)
        context.InvalidateRecordedState();
    if (context.RecordState(context.DrawState().stream_output.Update(empty.data(), empty.size())))
        commands->SOSetTargets(0, 4, empty.data());
    bound = false;
    // Keep the last filled-size counters readable after disabling capture.
    // Clear/blit unbinding only pauses host SO; a new guest session resets on bind.
    if (reset_cursor)
        reset_pending.fill(true);
}
u32 TransformFeedback::StreamMask() const {
    u32 mask{};
    if (bound) {
        for (u32 i = 0; i < 4; ++i) {
            if (buffers[i] && layout.strides[i]) {
                mask |= 1U << layout.streams[i];
            }
        }
    }
    return mask;
}
u64 TransformFeedback::ReadFilledSize(u32 stream) {
    if (stream >= 4) {
        throw std::out_of_range("Invalid stream output stream");
    }
    for (u32 i = 0; i < 4; ++i) {
        if (buffers[i] && layout.streams[i] == stream) {
            context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            auto result = readback.Request(8);
            context.Transition(counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.Retain(result.buffer);
            context.MarkWork("upload/copy");
            context.BeforeCopyWrite(result.buffer, result.offset, 8);
            context.Commands()->CopyBufferRegion(result.buffer->handle.Get(), result.offset,
                                                 counters->handle.Get(), i * 8, 8);
            context.Finish(FrameCount::ForcedSubmitBufferDownload);
            u64 value{};
            std::memcpy(&value, result.mapped_span.data(), 8);
            return value;
        }
    }
    return 0;
}
} // namespace D3D12
