add_subdirectory(consensus)

add_executable(test-new-out-msg-queue
  ../test-td-main.cpp
  test-new-out-msg-queue.cpp
)
target_link_libraries(test-new-out-msg-queue PRIVATE ton_validator common tdutils ton_crypto)
ton_test(test-new-out-msg-queue)
