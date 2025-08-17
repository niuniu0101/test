set(SOURCES lock_t.cpp transaction_manager.cpp)
add_library(transaction STATIC ${SOURCES})
target_link_libraries(transaction system recovery pthread)