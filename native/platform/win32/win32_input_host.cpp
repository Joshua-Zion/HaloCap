#include "win32_input_host.h"

#include <imm.h>

#include <algorithm>
#include <cstdint>

namespace halocap::platform::win32 {

namespace {

namespace core = halocap::core;

constexpr wchar_t kWindowClassName[] = L"HaloCapNativeInputHost";

// 钩子回调只能拿到静态函数，实例指针放在这里。一个进程一个输入宿主，
// 由 Start/Close 维护；第二个实例再 Start 会把第一个的钩子换掉，这是设计上的单例。
Win32InputHost* g_host = nullptr;

std::int64_t QpcNowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart * 1000000 / freq.QuadPart);
}

// 修饰键状态从系统读，而不是自己攒一份：钩子看到的顺序与真实按下顺序并不总是一致，
// 自己记账迟早会在「按住 Alt 再按 Ctrl」这类组合上错一位。
std::uint32_t CurrentModifierState() {
    std::uint32_t state = 0;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        state |= core::kModControl;
    }
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        state |= core::kModAlt;
    }
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        state |= core::kModShift;
    }
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) {
        state |= core::kModWin;
    }
    return state;
}

}

UINT ToNativeModifiers(std::uint32_t modifiers) {
    UINT native = MOD_NOREPEAT;   // 自动重复不算第二次触发，产品里没人要那个。
    if ((modifiers & core::kModAlt) != 0) {
        native |= MOD_ALT;
    }
    if ((modifiers & core::kModControl) != 0) {
        native |= MOD_CONTROL;
    }
    if ((modifiers & core::kModShift) != 0) {
        native |= MOD_SHIFT;
    }
    if ((modifiers & core::kModWin) != 0) {
        native |= MOD_WIN;
    }
    return native;
}

UINT ToNativeVirtualKey(core::KeySymbol key) {
    const auto value = static_cast<std::int32_t>(key);

    // 字母与数字的虚拟键码就是它们的 ASCII 值（A=0x41、0=0x30），F1..F24 = 0x70..0x87。
    // 这三段是 core 的编号区间直接对上原生区间的唯一地方，改动要同时改测试。
    if (value >= 1 && value <= 26) {
        return static_cast<UINT>(0x41 + (value - 1));
    }
    if (value >= 27 && value <= 36) {
        return static_cast<UINT>(0x30 + (value - 27));
    }
    if (value >= 37 && value <= 60) {
        return static_cast<UINT>(0x70 + (value - 37));
    }

    switch (key) {
    case core::KeySymbol::Escape: return VK_ESCAPE;
    case core::KeySymbol::Enter: return VK_RETURN;
    case core::KeySymbol::Space: return VK_SPACE;
    case core::KeySymbol::Tab: return VK_TAB;
    case core::KeySymbol::Backspace: return VK_BACK;
    case core::KeySymbol::Insert: return VK_INSERT;
    case core::KeySymbol::Delete: return VK_DELETE;
    case core::KeySymbol::Home: return VK_HOME;
    case core::KeySymbol::End: return VK_END;
    case core::KeySymbol::PageUp: return VK_PRIOR;
    case core::KeySymbol::PageDown: return VK_NEXT;
    case core::KeySymbol::Up: return VK_UP;
    case core::KeySymbol::Down: return VK_DOWN;
    case core::KeySymbol::Left: return VK_LEFT;
    case core::KeySymbol::Right: return VK_RIGHT;
    // 0x2C：PrintScreen 的原生码是 VK_SNAPSHOT，别按字母表顺序猜。
    case core::KeySymbol::PrintScreen: return VK_SNAPSHOT;
    default: return 0;
    }
}

