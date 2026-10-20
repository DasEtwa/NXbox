#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
using u8 = unsigned char;
using u32 = std::uint32_t;
using GPUVAddr = std::uint64_t;
#define ASSERT assert
namespace VideoCommon::SubmissionTrace {
template <class... A>
void Log(const char*, A...) {}
} // namespace VideoCommon::SubmissionTrace
struct Buffer : std::vector<u8> {
    void resize_destructive(size_t n) {
        resize(n);
    }
};
struct Memory {
    std::vector<u8> guest = std::vector<u8>(32, 0);
    std::vector<u8> cached = guest;
    bool dirty = false;
    unsigned safe_writes = 0, unsafe_writes = 0;
    void WriteBlockUnsafe(GPUVAddr address, const void* bytes, size_t size) {
        assert(address == 4096);
        ++unsafe_writes;
        std::memcpy(guest.data(), bytes, size);
    }
    void WriteBlock(GPUVAddr address, const void* bytes, size_t size) {
        assert(address == 4096);
        ++safe_writes;
        dirty = true;
        std::memcpy(guest.data(), bytes, size);
    }
    void PrepareImage() {
        if (dirty) {
            cached = guest;
            dirty = false;
        }
    }
};
struct Acceleration {
    bool enabled = false;
    Memory& memory;
    bool BufferClear(GPUVAddr, unsigned elements, u32 value) {
        if (enabled) {
            for (unsigned i = 0; i < elements; ++i)
                std::memcpy(memory.cached.data() + 4 * i, &value, 4);
        }
        return enabled;
    }
};
struct Engine {
    struct Regs {
        GPUVAddr offset_out = 4096;
        unsigned line_length_in = 8;
        struct {
            unsigned component_size_minus_one = 3;
            u32 remap_consta_value = 0x591642c8;
        } remap_const;
    } regs;
    Memory memory_manager;
    Acceleration accelerate{false, memory_manager};
    Buffer read_buffer;
    void Fill() {
        // PRODUCTION_BODY
    }
};
int main() {
    Engine software;
    software.Fill();
    assert(software.memory_manager.safe_writes == 1);
    assert(software.memory_manager.unsafe_writes == 0);
    software.memory_manager.PrepareImage();
    assert(software.memory_manager.cached == software.memory_manager.guest);
    assert(software.memory_manager.cached[0] == 0xc8);
    Engine accelerated;
    accelerated.accelerate.enabled = true;
    accelerated.Fill();
    assert(accelerated.memory_manager.safe_writes == 0);
    assert(accelerated.memory_manager.unsafe_writes == 1);
    assert(accelerated.memory_manager.cached == accelerated.memory_manager.guest);
    Engine narrow;
    narrow.accelerate.enabled = true;
    narrow.regs.remap_const.component_size_minus_one = 1;
    narrow.Fill();
    assert(narrow.memory_manager.safe_writes == 1);
    assert(narrow.memory_manager.unsafe_writes == 0);
}
