// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// A failed ScratchBuffer allocation must not leave a capacity without memory behind it. On the
// console the app sits at its memory limit, the renderer boundary swallows the bad_alloc, and the
// next blit of the same size used to write through the null buffer on the GPU thread.

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include "common/scratch_buffer.h"

int main() {
    unsigned checks = 0;
    const auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) {
            std::cerr << "FAIL: " << name << '\n';
            std::exit(1);
        }
    };
    // Volatile keeps the compiler from folding the impossible request into a diagnostic.
    volatile std::size_t impossible_request = std::numeric_limits<std::size_t>::max();
    const std::size_t impossible = impossible_request;
    const auto try_resize = [&](Common::ScratchBuffer<unsigned char>& buffer) {
        try {
            buffer.resize_destructive(impossible);
        } catch (const std::bad_alloc&) {
            return true;
        }
        return false;
    };

    Common::ScratchBuffer<unsigned char> empty;
    // The retry is the case that crashed: the second call must fail again, not return a null span.
    for (int attempt = 0; attempt < 2; ++attempt) {
        check(try_resize(empty), "impossible allocation throws on every attempt");
        check(empty.data() == nullptr && empty.capacity() == 0 && empty.size() == 0,
              "failed first allocation leaves the buffer empty");
    }

    Common::ScratchBuffer<unsigned char> grown;
    grown.resize_destructive(16);
    unsigned char* const original = grown.data();
    check(original != nullptr && grown.capacity() == 16 && grown.size() == 16, "normal growth");
    check(try_resize(grown), "failed growth throws");
    check(grown.data() == original && grown.capacity() == 16 && grown.size() == 16,
          "failed growth keeps the previous buffer");
    grown.resize_destructive(8);
    check(grown.data() == original && grown.size() == 8, "shrink reuses the buffer");

    std::cout << checks << " scratch-buffer checks passed\n";
}
