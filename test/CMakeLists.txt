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
  foreach(tgt IN LISTS KV_COVERAGE_TARGETS)
    list(APPEND KV_COVERAGE_TARGET_FILES $<TARGET_FILE:${tgt}>)
  endforeach()

  add_custom_target(
    coverage_html
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
    COMMAND
      ${CMAKE_COMMAND} -E env
      LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/coverage/kv-%p.profraw
      ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND
      bash -lc
      "llvm-profdata merge -sparse ${CMAKE_CURRENT_BINARY_DIR}/coverage/kv-*.profraw -o ${CMAKE_CURRENT_BINARY_DIR}/coverage/coverage.profdata"
    COMMAND
      llvm-cov show --format=html
      --output-dir=${CMAKE_CURRENT_BINARY_DIR}/coverage/html
      --instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/coverage.profdata
      ${KV_COVERAGE_TARGET_FILES}
      "-ignore-filename-regex=.*/test/.*"
      -show-instantiation-summary
      -Xdemangler llvm-cxxfilt -Xdemangler -n
    DEPENDS ${KV_COVERAGE_TARGETS}
    USES_TERMINAL
    VERBATIM)
endif()
