void ReferencePusher::ProcessCommands(std::span<const CommandHeader> commands) {
    std::vector<CommandHeader> pipeline_commands;
    if (rasterizer && rasterizer->HasGpuPipeline()) {
        rasterizer->DrainGpuPipeline();
        commands = VideoCommon::SnapshotGpuMethods(commands, pipeline_commands);
    }
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; // must ref (MUltiMethod re)
            dma_state.dma_word_offset = u32(index * sizeof(u32));
            const u32 max_write =
                u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            constexpr u32 cb_begin = static_cast<u32>(
                offsetof(Engines::Maxwell3D::Regs, const_buffer.buffer) / sizeof(u32));
            constexpr u32 cb_end = cb_begin + 16;
            if (!dma_increment_once && dma_state.method >= cb_begin && dma_state.method < cb_end &&
                subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D) {
                auto* engine = static_cast<Engines::Maxwell3D*>(subchannels[dma_state.subchannel]);
                const auto amount = static_cast<u32>(
                    (std::min)(commands.size() - index,
                               static_cast<size_t>(
                                   (std::min)(dma_state.method_count, cb_end - dma_state.method))));
                dma_state.dma_word_offset = static_cast<u32>(index * sizeof(u32));
                engine->ConsumeSink(system);
                engine->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
                engine->CallCBDataIncrementing(dma_state.method, &commands[index].argument, amount);
                dma_state.method += amount;
                dma_state.method_count -= amount;
                dma_state.is_last_call = dma_state.method_count == 0;
                index += amount;
                continue;
            }
            if (frontend_fastpath && !dma_increment_once &&
                subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D &&
                dma_state.method >= non_puller_methods && dma_state.method < MacroRegistersStart) {
                auto* engine = static_cast<Engines::Maxwell3D*>(subchannels[dma_state.subchannel]);
                const auto limit = static_cast<u32>(
                    (std::min)(commands.size() - index,
                               static_cast<size_t>(
                                   (std::min)(dma_state.method_count,
                                              MacroRegistersStart - dma_state.method))));
                u32 amount = 0;
                while (amount < limit && !engine->execution_mask[dma_state.method + amount])
                    ++amount;
                if (amount != 0) {
                    engine->CallMultiMethod(system, dma_state.method, &commands[index].argument,
                                            amount);
                    dma_state.dma_word_offset =
                        static_cast<u32>((index + amount - 1) * sizeof(u32));
                    dma_state.method += amount;
                    dma_state.method_count -= amount;
                    dma_state.is_last_call = dma_state.method_count == 0;
                    index += amount;
                    continue;
                }
            }
            auto const command_header = commands[index]; // can copy
            dma_state.dma_word_offset = u32(index * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; // can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset =
                    u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
    }
}
