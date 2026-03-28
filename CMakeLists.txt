cmake_minimum_required(VERSION 3.29)
project(kv CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds are not supported. Please create a separate build directory and run CMake from there."
  )
endif()

# Ensure that userver integration does not create Python virtual environments
# when this project is configured. Helpers should use the system Python
# specified by USERVER_PYTHON_PATH instead.
set(USERVER_TESTSUITE_USE_VENV
    OFF
    CACHE BOOL "Disable userver testsuite Python virtualenvs" FORCE)
set(USERVER_SQL_USE_VENV
    OFF
    CACHE BOOL "Disable userver SQL helper Python virtualenvs" FORCE)
set(USERVER_CHAOTIC_USE_VENV
    OFF
    CACHE BOOL "Disable userver chaotic helper Python virtualenvs" FORCE)

set(CMAKE_LINKER_TYPE MOLD)

function(apply_defaults target)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD 26)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET ${target} PROPERTY CXX_EXTENSIONS OFF)
  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
            -Wfloat-equal
            -Wdisabled-optimization
            -Wctad-maybe-unsupported)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-ggdb3>
                                           $<$<CONFIG:Release>:-O2>)
endfunction()

find_package(userver CONFIG REQUIRED COMPONENTS core)
find_package(fmt CONFIG REQUIRED)

add_library(libkv STATIC src/kv/kv.cpp)
set_target_properties(libkv PROPERTIES OUTPUT_NAME kv)
add_library(kv::kv ALIAS libkv)
target_include_directories(libkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(libkv PUBLIC userver::core)
apply_defaults(libkv)

add_executable(kv_cli src/main.cpp)
set_target_properties(kv_cli PROPERTIES OUTPUT_NAME kv)
target_link_libraries(kv_cli PRIVATE kv::kv fmt::fmt)
apply_defaults(kv_cli)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(test)
endif()
