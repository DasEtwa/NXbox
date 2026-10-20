# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production predicate recording with SDK-independent command mocks."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_query_gpu_reports import function_body

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class ConditionalRendering(unittest.TestCase):
    def test_recording_and_cpu_fallback_match_maxwell(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        body = function_body(source, "void QueryCacheRuntime::SetGpuPredicate(")
        compare = function_body(source, "bool QueryCacheRuntime::CompareConditional(")
        expression = re.search(r"const u64 predicate = (.*?);", compare, re.S).group(1)
        self.assertIn('BisectEnabled("NXBOX_D3D12_EXACT_CONDITIONAL")', compare)
        self.assertIn(
            "SetGpuPredicate(first.address, D3D12_PREDICATION_OP_EQUAL_ZERO, exact_conditional)",
            compare,
        )
        self.assertNotRegex(body, r"\.(?:Finish|Wait)\(")
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
using u32=std::uint32_t; using u64=std::uint64_t; using DAddr=u64;
using D3D12_PREDICATION_OP=int;
constexpr int D3D12_PREDICATION_OP_EQUAL_ZERO=0;
constexpr int D3D12_HEAP_TYPE_DEFAULT=0;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=1;
constexpr int D3D12_RESOURCE_STATE_UNORDERED_ACCESS=2;
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE=3;
constexpr int D3D12_RESOURCE_STATE_COPY_DEST=4;
constexpr int FALSE=0;
namespace VideoCommon {
enum class ObtainBufferSynchronize {FullSynchronize};
enum class ObtainBufferOperation {DoNothing};
}
namespace FrameCount { constexpr int Dispatches=1; }
struct Resource {
    struct Handle {
        Resource* resource;
        Resource* operator->() { return resource; }
        Resource* Get() { return resource; }
    } handle{this};
    std::array<u32,64> data{};
    int state{};
    u64 GetGPUVirtualAddress() { return reinterpret_cast<u64>(this); }
};
using ResourcePtr=std::shared_ptr<Resource>;
struct Buffer { ResourcePtr resource=std::make_shared<Resource>();
    ResourcePtr Handle() { return resource; }
};
struct Cache {
    std::mutex mutex;
    Buffer buffer;
    u64 offset=12;
    std::pair<Buffer*,u64> ObtainCPUBuffer(DAddr address,u32 size,
        VideoCommon::ObtainBufferSynchronize,VideoCommon::ObtainBufferOperation) {
        assert(address==0x1234 && size==8); return {&buffer,offset};
    }
};
struct ComObject { void* Get() { return this; } };
struct Profile { int dispatches{}; void Count(int) { ++dispatches; } };
struct Commands {
    std::array<u32,5> constants{};
    std::array<Resource*,5> roots{};
    bool root_set=false,pipeline_set=false,targets_unbound=false;
    int dispatches{},copies{};
    void OMSetRenderTargets(int count,void*,int,void*) {
        assert(count==0); targets_unbound=true;
    }
    void SetComputeRootSignature(void*) { root_set=true; }
    void SetPipelineState(void*) { pipeline_set=true; }
    void SetComputeRoot32BitConstants(int index,int count,const u32* data,int offset) {
        assert(index==0 && count==5 && offset==0);
        for(int i=0;i<5;++i) constants[i]=data[i];
    }
    void SetComputeRootShaderResourceView(int index,u64 address) {
        assert(index==1 || index==2); roots[index]=reinterpret_cast<Resource*>(address);
    }
    void SetComputeRootUnorderedAccessView(int index,u64 address) {
        assert(index==3 || index==4); roots[index]=reinterpret_cast<Resource*>(address);
    }
    void Dispatch(int x,int y,int z) {
        assert(x==1 && y==1 && z==1);
        assert(root_set && pipeline_set && targets_unbound);
        assert(constants[0]==3 && constants[1]==12);
        assert(roots[1]==roots[2]);
        assert(roots[1]->state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(roots[3]->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(roots[4]->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ++dispatches;
    }
    void CopyBufferRegion(Resource* dst,u64 dst_offset,Resource* src,u64 src_offset,u64 size) {
        assert(dst_offset==0 && src_offset==12 && size==8);
        assert(dst->state==4 && src->state==3); ++copies;
    }
};
struct Context {
    struct Commands commands;
    struct Profile profile;
    bool invalidated=false;
    int barriers{};
    std::vector<ResourcePtr> retained;
    ResourcePtr CreateBuffer(u64 size,int) { assert(size==8); return std::make_shared<Resource>(); }
    void InvalidateRecordedState() { invalidated=true; }
    void Transition(ResourcePtr resource,int state) { resource->state=state; }
    void Retain(ResourcePtr resource) { retained.push_back(resource); }
    void GlobalUavBarrier() { ++barriers; }
    void MarkWork(const char*) {}
    void BeforeCopyWrite(ResourcePtr,u64,u64) {}
    struct Commands* Commands() { return &commands; }
    struct Profile& Profile() { return profile; }
};
void QueryUavBarrier(Context& context, const ResourcePtr&, const ResourcePtr&) {
    ++context.barriers;
}
struct Impl {
    Context context;
    Cache cache;
    ResourcePtr predicate,gpu_counters;
    ComObject resolve_root,resolve_pipeline;
    bool predicate_set=false;
    int predicate_op=-1;
    void EnsureGpuCounters() { gpu_counters=std::make_shared<Resource>(); }
};
struct QueryCacheRuntime {
    std::unique_ptr<Impl> impl=std::make_unique<Impl>();
    bool resumed=false;
    void ResumeHostConditionalRendering() {
        assert(impl->predicate_set); resumed=true;
    }
    void SetGpuPredicate(DAddr address,D3D12_PREDICATION_OP op,bool compare_words) {
__BODY__
    }
};
bool Cpu(u64 a,u64 b,bool equal,bool has_second,bool exact_conditional) {
    struct { u64 address; } second{u64(has_second)};
    return __EXPRESSION__;
}
int main() {
    for(u32 low : {0u,1u,0x80000000u,0xffffffffu}) {
        for(u32 high : {0u,1u,0x80000000u,0xffffffffu}) {
            const u64 value=u64(low)|(u64(high)<<32);
            assert(Cpu(value,0,false,false,true)==(low!=0 && high!=0));
            assert(Cpu(value,0,false,false,false)==(value!=0));
            assert(Cpu(value,value,true,true,false));
            assert(!Cpu(value,value,false,true,false));
            assert(!Cpu(value,value^1,true,true,false));
            assert(Cpu(value,value^1,false,true,false));
        }
    }
    for(bool normalize : {false,true}) {
        QueryCacheRuntime runtime;
        runtime.SetGpuPredicate(0x1234,D3D12_PREDICATION_OP_EQUAL_ZERO,normalize);
        auto& impl=*runtime.impl;
        assert(runtime.resumed && impl.predicate_set && impl.predicate_op==0);
        assert(impl.context.commands.dispatches==int(normalize));
        assert(impl.context.commands.copies==int(!normalize));
        assert(impl.context.invalidated==normalize);
        assert(impl.context.barriers==(normalize?2:0));
        assert(impl.context.profile.dispatches==int(normalize));
        auto previous=impl.predicate;
        runtime.SetGpuPredicate(0x1234,D3D12_PREDICATION_OP_EQUAL_ZERO,normalize);
        assert(previous!=impl.predicate);
    }
    QueryCacheRuntime invalid;
    invalid.impl->cache.offset=u64(UINT32_MAX)+1;
    bool rejected=false;
    try { invalid.SetGpuPredicate(0x1234,D3D12_PREDICATION_OP_EQUAL_ZERO,true); }
    catch(const std::out_of_range&) { rejected=true; }
    assert(rejected && !invalid.resumed && !invalid.impl->predicate_set);
}
""".replace("__BODY__", body).replace("__EXPRESSION__", expression)
        with tempfile.TemporaryDirectory(prefix="nxbox-conditional-") as folder:
            cpp, binary = Path(folder) / "predicate.cpp", Path(folder) / "predicate"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
