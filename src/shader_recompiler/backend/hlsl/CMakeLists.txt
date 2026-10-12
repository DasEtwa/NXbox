# SPDX-License-Identifier: GPL-3.0-or-later
set(HLSL_BACKEND_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_atomic.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_bitwise_conversion.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_composite.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_context_get_set.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_convert.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_floating_point.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_image.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_integer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_logical.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_memory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_not_implemented.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_select.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_special.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_undefined.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_unsupported.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/emit_hlsl_warp.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/hlsl_emit_context.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/var_alloc.cpp"
)
if(TARGET shader_recompiler)
    target_sources(shader_recompiler PRIVATE ${HLSL_BACKEND_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/emit_hlsl.h
        ${CMAKE_CURRENT_LIST_DIR}/hlsl_root_constants.h)
endif()
