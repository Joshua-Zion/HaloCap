add_executable(core_tests display_list_tests.cpp)
target_link_libraries(core_tests PRIVATE halocap_core)
add_test(NAME core_tests COMMAND core_tests)

# 规则一的链接期兜底：core 不许链接任何库（含系统导入库）。
# include 扫描只挡住头文件，真正调用了 Win32 函数会在这里挂掉。
foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_libs halocap_core ${_prop})
    if(_libs)
        message(FATAL_ERROR
            "halocap_core 链接了依赖（${_prop}: ${_libs}），违反 2.0 计划的规则一。core 必须是纯标准库。")
    endif()
endforeach()
