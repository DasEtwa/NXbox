cmake_minimum_required(VERSION 3.20)
project(NXboxHlslHarness LANGUAGES CXX)

get_filename_component(NXBOX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RECOMPILER "${NXBOX_ROOT}/src/shader_recompiler")
find_package(Boost 1.81 REQUIRED)
# Eden's logging interface uses the fmt 10 API.
include(FetchContent)
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG e69e5f977d458f2650bb346dadf2ad30c5320281
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(fmt)

file(GLOB_RECURSE FRONTEND CONFIGURE_DEPENDS
  "${RECOMPILER}/frontend/*.cpp" "${RECOMPILER}/ir_opt/*.cpp")
add_executable(hlsl-emitter main.cpp host_support.cpp ${FRONTEND})
target_compile_features(hlsl-emitter PRIVATE cxx_std_20)
target_include_directories(hlsl-emitter PRIVATE host "${NXBOX_ROOT}/src" ${Boost_INCLUDE_DIRS})
target_link_libraries(hlsl-emitter PRIVATE fmt::fmt)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(hlsl-emitter PRIVATE -fbracket-depth=1024)
endif()
if(EXISTS "${RECOMPILER}/backend/hlsl/emit_hlsl.h")
  file(GLOB_RECURSE HLSL_SOURCES CONFIGURE_DEPENDS "${RECOMPILER}/backend/hlsl/*.cpp")
  target_sources(hlsl-emitter PRIVATE ${HLSL_SOURCES})
  target_compile_definitions(hlsl-emitter PRIVATE NXBOX_HAS_HLSL=1)
else()
  message(STATUS "SKIP: backend/hlsl is not available; frontend checks remain buildable")
endif()

enable_testing()
foreach(STAGE vs ps cs)
  add_test(NAME hlsl-frontend-${STAGE} COMMAND hlsl-emitter --synthetic ${STAGE} -)
endforeach()
