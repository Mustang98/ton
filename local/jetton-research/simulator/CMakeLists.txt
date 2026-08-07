add_executable(jetton-simulator
  main.cpp
  spam.cpp
  spam.h
  workload.cpp
  workload.h
  contracts.h
)

target_include_directories(jetton-simulator PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)

target_link_libraries(jetton-simulator PRIVATE
  tdutils
  ton_crypto
  ton_block
  lite-client-common
)
