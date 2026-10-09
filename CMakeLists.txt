# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 The typmax-router authors
cmake_minimum_required(VERSION 3.16)

# macOS: the SDK to build against, when it must not be the toolchain's default
# (some Command Line Tools installs ship a newest SDK whose .tbd stubs fail to
# link). -DTYPMAX_OSX_SYSROOT=<sdk path>, or the TYPMAX_OSX_SYSROOT environment
# variable at the first configure; empty (the default) leaves CMake's own choice.
# Must be set before project().
set(TYPMAX_OSX_SYSROOT "$ENV{TYPMAX_OSX_SYSROOT}" CACHE PATH "macOS SDK to build against (empty: the default)")
if(APPLE AND TYPMAX_OSX_SYSROOT AND NOT CMAKE_OSX_SYSROOT)
  set(CMAKE_OSX_SYSROOT "${TYPMAX_OSX_SYSROOT}" CACHE PATH "" FORCE)
endif()

project(typmax_router LANGUAGES CXX)

# KiCad 10 is C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- the vendored KiCad tree ------------------------------------------------
# vendor/kicad/ is byte-for-byte KiCad at the tag vendor/kicad.manifest names.
# Our own changes are patches/series, applied here to a COPY in the build
# directory; with an empty series the build reads vendor/kicad/ directly.
set(KICAD_VENDOR ${CMAKE_SOURCE_DIR}/vendor/kicad)
file(STRINGS ${CMAKE_SOURCE_DIR}/patches/series SERIES_LINES)
set(PATCHES)
foreach(l ${SERIES_LINES})
  string(STRIP "${l}" l)
  if(l AND NOT l MATCHES "^#")
    list(APPEND PATCHES ${l})
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_SOURCE_DIR}/patches/series ${CMAKE_SOURCE_DIR}/vendor/kicad.manifest
             ${CMAKE_SOURCE_DIR}/tools/kicad_closure.txt)
if(PATCHES)
  set(K ${CMAKE_BINARY_DIR}/kicad-patched)
  file(REMOVE_RECURSE ${K})
  file(COPY ${KICAD_VENDOR}/ DESTINATION ${K})
  find_program(PATCH_EXE patch REQUIRED)
  set(n 0)
  foreach(p ${PATCHES})
    math(EXPR n "${n} + 1")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/${p})
    execute_process(COMMAND ${PATCH_EXE} -p1 --forward --batch --silent -i ${CMAKE_SOURCE_DIR}/patches/${p}
                    WORKING_DIRECTORY ${K} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "patch ${n} (patches/${p}) does not apply to vendor/kicad")
    endif()
  endforeach()
  message(STATUS "vendor/kicad + ${n} patch(es) -> ${K}")
else()
  set(K ${KICAD_VENDOR})
endif()

# The translation units: every .cpp tools/kicad_closure.txt vendors.
file(STRINGS ${CMAKE_SOURCE_DIR}/tools/kicad_closure.txt CLOSURE_LINES)
set(KICAD_SOURCES)
foreach(l ${CLOSURE_LINES})
  if(l MATCHES "^[^#].*\\.cpp$")
    list(APPEND KICAD_SOURCES ${K}/${l})
  endif()
endforeach()
# KiCad's own time_limit.cpp reads wxWidgets' clock; shim/time_limit.cpp replaces it.
list(APPEND KICAD_SOURCES ${CMAKE_SOURCE_DIR}/shim/time_limit.cpp)

# Floating-point contraction off on every target (review D-2): Apple clang fuses
# multiply-adds by default (-ffp-contract=on) where the x86-64 baseline build
# cannot, so the two binaries would round different expressions. With it off,
# both round every product. No -ffast-math anywhere.
add_compile_options(-ffp-contract=off)

add_library(pns STATIC ${KICAD_SOURCES})
# shim/include FIRST: its stand-ins (wxWidgets, the board model, settings,
# the thread pool, ADVANCED_CFG) shadow the KiCad headers the router includes.
target_include_directories(pns SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/shim/include
  ${K}/pcbnew/router ${K}/pcbnew ${K}/include ${K}/libs/kimath/include ${K}/libs/core/include
  ${K}/thirdparty/clipper2/Clipper2Lib/include ${K}/thirdparty/rtree)
# USINGZ: KiCad builds Clipper2 with Z coordinates (thirdparty/clipper2/CMakeLists.txt)
target_compile_definitions(pns PUBLIC _USE_MATH_DEFINES NDEBUG USINGZ)
# vendored code is kept byte-for-byte: silence its warnings rather than edit it
target_compile_options(pns PRIVATE -w)

# The engine identity every route/drag answer carries: the tag and the commit
# (12 hex digits) vendor/kicad.manifest records.
file(STRINGS ${CMAKE_SOURCE_DIR}/vendor/kicad.manifest KICAD_TAG_LINE REGEX "^tag ")
file(STRINGS ${CMAKE_SOURCE_DIR}/vendor/kicad.manifest KICAD_COMMIT_LINE REGEX "^commit ")
string(REGEX REPLACE "^tag " "" KICAD_TAG "${KICAD_TAG_LINE}")
string(REGEX REPLACE "^commit ([0-9a-f]+)$" "\\1" KICAD_COMMIT "${KICAD_COMMIT_LINE}")
string(SUBSTRING "${KICAD_COMMIT}" 0 12 KICAD_COMMIT)
if(NOT KICAD_TAG OR NOT KICAD_COMMIT)
  message(FATAL_ERROR "vendor/kicad.manifest names no tag/commit: run tools/update_kicad.sh <tag>")
endif()
message(STATUS "vendored KiCad: ${KICAD_TAG} (${KICAD_COMMIT})")

add_executable(typmax-router src/main.cpp src/json.cpp src/board.cpp src/engine.cpp)
target_compile_definitions(typmax-router PRIVATE TYPMAX_KICAD_TAG="${KICAD_TAG}" TYPMAX_KICAD_COMMIT="${KICAD_COMMIT}")
target_link_libraries(typmax-router PRIVATE pns)
target_compile_options(typmax-router PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(typmax-router PRIVATE Threads::Threads)

enable_testing()
find_program(PYTHON3 NAMES python3.11 python3)
add_test(NAME synthetic COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
         --binary $<TARGET_FILE:typmax-router> --suite synthetic)
add_test(NAME determinism COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
         --binary $<TARGET_FILE:typmax-router> --suite determinism)
add_test(NAME fixtures COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
         --binary $<TARGET_FILE:typmax-router> --suite fixtures)
set_tests_properties(fixtures PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
add_test(NAME watcher COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_watcher.py)
