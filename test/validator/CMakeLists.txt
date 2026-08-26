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

add_executable(test-augmentation-replay augmentation-replay-test.cpp)
target_link_libraries(test-augmentation-replay PRIVATE
    td-test-main
    ton_crypto
)
add_test(NAME test-augmentation-replay COMMAND test-augmentation-replay)
add_dependencies(all-tests test-augmentation-replay)

add_executable(test-transaction-record-handoff transaction-record-handoff-test.cpp)
target_include_directories(test-transaction-record-handoff PRIVATE ${CMAKE_SOURCE_DIR}/validator/impl)
target_link_libraries(test-transaction-record-handoff PRIVATE
    td-test-main
    ton_crypto
)
add_test(NAME test-transaction-record-handoff COMMAND test-transaction-record-handoff)
add_dependencies(all-tests test-transaction-record-handoff)
