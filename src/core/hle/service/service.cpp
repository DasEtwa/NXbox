// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <map>
#include <string_view>
#include <thread>
#include <fmt/ranges.h>
#include "common/assert.h"
#include "common/logging.h"
#include "common/nxbox_wait_diag.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/service_trace.h"
#include "core/hle/service/sm/sm.h"
#include "core/reporter.h"

namespace Service {

static std::shared_ptr<ServiceTrace> GetServiceTrace(Core::System& system) {
    static std::mutex mutex;
    static std::map<Core::System*, std::weak_ptr<ServiceTrace>> traces;
    const std::scoped_lock guard{mutex};
    std::erase_if(traces, [](const auto& entry) { return entry.second.expired(); });
    auto& weak_trace = traces[&system];
    auto trace = weak_trace.lock();
    if (!trace) {
        const char* value = std::getenv("NXBOX_SERVICE_TRACE");
        trace = std::make_shared<ServiceTrace>(value && std::string_view{value} == "1");
        weak_trace = trace;
    }
    return trace;
}

void ServiceFrameworkBase::TraceRequest(HLERequestContext& ctx, u32 command) {
    const bool deferred = ctx.GetIsDeferred();
    const std::optional<u32> result =
        ctx.HasResponse() && !deferred
            ? std::optional{ctx.CommandBuffer()[ctx.GetDataPayloadOffset()]}
            : std::nullopt;
    if (service_trace->RecordCall(service_name, ctx.IsTipc(), command, result, deferred,
                                  interface_name)) {
        LOG_INFO(
            Service, "NXBOX_SERVICE_TRACE service={} protocol={} cmd={} result={} interface={}",
            service_name, ctx.IsTipc() ? "TIPC" : "CMIF", command,
            result ? fmt::format("{:#010x}", *result) : (deferred ? "deferred" : "no-response"),
            interface_name);
    }
}

/// @brief Creates a function string for logging, complete with the name (or header code, depending
/// on what's passed in) the port name, and all the cmd_buf arguments.
[[maybe_unused]] static std::string MakeFunctionString(std::string_view name, std::string_view port_name, const u32* cmd_buf) {
    // Number of params == bits 0-5 + bits 6-11
    int num_params = (cmd_buf[0] & 0x3F) + ((cmd_buf[0] >> 6) & 0x3F);
    std::string function_string = fmt::format("fn '{}': port={}", name, port_name);
    for (int i = 1; i <= num_params; ++i)
        function_string += fmt::format(", cmd_buf[{}]={:#x}", i, cmd_buf[i]);
    return function_string;
}

ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* service_name_,
                                           u32 max_sessions_, InvokerFn* handler_invoker_,
                                           std::string_view interface_name_)
    : SessionRequestHandler(system_.Kernel(), service_name_),
      service_trace{GetServiceTrace(system_)}, interface_name{interface_name_}, system{system_},
      service_name{service_name_}, is_i_storage{std::string_view{service_name_} == "IStorage"},
      handler_invoker{handler_invoker_}, max_sessions{max_sessions_} {}

ServiceFrameworkBase::~ServiceFrameworkBase() {
    // Wait for other threads to release access before destroying
    const auto guard = ServiceFrameworkBase::LockService();
}

