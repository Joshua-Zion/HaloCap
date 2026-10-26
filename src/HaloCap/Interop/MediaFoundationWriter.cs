using System;
using System.Runtime.InteropServices;

namespace HaloCap.Interop
{
    // Kept C# 5 compatible so the interop can be compiled and tested independently.
    internal sealed class MediaFoundationWriter : IDisposable
    {
        private const int MfVersion = 0x00020070;
        private const int MfStartupFull = 0;

        private static readonly Guid MfMediaTypeVideo =
            new Guid("73646976-0000-0010-8000-00AA00389B71");

        private static readonly Guid MfVideoFormatH264 =
            new Guid("34363248-0000-0010-8000-00AA00389B71");

        private static readonly Guid MfVideoFormatRgb32 =
            new Guid("00000016-0000-0010-8000-00AA00389B71");

        private static readonly Guid MfMtMajorType =
            new Guid("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");

        private static readonly Guid MfMtSubtype =
            new Guid("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");

        private static readonly Guid MfMtAvgBitrate =
            new Guid("20332624-fb0d-4d9e-bd0d-cbf6786c102e");

        private static readonly Guid MfMtInterlaceMode =
            new Guid("e2724bb8-e676-4806-b4b2-a8d6efb44ccd");

        private static readonly Guid MfMtFrameSize =
            new Guid("1652c33d-d6b2-4012-b834-72030849a37d");

        private static readonly Guid MfMtFrameRate =
            new Guid("c459a2e8-3d2c-4e44-b132-fee5156c7bb0");

        private static readonly Guid MfMtPixelAspectRatio =
            new Guid("c6376a1e-8d0a-4027-be45-6d9a0ad39bb6");

        private static readonly Guid MfMtMpeg2Profile =
            new Guid("ad76a80b-2d5c-4e0b-b375-64e520137036");

        private static readonly Guid MfMtDefaultStride =
            new Guid("644b4e48-1e02-4516-b0eb-c01ca9d49ac6");

        private static readonly Guid MfMtAllSamplesIndependent =
            new Guid("c9173739-5e56-461c-b713-46fb995cb95f");

        private const uint MfVideoInterlaceProgressive = 2;
        private const uint H264MainProfile = 77;

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate uint ReleaseDelegate(IntPtr self);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetUInt32Delegate(IntPtr self, ref Guid key, uint value);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetUInt64Delegate(IntPtr self, ref Guid key, ulong value);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetGuidDelegate(IntPtr self, ref Guid key, ref Guid value);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int AddStreamDelegate(IntPtr self, IntPtr targetType, out int streamIndex);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetInputMediaTypeDelegate(
            IntPtr self,
            int streamIndex,
            IntPtr inputType,
            IntPtr encodingParameters);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int NoArgDelegate(IntPtr self);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int WriteSampleDelegate(IntPtr self, int streamIndex, IntPtr sample);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int LockDelegate(
            IntPtr self,
            out IntPtr buffer,
            out uint maxLength,
            out uint currentLength);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetCurrentLengthDelegate(IntPtr self, uint length);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int SetSampleTimeDelegate(IntPtr self, long time);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int AddBufferDelegate(IntPtr self, IntPtr buffer);

        private IntPtr _sinkWriter;
        private IntPtr _outputType;
        private IntPtr _inputType;
        private int _streamIndex;
        private bool _writingStarted;
        private bool _finalized;
        private bool _disposed;
        private bool _mfStarted;
        private readonly int _width;
        private readonly int _height;
        private readonly int _frameBytes;

