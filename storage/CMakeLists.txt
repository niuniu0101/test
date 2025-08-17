set(SOURCES
        disk_manager.cpp
        buffer_pool_ex.cpp
        buffer_pool_manager.cpp
        page_handle.cpp
        ../replacer/replacer.h
        ../replacer/lru_replacer.cpp
)
add_library(storage STATIC ${SOURCES})
