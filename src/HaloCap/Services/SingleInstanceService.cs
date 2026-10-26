using System;
using System.Threading;
using HaloCap.Diagnostics;

namespace HaloCap.Services;

internal sealed class SingleInstanceService : IDisposable
{
    private const string MutexName = @"Local\HaloCap.SingleInstance";
    private const string ActivationEventName = @"Local\HaloCap.Activate";

    private Mutex? _mutex;
    private EventWaitHandle? _activationEvent;
    private Thread? _listenerThread;
    private volatile bool _stopping;
    private bool _ownsMutex;

    public event EventHandler? ActivationRequested;

    public bool TryAcquire()
    {
        _mutex = new Mutex(true, MutexName, out var createdNew);
        if (!createdNew)
        {
            _mutex.Dispose();
            _mutex = null;
            return false;
        }

        _ownsMutex = true;
        _activationEvent = new EventWaitHandle(
            false,
            EventResetMode.AutoReset,
            ActivationEventName);
        return true;
    }

    public void StartListening()
    {
        if (_activationEvent is null || _listenerThread is not null)
        {
            return;
        }

        _listenerThread = new Thread(ListenLoop)
        {
            IsBackground = true,
            Name = "HaloCap.SingleInstance"
        };
        _listenerThread.Start();
    }

    public static bool NotifyExistingInstance()
    {
        try
        {
            using var activationEvent = EventWaitHandle.OpenExisting(ActivationEventName);
            activationEvent.Set();
            return true;
        }
        catch (Exception ex)
        {
            Log.Error("通知已有 HaloCap 实例失败", ex);
            return false;
        }
    }

    private void ListenLoop()
    {
        while (!_stopping)
        {
            try
            {
                _activationEvent!.WaitOne();

                if (!_stopping)
                {
                    ActivationRequested?.Invoke(this, EventArgs.Empty);
                }
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (Exception ex)
            {
                Log.Error("等待单实例激活信号失败", ex);
                break;
            }
        }
    }

    public void Dispose()
    {
        _stopping = true;

        try
        {
            _activationEvent?.Set();
        }
        catch
        {
            // 正在销毁时忽略。
        }

        if (_listenerThread is { IsAlive: true })
        {
            _listenerThread.Join(500);
        }

        _activationEvent?.Dispose();
        _activationEvent = null;

        if (_ownsMutex)
        {
            try
            {
                _mutex?.ReleaseMutex();
            }
            catch (Exception ex)
            {
                Log.Error("释放单实例互斥量失败", ex);
            }
            finally
            {
                _ownsMutex = false;
            }
        }

        _mutex?.Dispose();
        _mutex = null;
    }
}
