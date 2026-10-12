# SPDX-License-Identifier: 0BSD
# Configure-only profile for building dynarmic_tests from the embedding project.
# The desktop SparseLargeVector implementation is unrelated to this test target.
# Keep the production common target untouched outside this explicit test profile.
function(dynarmic_cpu_test_profile)
    if(NOT DYNARMIC_TESTS OR NOT TARGET common)
        message(FATAL_ERROR "The CPU test profile requires DYNARMIC_TESTS and common")
    endif()
    if(MSVC)
        get_target_property(common_sources common SOURCES)
        list(FILTER common_sources EXCLUDE REGEX "(^|/)sparse_large_vector\\.cpp$")
        set_property(TARGET common PROPERTY SOURCES "${common_sources}")
        message(STATUS "CPU test profile: omit unrelated desktop SparseLargeVector")
    endif()
endfunction()

cmake_language(DEFER CALL dynarmic_cpu_test_profile)
