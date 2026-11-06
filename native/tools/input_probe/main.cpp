// 输入 seam 的一次跑通：热键注册与投递、单键热键的修饰键规则、拦截到底拦不拦得住、
// IME 断开与恢复。全程用 message-only 窗口，不显示任何东西、不激活、不抢前台。
//
// 按键是用 SendInput 注入的，只用 F13–F17 这几个基本没人绑定的功能键；组合都带 Ctrl+Alt。
// 这一步会让当前获得焦点的程序收到一次功能键按下（实践中无副作用），除此之外不产生任何可见影响。
#include "win32_input_host.h"

#include "input_host.h"

#include <imm.h>
#include <psapi.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;
using halocap::platform::win32::Win32InputHost;

// 观察者钩子：先装，因此它在拦截钩子之后被调用（钩子链按安装的倒序走）。
// 它数的是「有多少**按下**真的往下走了」——这就是拦截是否生效的唯一证据。
// 抬起分开数：策略本来就规定 up 一律放过（吞掉 up 会让前台卡在修饰键状态里），
// 把 up 算进「没拦住」是判据写错，不是实现错——第一版就是这么误报的。
int g_observerDownEvents = 0;
int g_observerUpEvents = 0;
DWORD g_observerWatchVk = 0;
HHOOK g_observerHook = nullptr;

LRESULT CALLBACK ObserverProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_observerWatchVk != 0) {
        const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (data->vkCode == g_observerWatchVk) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                ++g_observerDownEvents;
            } else {
                ++g_observerUpEvents;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void ResetObserverCounter() {
    g_observerDownEvents = 0;
    g_observerUpEvents = 0;
}

// 注入一个「修饰键 + 键」的组合，按下再松开。
void InjectCombo(std::uint32_t modifiers, UINT virtualKey) {
    std::vector<INPUT> inputs;
    const auto Push = [&inputs](UINT vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        inputs.push_back(in);
    };

    if (modifiers & core::kModControl) {
        Push(VK_CONTROL, true);
    }
    if (modifiers & core::kModAlt) {
        Push(VK_MENU, true);
    }
    if (modifiers & core::kModShift) {
        Push(VK_SHIFT, true);
    }
    Push(virtualKey, true);
    Push(virtualKey, false);
    if (modifiers & core::kModShift) {
        Push(VK_SHIFT, false);
    }
    if (modifiers & core::kModAlt) {
        Push(VK_MENU, false);
    }
    if (modifiers & core::kModControl) {
        Push(VK_CONTROL, false);
    }

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

double PrivateMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

// 注入之后要等消息：WM_HOTKEY 是异步投递的。
int CollectTokens(Win32InputHost& host, std::vector<int>& collected, int waitMs = 300) {
    collected.clear();
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(waitMs);
    while (GetTickCount64() < deadline) {
        host.PumpMessages();
        int token = 0;
        while (host.PollHotkey(token)) {
            collected.push_back(token);
        }
        if (!collected.empty()) {
            break;
        }
        Sleep(5);
    }
    return static_cast<int>(collected.size());
}

// 排空热键队列：不清就能把上一次注入的投递当成这一次的结果——第一版的
// hotkey_still_fired 就是这么来的假数。
void Drain(Win32InputHost& host) {
    host.PumpMessages();
    int token = 0;
    while (host.PollHotkey(token)) {
    }
}

int g_topLevelWindowsOfThisProcess = 0;

BOOL CALLBACK CountTopLevel(HWND window, LPARAM) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId()) {
        ++g_topLevelWindowsOfThisProcess;
    }
    return TRUE;
}

bool Contains(const std::vector<int>& tokens, int token) {
    for (const int value : tokens) {
        if (value == token) {
            return true;
        }
    }
    return false;
}

