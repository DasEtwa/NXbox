cmake_minimum_required(VERSION 3.20)
project(NXboxShaderCorpus LANGUAGES CXX)

get_filename_component(NXBOX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(RECOMPILER "${NXBOX_ROOT}/src/shader_recompiler")
find_package(Boost 1.81 REQUIRED)
include(FetchContent)
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG e69e5f977d458f2650bb346dadf2ad30c5320281
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(fmt)

file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
  "${RECOMPILER}/frontend/*.cpp" "${RECOMPILER}/ir_opt/*.cpp"
  "${RECOMPILER}/backend/hlsl/*.cpp")
add_executable(nxbox-shader-corpus main.cpp environment.cpp
  "${NXBOX_ROOT}/tools/hlsl-harness/host_support.cpp" ${SHADER_SOURCES})
target_compile_features(nxbox-shader-corpus PRIVATE cxx_std_20)
target_include_directories(nxbox-shader-corpus PRIVATE
  "${NXBOX_ROOT}/tools/hlsl-harness/host" "${NXBOX_ROOT}/src")
target_include_directories(nxbox-shader-corpus SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(nxbox-shader-corpus PRIVATE fmt::fmt)
if(MSVC)
  target_compile_definitions(nxbox-shader-corpus PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # Match Eden's permitted legacy constructs; keep remaining warnings fatal.
  target_compile_options(nxbox-shader-corpus PRIVATE /W4 /WX /permissive-
    /Zc:preprocessor /utf-8 /wd4100 /wd4201 /wd4324)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(nxbox-shader-corpus PRIVATE -fbracket-depth=1024)
endif()

add_executable(nxbox-corpus-environment-test environment_test.cpp environment.cpp)
target_compile_features(nxbox-corpus-environment-test PRIVATE cxx_std_20)
target_include_directories(nxbox-corpus-environment-test PRIVATE
  "${NXBOX_ROOT}/tools/hlsl-harness/host" "${NXBOX_ROOT}/src")
target_include_directories(nxbox-corpus-environment-test SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(nxbox-corpus-environment-test PRIVATE fmt::fmt)
if(MSVC)
  target_compile_definitions(nxbox-corpus-environment-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(nxbox-corpus-environment-test PRIVATE /W4 /WX /permissive-
    /Zc:preprocessor /utf-8 /wd4100 /wd4201 /wd4324)
endif()
add_dependencies(nxbox-shader-corpus nxbox-corpus-environment-test)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
enable_testing()
add_test(NAME shader-corpus-synthetic
  COMMAND ${CMAKE_COMMAND} -E env
    "NXBOX_SHADER_CORPUS_EMITTER=$<TARGET_FILE:nxbox-shader-corpus>"
    ${Python3_EXECUTABLE} "${NXBOX_ROOT}/tests/port/test_shader_corpus.py")
set_tests_properties(shader-corpus-synthetic PROPERTIES TIMEOUT 180)