void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers.reserve(handlers.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace_hint(handlers.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers_tipc.reserve(handlers_tipc.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers_tipc.emplace_hint(handlers_tipc.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::ReportUnimplementedFunction(HLERequestContext& ctx,
                                                       const FunctionInfoBase* info) {
    const bool first_missing =
        service_trace->RecordMissing(service_name, ctx.IsTipc(), ctx.GetCommand(), interface_name);
    if (!first_missing) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Settings::values.use_auto_stub ? ResultSuccess : IPC::ResultNotSupported);
        return;
    }
    LOG_WARNING(
        Service,
        "NXBOX_SERVICE_MISSING service={} protocol={} cmd={} name={} auto_stub={} interface={}",
        service_name, ctx.IsTipc() ? "TIPC" : "CMIF", u32{ctx.GetCommand()},
        info ? info->name : "<unknown>", Settings::values.use_auto_stub.GetValue(), interface_name);
    auto cmd_buf = ctx.CommandBuffer();
    std::string function_name = info == nullptr ? "<unknown>" : info->name;

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "function '{}({})': port='{}' cmd_buf={{[0]={:#x}", ctx.GetCommand(), function_name, service_name, cmd_buf[0]);
    for (int i = 1; i <= 8; ++i)
        fmt::format_to(std::back_inserter(buf), ", [{}]={:#x}", i, cmd_buf[i]);
    buf.push_back('}');

    if (info == nullptr) {
        const u32 command = ctx.GetCommand();
        bool linear_match = false;
        std::string keys;
        for (const auto& [key, value] : handlers) {
            linear_match = linear_match || key == command || value.expected_header == command;
            fmt::format_to(std::back_inserter(keys), "{}:{} ", key, value.expected_header);
        }
        LOG_CRITICAL(Service,
                     "handler lookup miss: port={} command={} size={} buckets={} hash={:#x} "
                     "linear_match={} keys=[{}]",
                     service_name, command, handlers.size(), handlers.bucket_count(),
                     handlers.hash_function()(command), linear_match, keys);
    }

    system.GetReporter().SaveUnimplementedFunctionReport(ctx, ctx.GetCommand(), function_name, service_name);
    UNIMPLEMENTED_MSG("Unknown / unimplemented {}", fmt::to_string(buf));
    // Without auto-stub, return an error instead of echoing an incoming request as a reply.
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(Settings::values.use_auto_stub ? ResultSuccess : IPC::ResultNotSupported);
}

void ServiceFrameworkBase::InvokeRequest(HLERequestContext& ctx) {
    const auto command = ctx.GetCommand();
    auto it = handlers.find(command);
    const bool is_cmd_read = command == 0;
    FunctionInfoBase const* info = it == handlers.end() ? nullptr : &it->second;
    ctx.has_response = false;
    if (info == nullptr || info->handler_callback == nullptr) {
        ReportUnimplementedFunction(ctx, info);
        TraceRequest(ctx, command);
        return;
    }

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    handler_invoker(this, info->handler_callback, ctx);
    TraceRequest(ctx, command);

    if (is_i_storage && is_cmd_read) {
        const auto* const process = ctx.GetThread().GetOwnerProcess();
        if (process != nullptr && system.IsNVDECActiveForProcess(process->GetId())) {
            std::this_thread::sleep_for(std::chrono::microseconds{600});
        }
    }
}

void ServiceFrameworkBase::InvokeRequestTipc(HLERequestContext& ctx) {
    const u32 command = ctx.GetCommand();
    auto it = handlers_tipc.find(command);
    FunctionInfoBase const* info = it == handlers_tipc.end() ? nullptr : &it->second;
    ctx.has_response = false;
    if (info == nullptr || info->handler_callback == nullptr) {
        ReportUnimplementedFunction(ctx, info);
        TraceRequest(ctx, command);
        return;
    }

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    handler_invoker(this, info->handler_callback, ctx);
    TraceRequest(ctx, command);
}

Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    // Start before LockService: an IPC handler waiting for another request is useful evidence.
    const NxboxWait::Scope wait_scope{
        NxboxWait::Kind::Service, ctx.GetThread().GetThreadId(), service_name,
        std::array<std::uint64_t, 2>{static_cast<u32>(ctx.GetCommandType()), ctx.GetCommand()}};
    const auto guard = LockService();

    Result result = ResultSuccess;

    switch (ctx.GetCommandType()) {
    case IPC::CommandType::Close:
    case IPC::CommandType::TIPC_Close: {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        result = IPC::ResultSessionClosed;
        break;
    }
    case IPC::CommandType::ControlWithContext:
    case IPC::CommandType::Control: {
        system.ServiceManager().InvokeControlRequest(ctx);
        break;
    }
    case IPC::CommandType::RequestWithContext:
    case IPC::CommandType::Request: {
        InvokeRequest(ctx);
        break;
    }
    default:
        if (ctx.IsTipc()) {
            InvokeRequestTipc(ctx);
            break;
        }

        UNIMPLEMENTED_MSG("command_type={}", ctx.GetCommandType());
        break;
    }

    // If emulation was shutdown, we are closing service threads, do not write the response back to
    // memory that may be shutting down as well.
    if (system.IsPoweredOn()) {
        ctx.WriteToOutgoingCommandBuffer();
    }

    return result;
}

} // namespace Service
