function(add_cpp_test target)
  cmake_parse_arguments(ARGS "" "" "SOURCES;LIBS;INCLUDE_DIRS" ${ARGN})

  add_executable(${target} ${ARGS_SOURCES})
  target_link_libraries(${target} PRIVATE userver::utest ${ARGS_LIBS})
  target_include_directories(
    ${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include
                      ${ARGS_INCLUDE_DIRS})
  apply_defaults(${target})
  apply_coverage(${target})

  add_test(NAME ${target} COMMAND ${target})
  set_property(GLOBAL APPEND PROPERTY KV_COVERAGE_TARGETS ${target})
endfunction()

add_cpp_test(
  kv_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/kv_test.cpp LIBS kv::kv)

if(KV_ENABLE_COVERAGE)
  get_property(KV_COVERAGE_TARGETS GLOBAL PROPERTY KV_COVERAGE_TARGETS)
  set(KV_COVERAGE_TARGET_FILES "")
  set(KV_COVERAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/coverage)
  foreach(tgt IN LISTS KV_COVERAGE_TARGETS)
    list(APPEND KV_COVERAGE_TARGET_FILES $<TARGET_FILE:${tgt}>)
  endforeach()

  add_custom_target(
    coverage_html
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${KV_COVERAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${KV_COVERAGE_DIR}
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && LLVM_PROFILE_FILE=${KV_COVERAGE_DIR}/kv-%p.profraw ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure"
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && llvm-profdata merge -sparse ${KV_COVERAGE_DIR}/kv-*.profraw -o ${KV_COVERAGE_DIR}/coverage.profdata"
    COMMAND
      llvm-cov show --format=html
      --output-dir=${KV_COVERAGE_DIR}/html
      --instr-profile=${KV_COVERAGE_DIR}/coverage.profdata
      ${KV_COVERAGE_TARGET_FILES}
      "-ignore-filename-regex=.*/test/.*"
      -show-instantiation-summary
      -Xdemangler llvm-cxxfilt -Xdemangler -n
    DEPENDS ${KV_COVERAGE_TARGETS}
    USES_TERMINAL
    VERBATIM)
endif()