core::KeySymbol FromNativeVirtualKey(DWORD virtualKey) {
    if (virtualKey >= 0x41 && virtualKey <= 0x5A) {
        return static_cast<core::KeySymbol>(1 + static_cast<int>(virtualKey - 0x41));
    }
    if (virtualKey >= 0x30 && virtualKey <= 0x39) {
        return static_cast<core::KeySymbol>(27 + static_cast<int>(virtualKey - 0x30));
    }
    if (virtualKey >= 0x70 && virtualKey <= 0x87) {
        return static_cast<core::KeySymbol>(37 + static_cast<int>(virtualKey - 0x70));
    }

    switch (virtualKey) {
    case VK_ESCAPE: return core::KeySymbol::Escape;
    case VK_RETURN: return core::KeySymbol::Enter;
    case VK_SPACE: return core::KeySymbol::Space;
    case VK_TAB: return core::KeySymbol::Tab;
    case VK_BACK: return core::KeySymbol::Backspace;
    case VK_INSERT: return core::KeySymbol::Insert;
    case VK_DELETE: return core::KeySymbol::Delete;
    case VK_HOME: return core::KeySymbol::Home;
    case VK_END: return core::KeySymbol::End;
    case VK_PRIOR: return core::KeySymbol::PageUp;
    case VK_NEXT: return core::KeySymbol::PageDown;
    case VK_UP: return core::KeySymbol::Up;
    case VK_DOWN: return core::KeySymbol::Down;
    case VK_LEFT: return core::KeySymbol::Left;
    case VK_RIGHT: return core::KeySymbol::Right;
    case VK_SNAPSHOT: return core::KeySymbol::PrintScreen;
    default: return core::KeySymbol::None;
    }
}

Win32InputHost::Win32InputHost() = default;

Win32InputHost::~Win32InputHost() { Close(); }

bool Win32InputHost::EnsureClassRegistered() {
    static bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Win32InputHost::WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClassName;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool Win32InputHost::Start() {
    if (window_ != nullptr) {
        return true;
    }

    EnsureClassRegistered();

    HWND window = CreateWindowExW(0, kWindowClassName, L"HaloCap input host", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (window == nullptr) {
        lastError_ = HRESULT_FROM_WIN32(GetLastError());
        errorKind_ = "hr";
        return false;
    }

    // HWND_MESSAGE 父：这个窗口从不在桌面上出现，也不会被激活——后台验证的硬要求。
    window_ = window;
    g_host = this;
    errorKind_ = "ok";
    return true;
}

void Win32InputHost::Close() {
    if (window_ == nullptr) {
        return;
    }

    HWND window = static_cast<HWND>(window_);

    for (const auto& entry : registered_) {
        UnregisterHotKey(window, entry.first);
    }
    registered_.clear();
    firedTokens_.clear();

    if (hook_ != nullptr) {
        UnhookWindowsHookEx(static_cast<HHOOK>(hook_));
        hook_ = nullptr;
    }

    DestroyWindow(window);
    window_ = nullptr;
    interception_.clear();

    if (g_host == this) {
        g_host = nullptr;
    }

    errorKind_ = "none";
}

bool Win32InputHost::RegisterHotkey(const core::HotkeySpec& spec, int token) {
    if (window_ == nullptr) {
        lastError_ = E_FAIL;
        errorKind_ = "invalid";
        return false;
    }

    if (!spec.Valid()) {
        lastError_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    const UINT virtualKey = ToNativeVirtualKey(spec.key);
    if (virtualKey == 0) {
        lastError_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    const BOOL ok = RegisterHotKey(static_cast<HWND>(window_), token,
                                   ToNativeModifiers(spec.modifiers), virtualKey);
    if (ok == 0) {
        const DWORD win32Error = GetLastError();
        lastError_ = HRESULT_FROM_WIN32(win32Error);
        // 1409：这个组合已经被别的程序（或我们自己）占住了。
        errorKind_ = win32Error == ERROR_HOTKEY_ALREADY_REGISTERED ? "inuse" : "hr";
        return false;
    }

    registered_.push_back({ token, spec });
    errorKind_ = "ok";
    return true;
}

void Win32InputHost::UnregisterHotkey(int token) {
    if (window_ == nullptr) {
        return;
    }

    UnregisterHotKey(static_cast<HWND>(window_), token);
    registered_.erase(std::remove_if(registered_.begin(), registered_.end(),
                                     [token](const auto& entry) { return entry.first == token; }),
                      registered_.end());
}

void Win32InputHost::SetInterception(const std::vector<core::HotkeySpec>& specs) {
    interception_ = specs;

    if (window_ == nullptr) {
        return;
    }

    // 每次都重新安装：低级钩子的链是**按安装顺序倒着**走的，重装才能保证我们这一份
    // 始终在最前面（不然它拦不拦得着取决于别人什么时候装的钩子）。
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(static_cast<HHOOK>(hook_));
        hook_ = nullptr;
    }

    if (interception_.empty()) {
        return;
    }

    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &Win32InputHost::HookProc,
                              GetModuleHandleW(nullptr), 0);
    if (hook_ == nullptr) {
        lastError_ = HRESULT_FROM_WIN32(GetLastError());
        errorKind_ = "hr";
    }
}

LRESULT CALLBACK Win32InputHost::HookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (g_host == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    return g_host->HandleHook(code, wParam, lParam);
}

LRESULT Win32InputHost::HandleHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(static_cast<HHOOK>(hook_), code, wParam, lParam);
    }

    const std::int64_t beginUs = QpcNowUs();
    ++hookEventsSeen_;

    const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    core::KeyEvent event{};
    event.key = FromNativeVirtualKey(data->vkCode);
    event.heldModifiers = CurrentModifierState();
    event.isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    // 虚拟键已经是按下状态，说明这是按住不放产生的重复。
    event.isRepeat = event.isKeyDown && ((GetKeyState(static_cast<int>(data->vkCode)) & 0x8000) != 0);
    event.injected = (data->flags & LLKHF_INJECTED) != 0;
    event.injectedByLowerIntegrity = (data->flags & LLKHF_LOWER_IL_INJECTED) != 0;

    // 产品路径一律走 core 的策略（注入的事件永远放过）。量测时另开一条：探针只能造注入
    // 事件，而「钩子返回值拦不拦得住」必须有人按下来验证，所以给它一个绕开注入豁免的开关。
    core::SwallowDecision decision = core::SwallowDecision::Pass;
    if (interceptionIncludesInjected_) {
        decision = event.isKeyDown && !event.isRepeat ? MatchOnly(event)
                                                      : core::SwallowDecision::Pass;
    } else {
        decision = core::DecideSwallow(event, interception_);
    }

    const std::int64_t costUs = QpcNowUs() - beginUs;
    if (costUs > hookMaxUsPerEvent_) {
        hookMaxUsPerEvent_ = costUs;
    }

    // 钩子里不能做阻塞调用：超过系统的 LowLevelHooksTimeout 之后钩子会被直接跳过，
    // 表现是「偶尔拦不住」，而日志里什么都看不到。
    if (decision == core::SwallowDecision::Swallow) {
        ++swallowed_;
        // 关键：键被吞掉之后系统就不会再产生 WM_HOTKEY 了。单键热键恰恰要靠吞掉它
        // 才能独占（设置页那句「单键会独占该键」），所以动作必须在这里补发——
        // 否则就是「注册成功、按了没反应」。
        for (const auto& entry : registered_) {
            if (core::MatchesHotkey(event, entry.second)) {
                firedTokens_.push_back(entry.first);
                ++hotkeyDeliveries_;
                lastHookedToken_ = entry.first;
                lastHookedMs_ = GetTickCount64();
                break;
            }
        }
        return 1;
    }

    return CallNextHookEx(static_cast<HHOOK>(hook_), code, wParam, lParam);
}