        public MediaFoundationWriter(string path, int width, int height, int framesPerSecond, int bitrate)
        {
            if (string.IsNullOrEmpty(path))
            {
                throw new ArgumentException("录制文件路径不能为空。", "path");
            }

            if (width < 2 || height < 2)
            {
                throw new ArgumentException("录制宽高必须至少为 2 像素。");
            }

            if ((width & 1) != 0 || (height & 1) != 0)
            {
                throw new ArgumentException("H.264 录制宽高必须为偶数。");
            }

            if (framesPerSecond <= 0)
            {
                throw new ArgumentException("帧率必须大于零。");
            }

            _width = width;
            _height = height;
            _frameBytes = checked(width * height * 4);

            var hr = NativeMf.MFStartup(MfVersion, MfStartupFull);
            Check(hr, "初始化 Media Foundation");
            _mfStarted = true;

            try
            {
                _outputType = CreateVideoType(
                    MfVideoFormatH264,
                    width,
                    height,
                    framesPerSecond,
                    bitrate,
                    true);
                _inputType = CreateVideoType(
                    MfVideoFormatRgb32,
                    width,
                    height,
                    framesPerSecond,
                    bitrate,
                    false);

                Check(
                    NativeMf.MFCreateSinkWriterFromURL(path, IntPtr.Zero, IntPtr.Zero, out _sinkWriter),
                    "创建 MP4 写入器");

                Check(
                    Invoke<AddStreamDelegate>(_sinkWriter, 3)(_sinkWriter, _outputType, out _streamIndex),
                    "添加 H.264 视频流");

                Check(
                    Invoke<SetInputMediaTypeDelegate>(_sinkWriter, 4)(
                        _sinkWriter,
                        _streamIndex,
                        _inputType,
                        IntPtr.Zero),
                    "设置视频输入格式");

                Check(Invoke<NoArgDelegate>(_sinkWriter, 5)(_sinkWriter), "开始写入");
                _writingStarted = true;
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        public void WriteFrame(byte[] bgraTopDown, long timestamp, long duration)
        {
            if (_disposed || _finalized || !_writingStarted || _sinkWriter == IntPtr.Zero)
            {
                throw new InvalidOperationException("录制写入器已经结束。");
            }

            if (bgraTopDown == null || bgraTopDown.Length < _frameBytes)
            {
                throw new ArgumentException("视频帧缓冲区太小。", "bgraTopDown");
            }

            if (timestamp < 0 || duration <= 0)
            {
                throw new ArgumentException("视频帧时间戳无效。");
            }

            IntPtr buffer = IntPtr.Zero;
            IntPtr sample = IntPtr.Zero;

            try
            {
                Check(
                    NativeMf.MFCreateMemoryBuffer((uint)_frameBytes, out buffer),
                    "创建视频帧缓冲区");

                IntPtr destination;
                uint maxLength;
                uint currentLength;
                Check(
                    Invoke<LockDelegate>(buffer, 3)(buffer, out destination, out maxLength, out currentLength),
                    "锁定视频帧缓冲区");

                Marshal.Copy(bgraTopDown, 0, destination, _frameBytes);
                Check(Invoke<NoArgDelegate>(buffer, 4)(buffer), "解锁视频帧缓冲区");
                Check(
                    Invoke<SetCurrentLengthDelegate>(buffer, 6)(buffer, (uint)_frameBytes),
                    "设置视频帧长度");

                Check(NativeMf.MFCreateSample(out sample), "创建视频帧样本");
                Check(Invoke<AddBufferDelegate>(sample, 42)(sample, buffer), "添加视频帧缓冲");
                Check(Invoke<SetSampleTimeDelegate>(sample, 36)(sample, timestamp), "设置帧时间戳");
                Check(Invoke<SetSampleTimeDelegate>(sample, 38)(sample, duration), "设置帧时长");
                Check(
                    Invoke<WriteSampleDelegate>(_sinkWriter, 6)(_sinkWriter, _streamIndex, sample),
                    "写入视频帧");
            }
            finally
            {
                Release(sample);
                Release(buffer);
            }
        }

        public void Complete()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;

            try
            {
                if (!_finalized && _sinkWriter != IntPtr.Zero)
                {
                    Check(Invoke<NoArgDelegate>(_sinkWriter, 11)(_sinkWriter), "完成 MP4 文件");
                    _finalized = true;
                }
            }
            finally
            {
                ReleaseAll();
            }
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;

            try
            {
                if (!_finalized && _writingStarted && _sinkWriter != IntPtr.Zero)
                {
                    Check(Invoke<NoArgDelegate>(_sinkWriter, 11)(_sinkWriter), "完成 MP4 文件");
                    _finalized = true;
                }
            }
            catch
            {
                // 退出或异常清理时尽力保留可用文件。
            }
            finally
            {
                ReleaseAll();
            }
        }

        private static IntPtr CreateVideoType(
            Guid subtype,
            int width,
            int height,
            int framesPerSecond,
            int bitrate,
            bool forOutput)
        {
            IntPtr type;
            Check(NativeMf.MFCreateMediaType(out type), "创建媒体类型");

            try
            {
                Check(SetGuid(type, MfMtMajorType, MfMediaTypeVideo), "设置媒体主类型");
                Check(SetGuid(type, MfMtSubtype, subtype), "设置媒体子类型");
                Check(
                    SetUInt64(type, MfMtFrameSize, ((ulong)(uint)width << 32) | (uint)height),
                    "设置视频尺寸");
                Check(
                    SetUInt64(type, MfMtFrameRate, ((ulong)(uint)framesPerSecond << 32) | 1),
                    "设置视频帧率");
                Check(SetUInt32(type, MfMtInterlaceMode, MfVideoInterlaceProgressive), "设置逐行扫描");
                Check(
                    SetUInt64(type, MfMtPixelAspectRatio, (1UL << 32) | 1),
                    "设置像素宽高比");

                if (forOutput)
                {
                    Check(SetUInt32(type, MfMtAvgBitrate, (uint)Math.Max(500000, bitrate)), "设置码率");
                    Check(SetUInt32(type, MfMtMpeg2Profile, H264MainProfile), "设置 H.264 Profile");
                }
                else
                {
                    Check(SetUInt32(type, MfMtDefaultStride, (uint)(width * 4)), "设置图像步幅");
                    Check(SetUInt32(type, MfMtAllSamplesIndependent, 1), "设置独立样本");
                }

                return type;
            }
            catch
            {
                Release(type);
                throw;
            }
        }

        private static int SetUInt32(IntPtr comObject, Guid key, uint value)
        {
            return Invoke<SetUInt32Delegate>(comObject, 21)(comObject, ref key, value);
        }

        private static int SetUInt64(IntPtr comObject, Guid key, ulong value)
        {
            return Invoke<SetUInt64Delegate>(comObject, 22)(comObject, ref key, value);
        }

        private static int SetGuid(IntPtr comObject, Guid key, Guid value)
        {
            return Invoke<SetGuidDelegate>(comObject, 24)(comObject, ref key, ref value);
        }

        private static T Invoke<T>(IntPtr comObject, int slot) where T : class
        {
            if (comObject == IntPtr.Zero)
            {
                throw new InvalidOperationException("COM 对象尚未初始化。");
            }

            var vtable = Marshal.ReadIntPtr(comObject);
            var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
        }

        private static void Release(IntPtr comObject)
        {
            if (comObject == IntPtr.Zero)
            {
                return;
            }

            try
            {
                Invoke<ReleaseDelegate>(comObject, 2)(comObject);
            }
            catch
            {
                // 释放过程中的异常不应掩盖原始错误。
            }
        }

        private static void Check(int hr, string operation)
        {
            if (hr < 0)
            {
                throw new COMException(
                    operation + "失败（HRESULT 0x" + hr.ToString("X8") + "）。",
                    hr);
            }
        }

        private void ReleaseAll()
        {
            Release(_sinkWriter);
            _sinkWriter = IntPtr.Zero;

            Release(_outputType);
            _outputType = IntPtr.Zero;

            Release(_inputType);
            _inputType = IntPtr.Zero;

            _writingStarted = false;

            if (_mfStarted)
            {
                try
                {
                    NativeMf.MFShutdown();
                }
                catch
                {
                    // 忽略 Media Foundation 关闭异常。
                }

                _mfStarted = false;
            }
        }

        private static class NativeMf
        {
            [DllImport("mfplat.dll", ExactSpelling = true)]
            public static extern int MFStartup(int version, int flags);

            [DllImport("mfplat.dll", ExactSpelling = true)]
            public static extern int MFShutdown();

            [DllImport("mfplat.dll", ExactSpelling = true)]
            public static extern int MFCreateMediaType(out IntPtr mediaType);

            [DllImport("mfplat.dll", ExactSpelling = true)]
            public static extern int MFCreateMemoryBuffer(uint maxLength, out IntPtr buffer);

            [DllImport("mfplat.dll", ExactSpelling = true)]
            public static extern int MFCreateSample(out IntPtr sample);

            [DllImport("mfreadwrite.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
            public static extern int MFCreateSinkWriterFromURL(
                string outputUrl,
                IntPtr byteStream,
                IntPtr attributes,
                out IntPtr sinkWriter);
        }
    }
}
