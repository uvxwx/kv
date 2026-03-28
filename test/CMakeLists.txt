add_executable(version_test version_test.cpp)
target_link_libraries(version_test PRIVATE userver::utest kv::kv)
target_include_directories(version_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
apply_defaults(version_test)

add_test(NAME version_test COMMAND version_test)