core::SwallowDecision Win32InputHost::MatchOnly(const core::KeyEvent& event) {
    for (const auto& spec : interception_) {
        if (core::MatchesHotkey(event, spec)) {
            return core::SwallowDecision::Swallow;
        }
    }
    return core::SwallowDecision::Pass;
}

LRESULT CALLBACK Win32InputHost::WindowProc(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    auto* self = reinterpret_cast<Win32InputHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_HOTKEY && self != nullptr) {
        const int token = static_cast<int>(wParam);
        // 刚由钩子补发过的同一把就别再来一次。
        if (token == self->lastHookedToken_ &&
            GetTickCount64() - self->lastHookedMs_ < 200) {
            return 0;
        }
        self->firedTokens_.push_back(token);
        ++self->hotkeyDeliveries_;
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void Win32InputHost::PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool Win32InputHost::PollHotkey(int& tokenOut) {
    if (firedTokens_.empty()) {
        return false;
    }
    tokenOut = firedTokens_.front();
    firedTokens_.pop_front();
    return true;
}

std::uintptr_t Win32InputHost::SuspendIme(core::NativeHandle window) {
    if (window == nullptr) {
        return 0;
    }

    // IME 的关联状态是按线程的：必须在拥有这个窗口的线程上调，否则改了也不生效。
    // 返回值就是「之前关联的上下文」，恢复时原样传回（1.x 的教训：不还原会让输入法哑掉）。
    HIMC previous = ImmAssociateContext(static_cast<HWND>(window), nullptr);
    return reinterpret_cast<std::uintptr_t>(previous);
}

void Win32InputHost::ResumeIme(core::NativeHandle window, std::uintptr_t previousContext) {
    if (window == nullptr) {
        return;
    }
    ImmAssociateContext(static_cast<HWND>(window), reinterpret_cast<HIMC>(previousContext));
}

}
