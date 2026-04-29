using System;
using System.IO.Ports;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace UartMonitor.Serial;

public sealed class SerialReader : IDisposable
{
    private readonly object _gate = new object();
    private SerialPort? _port;
    private CancellationTokenSource? _cts;
    private Task? _readerTask;
    private Encoding _encoding = Encoding.UTF8;
    private bool _mergeLineEndings = true;
    private readonly LineEndingNormalizer _lineEndingNormalizer = new LineEndingNormalizer();

    public sealed class ChunkReceivedEventArgs : EventArgs
    {
        public ChunkReceivedEventArgs(byte[] bytes, string text)
        {
            Bytes = bytes;
            Text = text;
        }

        public byte[] Bytes { get; }
        public string Text { get; }
    }

    public event EventHandler<byte[]>? BytesReceived;
    public event EventHandler<ChunkReceivedEventArgs>? ChunkReceived;
    public event EventHandler<string>? TextReceived;
    public event EventHandler<string>? StatusChanged;
    public event EventHandler<Exception>? Error;

    public bool IsConnected
    {
        get
        {
            lock (_gate)
                return _port?.IsOpen == true;
        }
    }

    public void Connect(SerialPortOptions options)
    {
        if (IsConnected)
            return;

        _encoding = options.Encoding;
        _mergeLineEndings = options.MergeLineEndings;

        SerialPort port = new SerialPort(options.PortName, options.BaudRate, options.Parity, options.DataBits, options.StopBits)
        {
            Handshake = options.Handshake,
            ReadTimeout = 250,
            WriteTimeout = 250,
            Encoding = options.Encoding,
            DtrEnable = false,
            RtsEnable = false
        };

        try
        {
            port.Open();

            lock (_gate)
            {
                _port = port;
                _cts = new CancellationTokenSource();
                _readerTask = Task.Run(() => ReadLoop(port, _cts.Token));
            }
        }
        catch
        {
            port.Dispose();
            throw;
        }

        StatusChanged?.Invoke(this, $"Connected to {options.PortName}");
    }

    public void Disconnect()
    {
        SerialPort? port;
        CancellationTokenSource? cts;
        Task? readerTask;

        lock (_gate)
        {
            port = _port;
            cts = _cts;
            readerTask = _readerTask;
            _port = null;
            _cts = null;
            _readerTask = null;
        }

        cts?.Cancel();

        try
        {
            if (port?.IsOpen == true)
                port.Close();
        }
        catch
        {
            // Ignore shutdown races.
        }

        if (readerTask != null && readerTask.Id != Task.CurrentId)
        {
            try
            {
#pragma warning disable VSTHRD002
                readerTask.Wait(500);
#pragma warning restore VSTHRD002
            }
            catch
            {
                // Ignore shutdown races.
            }
        }

        try
        {
            port?.Dispose();
        }
        catch
        {
            // Ignore shutdown races.
        }

        cts?.Dispose();

        StatusChanged?.Invoke(this, "Disconnected");
    }

    private void ReadLoop(SerialPort port, CancellationToken token)
    {
        byte[] buffer = new byte[4096];

        while (!token.IsCancellationRequested)
        {
            try
            {
                if (!port.IsOpen)
                    break;

                int count = port.Read(buffer, 0, buffer.Length);
                if (count <= 0)
                    continue;

                byte[] chunk = new byte[count];
                Buffer.BlockCopy(buffer, 0, chunk, 0, count);
                BytesReceived?.Invoke(this, chunk);

                string text = _encoding.GetString(buffer, 0, count);
                text = _lineEndingNormalizer.Normalize(text, _mergeLineEndings);
                ChunkReceived?.Invoke(this, new ChunkReceivedEventArgs(chunk, text));
                TextReceived?.Invoke(this, text);
            }
            catch (TimeoutException)
            {
                // Normal for SerialPort timeout-based reads.
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                if (!token.IsCancellationRequested)
                {
                    Error?.Invoke(this, ex);
                    Disconnect();
                }

                break;
            }
        }
    }

    public void Dispose()
    {
        Disconnect();
    }
}
