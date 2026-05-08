using System.Text;

namespace UartMonitor.Serial
{
    public sealed class HexStreamFormatter
    {
        private readonly StringBuilder _currentLine = new StringBuilder();
        private byte? _pendingLineEnding;
        private bool _pendingLineHadContent;

        public string Append(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0)
                return string.Empty;

            StringBuilder output = new StringBuilder(bytes.Length * 4);

            for (int index = 0; index < bytes.Length; index++)
            {
                byte value = bytes[index];

                if (_pendingLineEnding.HasValue)
                {
                    if ((_pendingLineEnding == 0x0d && value == 0x0a) || (_pendingLineEnding == 0x0a && value == 0x0d))
                    {
                        if (_pendingLineHadContent)
                            AppendByte(value);

                        FlushLine(output);
                        _pendingLineEnding = null;
                        _pendingLineHadContent = false;
                        continue;
                    }

                    FlushLine(output);
                    _pendingLineEnding = null;
                    _pendingLineHadContent = false;
                }

                if (value == 0x0d || value == 0x0a)
                {
                    _pendingLineEnding = value;
                    _pendingLineHadContent = _currentLine.Length > 0;

                    if (_pendingLineHadContent)
                        AppendByte(value);

                    continue;
                }

                AppendByte(value);
            }

            return output.ToString();
        }

        public string Flush()
        {
            StringBuilder output = new StringBuilder();

            if (_pendingLineEnding.HasValue)
            {
                FlushLine(output);
                _pendingLineEnding = null;
                _pendingLineHadContent = false;
            }
            else if (_currentLine.Length > 0)
            {
                FlushLine(output);
            }

            return output.ToString();
        }

        private void AppendByte(byte value)
        {
            if (_currentLine.Length > 0)
                _currentLine.Append(' ');

            _currentLine.Append(value.ToString("X2"));
        }

        private void FlushLine(StringBuilder output)
        {
            output.AppendLine(_currentLine.ToString());
            _currentLine.Clear();
        }
    }
}
