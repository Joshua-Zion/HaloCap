add_executable(core_tests display_list_tests.cpp)
target_link_libraries(core_tests PRIVATE halocap_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(scheduler_tests frame_scheduler_tests.cpp)
target_link_libraries(scheduler_tests PRIVATE halocap_core)
add_test(NAME scheduler_tests COMMAND scheduler_tests)

add_executable(capture_policy_tests capture_policy_tests.cpp)
target_link_libraries(capture_policy_tests PRIVATE halocap_core)
add_test(NAME capture_policy_tests COMMAND capture_policy_tests)

add_executable(encoder_policy_tests encoder_policy_tests.cpp)
target_link_libraries(encoder_policy_tests PRIVATE halocap_core)
add_test(NAME encoder_policy_tests COMMAND encoder_policy_tests)

add_executable(audio_policy_tests audio_policy_tests.cpp)
target_link_libraries(audio_policy_tests PRIVATE halocap_core)
add_test(NAME audio_policy_tests COMMAND audio_policy_tests)

add_executable(input_policy_tests input_policy_tests.cpp)
target_link_libraries(input_policy_tests PRIVATE halocap_core)
add_test(NAME input_policy_tests COMMAND input_policy_tests)

add_executable(settings_policy_tests settings_policy_tests.cpp)
target_link_libraries(settings_policy_tests PRIVATE halocap_core)
add_test(NAME settings_policy_tests COMMAND settings_policy_tests)

add_executable(store_policy_tests store_policy_tests.cpp)
target_link_libraries(store_policy_tests PRIVATE halocap_core)
add_test(NAME store_policy_tests COMMAND store_policy_tests)

add_executable(capability_policy_tests capability_policy_tests.cpp)
target_link_libraries(capability_policy_tests PRIVATE halocap_core)
add_test(NAME capability_policy_tests COMMAND capability_policy_tests)

# §3.3 规则三的链接期兜底：core 不许链接任何库（含系统导入库）。
# include 扫描只挡住头文件，真正调用了 Win32 函数会在这里挂掉。
foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_libs halocap_core ${_prop})
    if(_libs)
        message(FATAL_ERROR
            "halocap_core 链接了依赖（${_prop}: ${_libs}），违反 2.0 计划的 §3.3 规则三。core 必须是纯标准库。")
    endif()
endforeach()
