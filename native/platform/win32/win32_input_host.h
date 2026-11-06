#pragma once

#include "input_host.h"

#include <windows.h>

#include <deque>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

// IInputHost 的 Win32 实现：RegisterHotKey + WH_KEYBOARD_LL + ImmAssociateContext。
//
// 一个进程只允许一个输入宿主：低级钩子是**按安装顺序倒着**走链的，装两份就会有两个钩子
// 互相决定谁先看到按键，而这种顺序一旦写进产品就没人能解释。实例指针放在文件作用域，
// 钩子回调只能用静态函数——这是 Win32 的约束，不是偷懒。
class Win32InputHost final : public core::IInputHost {
public:
    Win32InputHost();
    ~Win32InputHost() override;

    Win32InputHost(const Win32InputHost&) = delete;
    Win32InputHost& operator=(const Win32InputHost&) = delete;

    bool Start() override;
    void Close() override;

    bool RegisterHotkey(const core::HotkeySpec& spec, int token) override;
    void UnregisterHotkey(int token) override;

    void SetInterception(const std::vector<core::HotkeySpec>& specs) override;

    void PumpMessages() override;
    bool PollHotkey(int& tokenOut) override;

    std::uintptr_t SuspendIme(core::NativeHandle window) override;
    void ResumeIme(core::NativeHandle window, std::uintptr_t previousContext) override;

    const char* LastErrorKind() const override { return errorKind_; }
    std::int32_t LastErrorCode() const override { return static_cast<std::int32_t>(lastError_); }

    // ---- 只给探针用 ----

    core::NativeHandle Window() const { return window_; }

    // 注入的按键默认一律放过（core 的策略），但「钩子返回值到底能不能拦住」必须是可测的，
    // 而探针唯一能产生的事件就是注入的。这个开关只服务量测，产品路径不动它。
    void SetInterceptionIncludesInjected(bool include) { interceptionIncludesInjected_ = include; }

    std::int64_t HookEventsSeen() const { return hookEventsSeen_; }
    std::int64_t SwallowedCount() const { return swallowed_; }
    std::int64_t HotkeyDeliveries() const { return hotkeyDeliveries_; }

    // 钩子回调里的驻留时间：超过系统的 LowLevelHooksTimeout 之后钩子会被直接跳过，
    // 表现出来就是「偶尔拦不住」，所以这个数要能打印出来。
    std::int64_t HookMaxUsPerEvent() const { return hookMaxUsPerEvent_; }

private:
    static bool EnsureClassRegistered();

    static LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT HandleHook(int code, WPARAM wParam, LPARAM lParam);

    // 只按精确匹配决定吞不吞，跳过「注入一律放过」那一条——仅服务量测。
    core::SwallowDecision MatchOnly(const core::KeyEvent& event);

    std::vector<core::HotkeySpec> interception_;
    std::deque<int> firedTokens_;
    std::vector<std::pair<int, core::HotkeySpec>> registered_;

    void* window_ = nullptr;
    void* hook_ = nullptr;
    bool interceptionIncludesInjected_ = false;

    std::int64_t hookEventsSeen_ = 0;
    std::int64_t swallowed_ = 0;
    std::int64_t hotkeyDeliveries_ = 0;
    std::int64_t hookMaxUsPerEvent_ = 0;

    HRESULT lastError_ = S_OK;
    const char* errorKind_ = "none";
};

// KeySymbol ↔ 原生键盘常量两边的翻译。值与 Win32 的 MOD_* 恰好相同是巧合，
// 这里仍然逐项写出来：哪天 core 加一位就会串到系统常量上。
UINT ToNativeModifiers(std::uint32_t modifiers);
UINT ToNativeVirtualKey(core::KeySymbol key);
core::KeySymbol FromNativeVirtualKey(DWORD virtualKey);

}
