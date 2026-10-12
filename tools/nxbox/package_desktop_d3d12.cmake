# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.31)
if(NOT WIN32)
  message(FATAL_ERROR "Desktop DLL packaging must run on Windows")
endif()
get_filename_component(NXBOX_BUILD_DIR "${NXBOX_BUILD_DIR}" ABSOLUTE)
set(package "${NXBOX_BUILD_DIR}/package")
set(executable "${NXBOX_BUILD_DIR}/bin/eden-cli.exe")
file(MAKE_DIRECTORY "${package}")
# CPM packages may keep imported SDL/FFmpeg DLLs outside the output directory.
file(GLOB_RECURSE candidates "${NXBOX_BUILD_DIR}/*.dll"
     "${CMAKE_CURRENT_LIST_DIR}/../../.cache/cpm/*.dll")
set(search_dirs "${NXBOX_BUILD_DIR}/bin")
foreach(candidate IN LISTS candidates)
  get_filename_component(directory "${candidate}" DIRECTORY)
  list(APPEND search_dirs "${directory}")
endforeach()
list(REMOVE_DUPLICATES search_dirs)
# DXC is loaded at runtime, so seed both DLLs explicitly as dependency roots.
file(
  GET_RUNTIME_DEPENDENCIES
  EXECUTABLES
  "${executable}"
  LIBRARIES
  "${NXBOX_BUILD_DIR}/bin/dxcompiler.dll"
  "${NXBOX_BUILD_DIR}/bin/dxil.dll"
  DIRECTORIES
  ${search_dirs}
  RESOLVED_DEPENDENCIES_VAR
  dependencies
  UNRESOLVED_DEPENDENCIES_VAR
  missing
  PRE_EXCLUDE_REGEXES
  "api-ms-.*"
  "ext-ms-.*"
  POST_EXCLUDE_REGEXES
  ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*")
if(missing)
  message(FATAL_ERROR "Unresolved desktop runtime dependencies: ${missing}")
endif()
file(
  COPY "${executable}"
       ${dependencies}
       "${NXBOX_BUILD_DIR}/bin/dxcompiler.dll"
       "${NXBOX_BUILD_DIR}/bin/dxil.dll"
       "${NXBOX_BUILD_DIR}/bin/DXC-LICENSE-MS.txt"
       "${NXBOX_BUILD_DIR}/bin/DXC-LICENSE-LLVM.txt"
       "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE.txt"
       "${CMAKE_CURRENT_LIST_DIR}/../../LICENSES"
       "${CMAKE_CURRENT_LIST_DIR}/../../docs/desktop-d3d12.md"
  DESTINATION "${package}")
# Seed app-local MSVC runtime even if dependency resolution found a system copy.
file(GLOB crt "${NXBOX_BUILD_DIR}/bin/msvcp*.dll"
     "${NXBOX_BUILD_DIR}/bin/vcruntime*.dll")
if(crt)
  file(COPY ${crt} DESTINATION "${package}")
endif()
