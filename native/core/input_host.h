#pragma once

#include "surface_window.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 8 个 seam 里的 IInputHost（§3.2）：全局热键、低级拦截、IME 生命周期。
//
// 按键用一套自己的编号（KeySymbol）而不是 Win32 的 VK：VK_* 是平台常量，core 里出现它们
// 就等于把边界挪到了头文件外面。映射表在 platform/win32 里，而「映射对不对」不由单测保证，
// 由 input_probe 真注册真按一次来证明——这条区别要记着，否则很容易写出一套自证的假数字。

// 全部显式声明：只写区间端点的话，调用方就没法说 KeySymbol::F13，而编号的连续性由
// input_policy 里的名字表生成、由 input_policy_tests 逐个键名往回验证。
enum class KeySymbol : std::int32_t {
    None = 0,

    A = 1, B = 2, C = 3, D = 4, E = 5, F = 6, G = 7, H = 8, I = 9, J = 10, K = 11, L = 12,
    M = 13, N = 14, O = 15, P = 16, Q = 17, R = 18, S = 19, T = 20, U = 21, V = 22, W = 23,
    X = 24, Y = 25, Z = 26,

    Num0 = 27, Num1 = 28, Num2 = 29, Num3 = 30, Num4 = 31, Num5 = 32, Num6 = 33, Num7 = 34,
    Num8 = 35, Num9 = 36,

    F1 = 37, F2 = 38, F3 = 39, F4 = 40, F5 = 41, F6 = 42, F7 = 43, F8 = 44, F9 = 45,
    F10 = 46, F11 = 47, F12 = 48, F13 = 49, F14 = 50, F15 = 51, F16 = 52, F17 = 53,
    F18 = 54, F19 = 55, F20 = 56, F21 = 57, F22 = 58, F23 = 59, F24 = 60,

    Escape = 61,
    Enter = 62,
    Space = 63,
    Tab = 64,
    Backspace = 65,
    Insert = 66,
    Delete = 67,
    Home = 68,
    End = 69,
    PageUp = 70,
    PageDown = 71,
    Up = 72,
    Down = 73,
    Left = 74,
    Right = 75,
    PrintScreen = 76,

    Last = PrintScreen,
};

// 修饰键。数值与 Win32 的 MOD_* 恰好相同，但平台侧仍然逐项翻译而不是直接 cast：
// 巧合不能当依赖，哪天加一个 core 独有的位就会串到系统常量上。
enum Modifiers : std::uint32_t {
    kModAlt = 1u << 0,
    kModControl = 1u << 1,
    kModShift = 1u << 2,
    kModWin = 1u << 3,

    // 只影响注册请求（不要把自动重复也算成一次触发），不参与按键状态的匹配。
    kModNoRepeat = 1u << 4,
};

// 参与状态比较的那四位。
constexpr std::uint32_t kStateModifiers = kModAlt | kModControl | kModShift | kModWin;

struct HotkeySpec {
    KeySymbol key = KeySymbol::None;
    std::uint32_t modifiers = 0;

    bool operator==(const HotkeySpec& other) const {
        return key == other.key && modifiers == other.modifiers;
    }

    bool Valid() const { return key != KeySymbol::None; }
};

// 一次按键事件，从低级钩子的原始数据折过来。heldModifiers 只放四位状态修饰键。
struct KeyEvent {
    KeySymbol key = KeySymbol::None;
    std::uint32_t heldModifiers = 0;
    bool isKeyDown = false;
    bool isRepeat = false;          // 同一键还没抬起就又来一次 down
    bool injected = false;          // 别的进程注入的
    bool injectedByLowerIntegrity = false;
};

// 设置里存的是文本（"Ctrl+Alt+K"、"F13"），解析与格式化必须留在 core：
// 1.x 的两套实现（写与读）不一致过，导致某些组合显示出来按不响。
bool ParseHotkeyText(std::string_view text, HotkeySpec& out);
std::string FormatHotkeyText(const HotkeySpec& spec);

const char* KeySymbolName(KeySymbol key);
bool KeySymbolFromName(std::string_view name, KeySymbol& out);

// 精确匹配：多按一个修饰键就不算命中。单键热键（1.x 的圆盘键就是这么用的）最容易在这里
// 出事——按住 Ctrl 的 F13 不能被当成裸 F13 触发。
bool MatchesHotkey(const KeyEvent& event, const HotkeySpec& spec);

enum class SwallowDecision {
    Pass,      // 放过去，让当前获得焦点的程序照常收到
    Swallow,   // 吞掉：热键已经被我们用了，不该再让前台输入一次
};

// 拦截只在 down 上做，且只在精确命中时做。up 一定要放过去，否则前台程序会等到一个
// 永不来的抬起、卡在修饰键状态里；重复的 down 不放过去，按住不放就是一次都不触发。
SwallowDecision DecideSwallow(const KeyEvent& event, const std::vector<HotkeySpec>& specs);

// 钩子回调里能用的判断：不能有任何分配，也不能调用会阻塞的东西。
// 这条限制来自 1.x 的教训——低级钩子超时（LowLevelHooksTimeout）之后系统会直接跳过钩子，
// 表现出来就是「偶尔拦不住」，很难查。
class IInputHost {
public:
    virtual ~IInputHost() = default;

    // 建一个只收消息、从不上屏的窗口（HWND_MESSAGE 父），热键的 WM_HOTKEY 就投递到那里。
    virtual bool Start() = 0;

    virtual void Close() = 0;

    // token 是调用方给的一个整数（1.x 用「动作枚举」），平台只负责把它带回来。
    // 同一个组合被别的程序占着时返回 false，LastErrorKind == "inuse"。
    virtual bool RegisterHotkey(const HotkeySpec& spec, int token) = 0;

    virtual void UnregisterHotkey(int token) = 0;

    // 要吞掉的那些组合（通常就是已注册的热键本身）。传空表就是完全不拦。
    virtual void SetInterception(const std::vector<HotkeySpec>& specs) = 0;

    // 需要 Pump 才有 WM_HOTKEY：消息循环归谁，看这个 seam 挂在哪个宿主里。
    virtual void PumpMessages() = 0;

    // 取出一个已经到达的热键触发；没有就返回 false。
    virtual bool PollHotkey(int& tokenOut) = 0;

    // 覆盖层/截图窗口必须在显示前把 IME 断开，否则中文输入法会把选区键吃掉（1.x 的
    // 那条教训）。返回值是「之前关联的上下文」，恢复时原样传回去；0 表示原本没有。
    virtual std::uintptr_t SuspendIme(NativeHandle window) = 0;
    virtual void ResumeIme(NativeHandle window, std::uintptr_t previousContext) = 0;

    // "ok" / "invalid" / "inuse" / "denied" / "hr" / "none"
    virtual const char* LastErrorKind() const = 0;
    virtual std::int32_t LastErrorCode() const = 0;
};

}