core::HotkeySpec Spec(const char* text) {
    core::HotkeySpec spec{};
    core::ParseHotkeyText(text, spec);
    return spec;
}

}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    int failures = 0;
    const double memoryStartMb = PrivateMemoryMb();

    Win32InputHost host;
    if (!host.Start()) {
        std::printf("START=FAILED kind=%s code=0x%08lX\n", host.LastErrorKind(),
                    static_cast<unsigned long>(host.LastErrorCode()));
        return 2;
    }
    if (!host.Start()) {
        std::printf("FAIL 第二次 Start 应当是幂等的\n");
        ++failures;
    }

    HWND window = static_cast<HWND>(host.Window());
    // 这四个候选值本身就是要记的事实：Win11 26300 上 message-only 窗口 GetParent 给 0、
    // GA_PARENT 给系统每线程一个的隐藏父窗口，所以「== HWND_MESSAGE」那种判据根本不成立。
    // 真正要的性質是「桌面上没有属于本进程的顶层窗口」，这个可以直接数出来。
    g_topLevelWindowsOfThisProcess = 0;
    EnumWindows(&CountTopLevel, 0);
    std::printf("WINDOW HWND_MESSAGE=%p parent=%p ancestor_parent=%p ancestor_root=%p visible=%d "
                "process_top_level_windows=%d\n",
                static_cast<void*>(HWND_MESSAGE), static_cast<void*>(GetParent(window)),
                static_cast<void*>(GetAncestor(window, GA_PARENT)),
                static_cast<void*>(GetAncestor(window, GA_ROOT)), IsWindowVisible(window),
                g_topLevelWindowsOfThisProcess);

    if (g_topLevelWindowsOfThisProcess != 0 || IsWindowVisible(window) != 0) {
        std::printf("FAIL 输入宿主不该在桌面上留下任何顶层窗口\n");
        ++failures;
    }

    // 注册五把：三把带 Ctrl+Alt，两把裸功能键（裸键才是 1.x 圆盘热键的形状）。
    struct Entry {
        const char* text;
        int token;
    };
    const Entry entries[] = {
        { "Ctrl+Alt+F13", 11 },
        { "Ctrl+Alt+F14", 12 },
        { "Ctrl+Alt+F15", 13 },
        { "F16", 14 },
        { "F17", 15 },
    };

    int registered = 0;
    for (const auto& entry : entries) {
        const core::HotkeySpec spec = Spec(entry.text);
        const bool ok = host.RegisterHotkey(spec, entry.token);
        std::printf("REGISTER[%s] ok=%d kind=%s code=0x%08lX\n", entry.text, ok ? 1 : 0,
                    host.LastErrorKind(), static_cast<unsigned long>(host.LastErrorCode()));
        if (ok) {
            ++registered;
        }
    }

    // 重复注册同一把：系统会拒，错误分类必须是 inuse，产品才可能提示「这键被别的程序占了」。
    {
        const bool again = host.RegisterHotkey(Spec("Ctrl+Alt+F13"), 99);
        std::printf("DUPLICATE_REGISTER ok=%d kind=%s EXPECT=inuse\n", again ? 1 : 0,
                    host.LastErrorKind());
        if (again || std::string_view(host.LastErrorKind()) != "inuse") {
            ++failures;
        }
    }

    // 非法 spec 要在入口就拒掉。
    {
        core::HotkeySpec invalid{};
        const bool ok = host.RegisterHotkey(invalid, 98);
        std::printf("INVALID_REGISTER ok=%d kind=%s EXPECT=invalid\n", ok ? 1 : 0,
                    host.LastErrorKind());
        if (ok || std::string_view(host.LastErrorKind()) != "invalid") {
            ++failures;
        }
    }

    // 投递：每把注入一次，看 WM_HOTKEY 有没有回来。keyText 单独给——上一版把说明文字
    // 当成热键文本去解析，解析失败就得到 vk=0，注入个不存在的键，「没触发」那条就白过了。
    struct DeliveryCase {
        const char* label;
        const char* keyText;
        int token;
        std::uint32_t modifiers;
        bool expectFired;
    };
    const DeliveryCase deliveries[] = {
        { "Ctrl+Alt+F13", "Ctrl+Alt+F13", 11, core::kModControl | core::kModAlt, true },
        { "Ctrl+Alt+F14", "Ctrl+Alt+F14", 12, core::kModControl | core::kModAlt, true },
        { "Ctrl+Alt+F15", "Ctrl+Alt+F15", 13, core::kModControl | core::kModAlt, true },
        { "F16 裸按", "F16", 14, 0, true },
        // 1.x 的老坑：裸键热键不能被「按住 Ctrl 的同一把键」触发。
        { "Ctrl+F16 对 F16 热键", "F16", 14, core::kModControl, false },
    };

    for (const auto& item : deliveries) {
        const UINT vk = halocap::platform::win32::ToNativeVirtualKey(Spec(item.keyText).key);
        if (vk == 0) {
            std::printf("FAIL 用例的键名解析不出来：%s\n", item.keyText);
            ++failures;
            continue;
        }

        Drain(host);
        InjectCombo(item.modifiers, vk);

        std::vector<int> tokens;
        CollectTokens(host, tokens);
        const bool fired = Contains(tokens, item.token);
        std::printf("DELIVERY[%s] fired=%d tokens=%d EXPECT_FIRED=%d\n", item.label, fired ? 1 : 0,
                    static_cast<int>(tokens.size()), item.expectFired ? 1 : 0);
        if (fired != item.expectFired) {
            ++failures;
        }
    }

    // 拦截：观察者钩子先装，然后才让宿主装它的拦截钩子——链序反过来就测不出东西了。
    g_observerHook = SetWindowsHookExW(WH_KEYBOARD_LL, &ObserverProc, GetModuleHandleW(nullptr), 0);
    std::printf("OBSERVER_HOOK installed=%d\n", g_observerHook != nullptr ? 1 : 0);
    if (g_observerHook == nullptr) {
        std::printf("FAIL 观察者钩子装不上，拦截这一节测不了\n");
        ++failures;
    } else {
        const core::HotkeySpec combo = Spec("Ctrl+Alt+F14");
        g_observerWatchVk = halocap::platform::win32::ToNativeVirtualKey(combo.key);

        // (a) 产品策略：注入的事件一律放过，观察者必须看到那一次按下。
        host.SetInterception({ combo });
        ResetObserverCounter();
        Drain(host);
        InjectCombo(core::kModControl | core::kModAlt, g_observerWatchVk);
        host.PumpMessages();
        std::printf("SWALLOW_POLICY injected_down_passes=%d up_seen=%d host_seen=%lld swallowed=%lld\n",
                    g_observerDownEvents, g_observerUpEvents,
                    static_cast<long long>(host.HookEventsSeen()),
                    static_cast<long long>(host.SwallowedCount()));
        if (g_observerDownEvents == 0) {
            std::printf("FAIL 注入的按键不该被产品路径拦掉\n");
            ++failures;
        }

        // (b) 量测开关：允许拦注入的事件，观察者就必须看不到那一次**按下**；
        //     抬起仍然会放过，这是策略的一部分，不是漏拦。
        host.SetInterceptionIncludesInjected(true);
        host.SetInterception({ combo });   // 重装，拦截钩子重新排在链首
        const std::int64_t swallowedBefore = host.SwallowedCount();
        ResetObserverCounter();
        Drain(host);
        InjectCombo(core::kModControl | core::kModAlt, g_observerWatchVk);
        host.PumpMessages();

        std::vector<int> tokensAfterSwallow;
        CollectTokens(host, tokensAfterSwallow, 150);
        std::printf("SWALLOW_MECHANISM observer_down=%d observer_up=%d swallowed_delta=%lld "
                    "hotkey_still_fired=%d\n",
                    g_observerDownEvents, g_observerUpEvents,
                    static_cast<long long>(host.SwallowedCount() - swallowedBefore),
                    Contains(tokensAfterSwallow, 12) ? 1 : 0);
        if (g_observerDownEvents != 0) {
            std::printf("FAIL 拦截钩子没有把按下挡住\n");
            ++failures;
        }
        if (host.SwallowedCount() <= swallowedBefore) {
            std::printf("FAIL 拦截计数没有增加\n");
            ++failures;
        }

        host.SetInterceptionIncludesInjected(false);
        host.SetInterception({});
    }

    std::printf("HOST counters events=%lld deliveries=%lld max_us_per_event=%lld\n",
                static_cast<long long>(host.HookEventsSeen()),
                static_cast<long long>(host.HotkeyDeliveries()),
                static_cast<long long>(host.HookMaxUsPerEvent()));

    // IME：`ImmAssociateContext` 的返回值是「之前关联的那个上下文」，所以断开时拿到的句柄
    // 必须正好是我们挂上去的那一个，恢复之后再探一次也必须还是它。
    // 判据不用 ImmGetContext 的返回值：没有关联时它会给出系统默认上下文，比不出东西。
    {
        const HIMC systemPrevious = ImmAssociateContext(window, nullptr);
        const HIMC mine = ImmCreateContext();
        ImmAssociateContext(window, mine);

        const std::uintptr_t suspendToken = host.SuspendIme(window);
        const bool suspendReturnsOurs = suspendToken == reinterpret_cast<std::uintptr_t>(mine);

        host.ResumeIme(window, suspendToken);

        // 再探一次：探完顺手把它挂回去，保持原状。
        const HIMC afterResume = ImmAssociateContext(window, mine);
        const bool resumeRestored = afterResume == mine;

        ImmAssociateContext(window, systemPrevious);
        ImmDestroyContext(mine);

        std::printf("IME suspend_returns_previous=%d (token=%p ours=%p) resume_restored=%d "
                    "system_previous=%p\n",
                    suspendReturnsOurs ? 1 : 0, reinterpret_cast<void*>(suspendToken),
                    static_cast<void*>(mine), resumeRestored ? 1 : 0,
                    static_cast<void*>(systemPrevious));

        if (!suspendReturnsOurs || !resumeRestored) {
            std::printf("FAIL IME 的断开/恢复没有原样带回上下文\n");
            ++failures;
        }
    }

    // 收尾：Close 之后不能再有排队的热键。
    host.Close();
    int leakedToken = 0;
    const bool emptyAfterClose = !host.PollHotkey(leakedToken);
    std::printf("CLOSED poll_empty=%d\n", emptyAfterClose ? 1 : 0);
    if (!emptyAfterClose) {
        ++failures;
    }

    if (g_observerHook != nullptr) {
        UnhookWindowsHookEx(g_observerHook);
        g_observerHook = nullptr;
    }

    std::printf("REGISTRATIONS=%d/%d PRIVATE_MEMORY_MB start=%.0f end=%.0f\n", registered,
                static_cast<int>(sizeof(entries) / sizeof(entries[0])), memoryStartMb,
                PrivateMemoryMb());

    if (failures != 0) {
        std::printf("OUTCOME=FAILED checks=%d\n", failures);
        return 1;
    }

    std::printf("OUTCOME=OK\n");
    return 0;
}
