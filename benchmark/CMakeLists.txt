add_executable(kv_bench ${CMAKE_CURRENT_SOURCE_DIR}/kv_bench.cpp)
target_link_libraries(kv_bench PRIVATE kv::kv userver::ubench)
target_include_directories(kv_bench PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
apply_defaults(kv_bench)
apply_coverage(kv_bench)

if(KV_ENABLE_BENCHMARK_PROFILING)
  find_package(Perl REQUIRED)

  set(KV_BENCHMARK_FLAMEGRAPH_FILTER "^KvBench_RunInTx(ReadOnly|WriteHeavy|MixedReadWrite|EraseUpdate)$")
  set(KV_BENCHMARK_FLAMEGRAPH_DIR ${CMAKE_CURRENT_BINARY_DIR}/flamegraph)

  add_custom_target(
    benchmark_flamegraph
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${KV_BENCHMARK_FLAMEGRAPH_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KV_BENCHMARK_FLAMEGRAPH_DIR}
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && interp=$(readelf -l $<TARGET_FILE:kv_bench> | sed -n 's/.*Requesting program interpreter: \\(.*\\)]/\\1/p') && test -n \"$interp\" && perf record --call-graph dwarf --freq 99 --output ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.perf.data -- \"$interp\" --library-path \"$LD_LIBRARY_PATH\" $<TARGET_FILE:kv_bench> --benchmark_filter='${KV_BENCHMARK_FLAMEGRAPH_FILTER}' --benchmark_min_time=1s --benchmark_repetitions=1 --benchmark_report_aggregates_only=true > ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.stdout 2> ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.stderr"
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && perf script --input ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.perf.data > ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.perf.script"
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && grep -Eq 'kv::|runInTx' ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.perf.script || { echo 'flamegraph perf script contains no kv frames' >&2; echo 'stdout:' >&2; sed -n '1,80p' ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.stdout >&2; echo 'stderr:' >&2; sed -n '1,80p' ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.stderr >&2; exit 1; }"
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && ${PERL_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/collapse_perf_script.pl kv_bench < ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.perf.script > ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.folded"
    COMMAND
      bash --noprofile --norc -lc
      "set -euo pipefail && flamegraph.pl ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.folded > ${KV_BENCHMARK_FLAMEGRAPH_DIR}/flamegraph.svg"
    DEPENDS kv_bench
    USES_TERMINAL
    VERBATIM)
endif()
