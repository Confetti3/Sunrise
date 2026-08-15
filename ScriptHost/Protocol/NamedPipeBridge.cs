using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Sunrise.ScriptHost.Runtime;

namespace Sunrise.ScriptHost.Protocol;

public sealed class NamedPipeBridge : IAsyncDisposable
{
    private const int MaximumLineBytes = 2_048;

    private readonly string _pipeName;
    private readonly object _connectionLock = new();
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private NamedPipeServerStream? _pipe;
    private StreamWriter? _writer;
    private bool _disposed;

    public NamedPipeBridge(string pipeName)
    {
        _pipeName = pipeName;
    }

    public event Func<JsonElement, CancellationToken, Task>? MessageReceived;
    public event Func<bool, CancellationToken, Task>? ConnectionChanged;

    public bool IsConnected
    {
        get
        {
            lock (_connectionLock)
            {
                return _pipe is { IsConnected: true } && _writer is not null;
            }
        }
    }

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                _pipeName,
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);

            try
            {
                await pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
                await HandleConnectionAsync(pipe, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (IOException exception)
            {
                Console.Error.WriteLine($"Bridge connection ended: {exception.Message}");
            }
            finally
            {
                await ClearConnectionAsync(pipe, cancellationToken).ConfigureAwait(false);
            }
        }
    }

    public async Task<bool> SendAsync<T>(T message, CancellationToken cancellationToken)
    {
        StreamWriter? writer;
        lock (_connectionLock)
        {
            writer = _writer;
        }

        if (writer is null)
        {
            return false;
        }

        string json = JsonSerializer.Serialize(message, Json.WireOptions);
        if (Encoding.UTF8.GetByteCount(json) > MaximumLineBytes
            || json.Contains('\r') || json.Contains('\n'))
        {
            throw new InvalidOperationException(
                $"Bridge messages must be one line and at most {MaximumLineBytes:N0} UTF-8 bytes.");
        }

        await _writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await writer.WriteLineAsync(json.AsMemory(), cancellationToken).ConfigureAwait(false);
            await writer.FlushAsync(cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (ObjectDisposedException)
        {
            return false;
        }
        finally
        {
            _writeGate.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        NamedPipeServerStream? pipe;
        StreamWriter? writer;
        lock (_connectionLock)
        {
            pipe = _pipe;
            writer = _writer;
            _pipe = null;
            _writer = null;
        }

        if (writer is not null)
        {
            await writer.DisposeAsync().ConfigureAwait(false);
        }

        if (pipe is not null)
        {
            await pipe.DisposeAsync().ConfigureAwait(false);
        }

        _writeGate.Dispose();
    }

    private async Task HandleConnectionAsync(
        NamedPipeServerStream pipe,
        CancellationToken cancellationToken)
    {
        await using var writer = new StreamWriter(
            pipe,
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
            bufferSize: 4096,
            leaveOpen: true)
        {
            AutoFlush = true,
        };

        lock (_connectionLock)
        {
            _pipe = pipe;
            _writer = writer;
        }

        await RaiseConnectionChangedAsync(connected: true, cancellationToken).ConfigureAwait(false);

        var lineBuffer = new BoundedUtf8LineBuffer(MaximumLineBytes);
        var chunk = new byte[4096];

        while (pipe.IsConnected && !cancellationToken.IsCancellationRequested)
        {
            int read = await pipe.ReadAsync(chunk, cancellationToken).ConfigureAwait(false);
            if (read == 0)
            {
                break;
            }

            for (int index = 0; index < read; ++index)
            {
                switch (lineBuffer.Push(chunk[index], out string? line))
                {
                    case BoundedLineResult.Pending:
                        break;
                    case BoundedLineResult.Complete:
                        if (line is { Length: > 0 })
                        {
                            await DispatchLineAsync(line, cancellationToken).ConfigureAwait(false);
                        }
                        break;
                    case BoundedLineResult.InvalidUtf8:
                        Console.Error.WriteLine("Rejected invalid UTF-8 bridge line.");
                        break;
                    case BoundedLineResult.Oversized:
                        Console.Error.WriteLine(
                            $"Rejected bridge line longer than {MaximumLineBytes:N0} bytes.");
                        return;
                }
            }
        }
    }

    private async Task DispatchLineAsync(string line, CancellationToken cancellationToken)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(line);
            JsonElement root = document.RootElement.Clone();
            Func<JsonElement, CancellationToken, Task>? handler = MessageReceived;
            if (handler is not null)
            {
                await handler(root, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (JsonException exception)
        {
            Console.Error.WriteLine($"Rejected malformed bridge JSON: {exception.Message}");
        }
    }

    private async Task ClearConnectionAsync(
        NamedPipeServerStream pipe,
        CancellationToken cancellationToken)
    {
        bool changed = false;
        lock (_connectionLock)
        {
            if (ReferenceEquals(_pipe, pipe))
            {
                _pipe = null;
                _writer = null;
                changed = true;
            }
        }

        if (changed)
        {
            await RaiseConnectionChangedAsync(connected: false, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async Task RaiseConnectionChangedAsync(
        bool connected,
        CancellationToken cancellationToken)
    {
        Func<bool, CancellationToken, Task>? handler = ConnectionChanged;
        if (handler is not null)
        {
            await handler(connected, cancellationToken).ConfigureAwait(false);
        }
    }
}
