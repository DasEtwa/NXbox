# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Differentially execute the production old and batched reduction shaders."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_query_gpu_reports import function_body
from test_d3d12_draw_bookkeeping import compile_run
from test_d3d12_subresource_feedback import MOCK, function

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


class QueryBatchTests(unittest.TestCase):
    def test_observation_and_submission_hooks(self):
        runtime = (BACKEND / "d3d12_query_cache.cpp").read_text()
        context = (BACKEND / "d3d12_context.cpp").read_text()
        rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        self.assertIn('std::getenv("NXBOX_D3D12_BATCH_QUERIES")', runtime)
        self.assertIn("value && value[0] == '1' && value[1] == '\\0'", runtime)
        read = function_body(runtime, "QueryCacheRuntime::ReadCounter(")
        self.assertIn("++destination->content_version;", read)
        self.assertIn("CopyFilledSize(stream, impl->gpu_report)", read)
        flush = function_body(runtime, "void FlushBatch()")
        self.assertEqual(flush.count("commands->Dispatch("), 1)
        self.assertEqual(flush.count("QueryUavBarrier("), 1)
        self.assertIn("QueryUavBarrier(context, gpu_counters, {});", flush)
        self.assertNotRegex(flush, r"\b(?:Request|Finish|Wait)\(")
        self.assertIn("flushing_batch || batch_operations.empty()", flush)
        self.assertIn("context.before_submit = [this] { impl->FlushBatch(); };", runtime)
        self.assertIn("before_submit();", function_body(context, "u64 Context::Submit("))
        for signature in ("void Context::Transition(", "void Context::TransitionBatch("):
            body = function_body(context, signature)
            self.assertLess(body.index("before_resource_use"), body.index("Retain("))
        for method in ("FlushRegion", "InvalidateRegion", "OnCacheInvalidation", "UnmapMemory"):
            self.assertIn(
                "query_runtime.FlushPendingReports(addr, size)",
                function_body(rasterizer, f"void RasterizerD3D12::{method}("),
            )
        self.assertIn(
            "impl->FlushBatch();", function_body(runtime, "void QueryCacheRuntime::CopyValue(")
        )
        self.assertIn("FlushBatch();", function_body(runtime, "void Rotate()"))

    def test_gpu_observer_orders_writes_even_when_the_read_state_was_cached(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(context, f"void Context::{name}(")
            for name in ("Transition", "TransitionBatch", "TransitionSubresources")
        )
        compile_run(
            MOCK
            + methods
            + r"""
int main() {
    Context context;
    auto resource = std::make_shared<Resource>();
    resource->state = SrvState;
    bool pending = true;
    unsigned drains = 0;
    context.before_resource_use = [&](const ResourcePtr& used) {
        if (used == resource && pending) {
            pending = false;
            ++drains;
            // Materializing a deferred report changes the resource's tracked state.
            used->state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    };
    context.Transition(resource, SrvState);
    assert(drains == 1 && resource->state == SrvState);
    assert(context.recording.calls.back()[0].Transition.StateBefore ==
           D3D12_RESOURCE_STATE_COPY_DEST);
    pending = true;
    const std::pair<ResourcePtr,D3D12_RESOURCE_STATES> uses[]{{resource,SrvState}};
    context.TransitionBatch(uses);
    assert(drains == 2 && resource->state == SrvState);
    assert(context.recording.calls.size() == 2);
    assert(context.recording.calls.back()[0].Transition.StateBefore ==
           D3D12_RESOURCE_STATE_COPY_DEST);
    context.TransitionBatch(uses);
    assert(drains == 2 && context.recording.calls.size() == 2);
}
"""
        )

    def test_random_reports_reads_predicates_and_writes(self):
        shader = (
            (BACKEND / "d3d12_query_resolve.h")
            .read_text()
            .split('R"hlsl(', 1)[1]
            .split(')hlsl"', 1)[0]
        )
        shader = re.sub(
            r"cbuffer Parameters[^}]+};",
            "uint operation, first_slot, slot_count, report_type, report_stream;",
            shader,
        )
        shader = re.sub(r"(?:RW)?ByteAddressBuffer (\w+) : register\([^;]+;", r"Buffer \1;", shader)
        shader = shader.replace("[numthreads(32, 1, 1)]", "").replace(
            "void main(uint3 id : SV_DispatchThreadID)", "void Resolve(uint3 id)"
        )
        enums = (ROOT / "src/video_core/query_cache/types.h").read_text()
        defines = "\n".join(
            f"#define {name} {value}"
            for name, value in re.findall(r"^    (\w+) = (\d+),", enums, re.M)
        )
        driver = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
