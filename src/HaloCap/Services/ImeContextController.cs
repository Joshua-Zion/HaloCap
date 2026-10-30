using System;
using HaloCap.Interop;

namespace HaloCap.Services;

/// <summary>
/// 截图/贴图这类非文本窗口默认解绑 IMM 输入上下文，让单键快捷键先于中文输入法生效；
/// 只有文字工具编辑时才重新关联原上下文。
/// </summary>
internal sealed class ImeContextController : IDisposable
{
    private readonly IntPtr _window;
    private IntPtr _context;
    private bool _created;
    private bool _detached;

    public ImeContextController(IntPtr window)
    {
        _window = window;

        if (_window != IntPtr.Zero)
        {
            Detach(cancelComposition: false);
        }
    }

    public void Attach()
    {
        if (_window == IntPtr.Zero || !_detached)
        {
            return;
        }

        if (_context == IntPtr.Zero)
        {
            _context = NativeMethods.ImmCreateContext();
            _created = _context != IntPtr.Zero;
        }

        if (_context != IntPtr.Zero)
        {
            NativeMethods.ImmAssociateContext(_window, _context);
        }

        _detached = false;
    }

    public void Detach(bool cancelComposition)
    {
        if (_window == IntPtr.Zero || _detached)
        {
            return;
        }

        if (cancelComposition)
        {
            var active = NativeMethods.ImmGetContext(_window);

            if (active != IntPtr.Zero)
            {
                NativeMethods.ImmNotifyIME(
                    active,
                    NativeMethods.NI_COMPOSITIONSTR,
                    NativeMethods.CPS_CANCEL,
                    0);
                NativeMethods.ImmReleaseContext(_window, active);
            }
        }

        var previous = NativeMethods.ImmAssociateContext(_window, IntPtr.Zero);

        if (previous != IntPtr.Zero)
        {
            _context = previous;
        }

        _detached = true;
    }

    public void Dispose()
    {
        if (_window == IntPtr.Zero)
        {
            return;
        }

        if (!_detached)
        {
            NativeMethods.ImmAssociateContext(_window, IntPtr.Zero);
            _detached = true;
        }

        if (_created && _context != IntPtr.Zero)
        {
            NativeMethods.ImmDestroyContext(_context);
            _context = IntPtr.Zero;
            _created = false;
            return;
        }

        if (_context != IntPtr.Zero)
        {
            NativeMethods.ImmAssociateContext(_window, _context);
        }
    }
}
