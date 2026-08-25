add_subdirectory(consensus)

add_executable(test-ext-message-reuse ext-message-reuse-test.cpp)
target_link_libraries(test-ext-message-reuse PRIVATE
    td-test-main
    smc-envelope
    ton_validator
    validator
)
add_test(NAME test-ext-message-reuse COMMAND test-ext-message-reuse)
add_dependencies(all-tests test-ext-message-reuse)