using uint = std::uint32_t;
struct uint2 { uint x,y; uint2(uint x,uint y):x(x),y(y){} };
struct uint3 { uint x,y,z; };
struct uint4 { uint x,y,z,w; };
struct Buffer {
    std::vector<uint> data = std::vector<uint>(16384);
    uint2 Load2(uint n) { return {data.at(n/4),data.at(n/4+1)}; }
    uint4 Load4(uint n) { return {data.at(n/4),data.at(n/4+1),data.at(n/4+2),data.at(n/4+3)}; }
    void Store2(uint n,uint2 v) { data.at(n/4)=v.x; data.at(n/4+1)=v.y; }
};
#define TYPE_COUNT 32
#define RESULT_STRIDE 160
#define SO_OFFSET 96
__DEFINES__
__SHADER__
struct Destination { uint address,size; };
int main() {
    results.data.resize(32768);
    for (uint seed=0;seed<128;++seed) {
        std::mt19937 random(seed);
        Buffer old_counters, new_counters;
        std::array<unsigned char,256> old_memory{}, new_memory{};
        old_memory.fill(0x55); new_memory=old_memory;
        std::vector<std::array<uint,8>> instructions;
        std::vector<Destination> destinations;
        std::array<uint,128> lanes;
        std::iota(lanes.begin(),lanes.end(),0);
        uint slot=0;
        auto execute=[&](Buffer& state,uint op,uint first,uint count,uint type,uint stream) {
            counters=state;
            operation=op; first_slot=first; slot_count=count; report_type=type; report_stream=stream;
            // Arbitrary lane order checks that batches have no inter-lane dependencies.
            std::shuffle(lanes.begin(),lanes.end(),random);
            for(uint lane:lanes) Resolve({lane,0,0});
            state=counters;
        };
        auto value=[&](uint index) {
            const auto v=report.Load2(index*8);
            return (std::uint64_t(v.y)<<32)|v.x;
        };
        auto drain=[&] {
            if(instructions.empty()) return;
            std::memcpy(metadata.data.data()+4096/4,instructions.data(),instructions.size()*32);
            execute(new_counters,4,4096,uint(instructions.size()),0,0);
            for(uint i=0;i<destinations.size();++i) {
                auto v=value(i); const auto& d=destinations[i];
                std::memcpy(new_memory.data()+d.address,&v,d.size);
            }
            assert(new_counters.data==old_counters.data);
            assert(new_memory==old_memory);
            instructions.clear(); destinations.clear();
        };
        for(uint step=0;step<4000;++step) {
            uint action=random()%12;
            if(instructions.size()>=900 || slot==256) { drain(); slot=0; }
            if(action<3) {
                // Draw results include carry, inactive/poisoned slots, SO streams and GS.
                uint physical_slot = (seed % 3) * 256 + slot;
                for(uint field=0;field<40;++field) results.data.at(physical_slot*40+field)=random();
                auto base=slot*4;
                metadata.data[base]=random()%2;
                metadata.data[base+1]=random()%2;
                metadata.data[base+2]=random()%2;
                metadata.data[base+3]=random()%16;
                std::array<uint,4> saved;
                std::copy_n(metadata.data.begin(),4,saved.begin());
                for(uint i=0;i<4;++i) metadata.data[i]=metadata.data[base+i];
                execute(old_counters,0,physical_slot,1,0,0);
                std::copy(saved.begin(),saved.end(),metadata.data.begin());
                instructions.push_back({0,physical_slot,1,0,0,0,0,0});
                ++slot;
            } else if(action==3) {
                uint type=random()%32;
                execute(old_counters,2,0,0,type,0);
                instructions.push_back({2,0,0,type,0,0,0,0});
            } else if(action<8) {
                uint type=random()%32, stream=random()%4;
                uint address=(random()%60)*4, size=(random()%2+1)*4;
                execute(old_counters,1,0,0,type,stream);
                auto v=value(0);
                std::memcpy(old_memory.data()+address,&v,size);
                instructions.push_back({1,0,0,type,stream,uint(destinations.size()),0,0});
                destinations.push_back({address,size});
            } else {
                // CPU/buffer reads, GPU consumers, predicates, semaphores and list seals
                // are observation boundaries. Partial writes happen after prior reports.
                drain();
                uint address=(random()%60)*4;
                std::uint64_t old_value{}, new_value{};
                std::memcpy(&old_value,old_memory.data()+address,8);
                std::memcpy(&new_value,new_memory.data()+address,8);
                assert(old_value==new_value);
                assert((old_value!=0)==(new_value!=0));
                assert((uint(old_value)!=0 && uint(old_value>>32)!=0)==
                       (uint(new_value)!=0 && uint(new_value>>32)!=0));
                if(action==11) {
                    uint v=random();
                    std::memcpy(old_memory.data()+address,&v,4);
                    std::memcpy(new_memory.data()+address,&v,4);
                }
            }
        }
        drain();
    }
}
""".replace("__DEFINES__", defines).replace("__SHADER__", shader)
        with tempfile.TemporaryDirectory(prefix="nxbox-query-batch-") as folder:
            cpp, binary = Path(folder) / "batch.cpp", Path(folder) / "batch"
            cpp.write_text(driver)
            result = subprocess.run(
                ["clang++", "-std=c++20", "-O2", str(cpp), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=60)
