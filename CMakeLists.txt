cmake_minimum_required(VERSION 3.16)
project(nandsim VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NANDSIM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(NANDSIM_WERROR "Treat warnings as errors" OFF)

add_library(nandsim STATIC
  src/flash_model.cpp src/ftl.cpp src/gc.cpp src/wear.cpp
  src/workload.cpp src/crash_harness.cpp)
target_include_directories(nandsim PUBLIC include)

function(nandsim_configure target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(NANDSIM_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(NANDSIM_SANITIZE)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(NANDSIM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(NANDSIM_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

nandsim_configure(nandsim)

add_executable(nandsim_cli src/main.cpp)
target_link_libraries(nandsim_cli PRIVATE nandsim)
nandsim_configure(nandsim_cli)

enable_testing()
add_executable(nandsim_tests
  tests/test_main.cpp tests/test_flash_model.cpp tests/test_ftl.cpp
  tests/test_gc_wear.cpp tests/test_recovery.cpp tests/test_fuzz.cpp tests/test_workload.cpp)
target_link_libraries(nandsim_tests PRIVATE nandsim)
nandsim_configure(nandsim_tests)

# One ctest entry per suite so failures are attributable (ctest -R recovery).
foreach(suite flash ftl gc wear recovery fuzz workload)
  add_test(NAME ${suite} COMMAND nandsim_tests --filter ${suite}.)
endforeach()
