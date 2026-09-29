add_executable(ringbuffer_test ringbuffer_test.cpp)
target_include_directories(ringbuffer_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(ringbuffer_test PRIVATE -Wall -Wextra -O1 -g)
find_package(Threads REQUIRED)
target_link_libraries(ringbuffer_test PRIVATE Threads::Threads)
nas_add_tsan(ringbuffer_test)
add_test(NAME ringbuffer COMMAND ringbuffer_test)

add_executable(logger_test logger_test.cpp)
target_link_libraries(logger_test PRIVATE nas_common)
target_compile_options(logger_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(logger_test)
add_test(NAME logger COMMAND logger_test)

# 测试替身库（FakeSystemState 等），仅测试使用。
add_library(nas_test_support STATIC
    fakes/FakeSystemState.cpp
)
target_include_directories(nas_test_support PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(nas_test_support PRIVATE -Wall -Wextra -O1 -g)
target_link_libraries(nas_test_support PUBLIC nas_platform)
nas_add_tsan(nas_test_support)

add_executable(fake_systemstate_test fake_systemstate_test.cpp)
target_link_libraries(fake_systemstate_test PRIVATE nas_test_support)
target_compile_options(fake_systemstate_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(fake_systemstate_test)
add_test(NAME fake_systemstate COMMAND fake_systemstate_test)

add_executable(linux_systemstate_test linux_systemstate_test.cpp)
target_include_directories(linux_systemstate_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(linux_systemstate_test PRIVATE nas_platform)
target_compile_options(linux_systemstate_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(linux_systemstate_test)
add_test(NAME linux_systemstate COMMAND linux_systemstate_test)

add_executable(system_monitor_test system_monitor_test.cpp)
target_link_libraries(system_monitor_test PRIVATE nas_system nas_test_support)
target_compile_options(system_monitor_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(system_monitor_test)
add_test(NAME system_monitor COMMAND system_monitor_test)

add_executable(http_test http_test.cpp)
target_link_libraries(http_test PRIVATE nas_http nas_test_support)
target_compile_options(http_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(http_test)
add_test(NAME http COMMAND http_test)

add_executable(health_probe_test health_probe_test.cpp)
target_link_libraries(health_probe_test PRIVATE nas_http)
target_compile_options(health_probe_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(health_probe_test)
add_test(NAME health_probe COMMAND health_probe_test)

add_executable(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE nas_config)
target_compile_options(config_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(config_test)
add_test(NAME config COMMAND config_test)

add_executable(file_service_test file_service_test.cpp)
target_link_libraries(file_service_test PRIVATE nas_file)
target_compile_options(file_service_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(file_service_test)
add_test(NAME file_service COMMAND file_service_test)

add_executable(file_api_test file_api_test.cpp)
target_link_libraries(file_api_test PRIVATE nas_http nas_file)
target_compile_options(file_api_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(file_api_test)
add_test(NAME file_api COMMAND file_api_test)

add_executable(web_ui_test web_ui_test.cpp)
target_link_libraries(web_ui_test PRIVATE nas_http)
target_compile_options(web_ui_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(web_ui_test)
add_test(NAME web_ui COMMAND web_ui_test)

add_executable(buffer_pool_test buffer_pool_test.cpp)
target_link_libraries(buffer_pool_test PRIVATE nas_io)
target_compile_options(buffer_pool_test PRIVATE -Wall -Wextra -O1 -g)
nas_add_tsan(buffer_pool_test)
add_test(NAME buffer_pool COMMAND buffer_pool_test)

if (NAS_HAS_IO_URING)
    add_executable(io_uring_test io_uring_test.cpp)
    target_link_libraries(io_uring_test PRIVATE nas_io)
    target_compile_options(io_uring_test PRIVATE -Wall -Wextra -O1 -g)
    nas_add_tsan(io_uring_test)
    add_test(NAME io_uring COMMAND io_uring_test)
endif()
