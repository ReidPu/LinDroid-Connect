cmake_minimum_required(VERSION 3.20)
project(l2a_jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Directly share the root C++20 core library (zero code duplication)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core l2a_core_build)

add_library(l2a_jni SHARED
    jni_bridge.cpp
)

if(ANDROID)
    find_library(log-lib log)
    target_link_libraries(l2a_jni
        PRIVATE
            l2a_core
            ${log-lib}
    )
else()
    find_package(JNI REQUIRED)
    target_include_directories(l2a_jni PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(l2a_jni
        PRIVATE
            l2a_core
    )

    add_executable(test_android_jni
        test_android_jni.cpp
    )
    target_link_libraries(test_android_jni
        PRIVATE
            l2a_core
    )
endif()
