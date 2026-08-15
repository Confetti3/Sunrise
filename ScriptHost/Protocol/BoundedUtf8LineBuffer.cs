using System.Text;

namespace Sunrise.ScriptHost.Protocol;

internal enum BoundedLineResult
{
    Pending,
    Complete,
    InvalidUtf8,
    Oversized,
}

internal sealed class BoundedUtf8LineBuffer
{
    private static readonly Encoding StrictUtf8 = new UTF8Encoding(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);

    private readonly byte[] _bytes;
    private int _count;

    internal BoundedUtf8LineBuffer(int maximumLineBytes)
    {
        _bytes = new byte[maximumLineBytes];
    }

    internal BoundedLineResult Push(byte value, out string? line)
    {
        line = null;

        if (value == (byte)'\n')
        {
            int length = _count;
            if (length != 0 && _bytes[length - 1] == (byte)'\r')
            {
                --length;
            }

            try
            {
                line = StrictUtf8.GetString(_bytes, 0, length);
            }
            catch (DecoderFallbackException)
            {
                _count = 0;
                return BoundedLineResult.InvalidUtf8;
            }

            _count = 0;
            return BoundedLineResult.Complete;
        }

        if (_count == _bytes.Length)
        {
            _count = 0;
            return BoundedLineResult.Oversized;
        }

        _bytes[_count++] = value;
        return BoundedLineResult.Pending;
    }
}
