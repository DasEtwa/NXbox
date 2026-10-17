// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_base.h"

// Requests run on the compositor thread after each presented frame. Screenshot
// callbacks own their buffers and completion state independently of SDL teardown.
class FrameCapture : public std::enable_shared_from_this<FrameCapture> {
public:
    explicit FrameCapture(const char* frame_list) {
        if (!frame_list || !*frame_list) {
            return;
        }
        std::string_view remaining{frame_list};
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            auto token = remaining.substr(0, comma);
            const auto first = token.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                throw std::runtime_error("Empty NXBOX_CAPTURE_FRAMES entry");
            }
            token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
            u64 frame{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), frame);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
                frame == 0) {
                throw std::runtime_error(
                    "NXBOX_CAPTURE_FRAMES must contain positive frame numbers");
            }
            frames.push_back(frame);
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
            if (remaining.empty()) {
                throw std::runtime_error("Empty NXBOX_CAPTURE_FRAMES entry");
            }
        }
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        const char* directory = std::getenv("NXBOX_CAPTURE_DIR");
        output_dir = directory && *directory ? std::filesystem::path{directory}
                                             : std::filesystem::current_path();
        std::filesystem::create_directories(output_dir);
        const char* exit_setting = std::getenv("NXBOX_CAPTURE_EXIT");
        exit_after_capture = exit_setting && std::string_view{exit_setting} == "1";
    }

    void RequestFrame(VideoCore::RendererBase& renderer, u64 frame) {
        if (!std::binary_search(frames.begin(), frames.end(), frame)) {
            return;
        }
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        auto pixels = std::make_shared<std::vector<u8>>(static_cast<size_t>(layout.width) *
                                                        layout.height * 4);
        renderer.RequestScreenshot(
            pixels->data(),
            [self = shared_from_this(), pixels, frame, layout](bool invert_y) {
                try {
                    const auto path = self->output_dir / fmt::format("frame_{:06}.ppm", frame);
                    std::ofstream output{path, std::ios::binary};
                    output.exceptions(std::ios::badbit | std::ios::failbit);
                    output << "P6\n" << layout.width << ' ' << layout.height << "\n255\n";
                    std::vector<char> row(static_cast<size_t>(layout.width) * 3);
                    for (u32 y = 0; y < layout.height; ++y) {
                        const auto source_y = invert_y ? layout.height - 1 - y : y;
                        const auto* source =
                            pixels->data() + static_cast<size_t>(source_y) * layout.width * 4;
                        for (u32 x = 0; x < layout.width; ++x) {
                            row[x * 3] = static_cast<char>(source[x * 4 + 2]);
                            row[x * 3 + 1] = static_cast<char>(source[x * 4 + 1]);
                            row[x * 3 + 2] = static_cast<char>(source[x * 4]);
                        }
                        output.write(row.data(), static_cast<std::streamsize>(row.size()));
                    }
                    output.close();
                    LOG_INFO(Frontend, "Captured frame {} to {}", frame, path.string());
                    self->written.fetch_add(1, std::memory_order_release);
                } catch (const std::exception& error) {
                    LOG_ERROR(Frontend, "Failed to write frame {}: {}", frame, error.what());
                }
            },
            layout);
    }

    void OnFrameDisplayed(VideoCore::RendererBase& renderer) {
        RequestFrame(renderer, ++presented_frames + 1);
    }

    bool ShouldExit() const {
        return exit_after_capture && !frames.empty() &&
               written.load(std::memory_order_acquire) == frames.size();
    }

private:
    std::vector<u64> frames;
    std::filesystem::path output_dir;
    std::atomic<size_t> written{};
    u64 presented_frames{};
    bool exit_after_capture{};
};
