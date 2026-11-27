cmake_minimum_required(VERSION 3.20)

find_package(Threads REQUIRED)

add_library(l2a_core STATIC
    src/sha256.cpp
    src/protocol.cpp
    src/tls_channel.cpp
    src/discovery.cpp
    src/clipboard_stream.cpp
    src/history_manager.cpp
    src/file_transfer.cpp
    src/engine.cpp
)

target_include_directories(l2a_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(l2a_core PRIVATE
    -Wall -Wextra -Wpedantic
)

set_target_properties(l2a_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

option(L2A_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(L2A_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(L2A_ENABLE_ASAN)
    target_compile_options(l2a_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(l2a_core PUBLIC -fsanitize=address,undefined)
endif()
if(L2A_ENABLE_TSAN)
    target_compile_options(l2a_core PUBLIC -fsanitize=thread)
    target_link_options(l2a_core PUBLIC -fsanitize=thread)
endif()

target_link_libraries(l2a_core
    PUBLIC
        Threads::Threads
)
