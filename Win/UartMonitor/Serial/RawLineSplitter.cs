using System;
using System.Collections.Generic;

namespace UartMonitor.Serial
{
    public sealed class RawLineSplitter
    {
        private readonly List<byte> _currentLine = new List<byte>();
        private byte? _pendingLineEnding;
        private int _emittedCount;
        private DateTime _currentLineTimestamp;

        public IEnumerable<RawLinePart> Append(byte[] bytes)
        {
            return Append(bytes, DateTime.MinValue);
        }

        public IEnumerable<RawLinePart> Append(byte[] bytes, DateTime timestamp)
        {
            if (bytes == null || bytes.Length == 0)
                yield break;

            for (int index = 0; index < bytes.Length; index++)
            {
                byte value = bytes[index];

                if (_pendingLineEnding.HasValue)
                {
                    if (IsLineEndingPair(_pendingLineEnding.Value, value))
                    {
                        _currentLine.Add(value);
                        yield return Flush(isComplete: true);
                        _pendingLineEnding = null;
                        continue;
                    }

                    yield return Flush(isComplete: true);
                    _pendingLineEnding = null;
                }

                if (_currentLine.Count == 0)
                    _currentLineTimestamp = timestamp;

                _currentLine.Add(value);

                if (value == 0x0d || value == 0x0a)
                    _pendingLineEnding = value;
            }

        }

        public RawLinePart? Flush()
        {
            if (_currentLine.Count == 0)
                return null;

            _pendingLineEnding = null;
            return Flush(isComplete: _currentLine[_currentLine.Count - 1] == 0x0d || _currentLine[_currentLine.Count - 1] == 0x0a);
        }

        public void Clear()
        {
            _currentLine.Clear();
            _pendingLineEnding = null;
            _emittedCount = 0;
            _currentLineTimestamp = DateTime.MinValue;
        }

        private RawLinePart Flush(bool isComplete)
        {
            RawLinePart part = CreatePart(_emittedCount, _currentLine.Count - _emittedCount, isComplete);
            _currentLine.Clear();
            _emittedCount = 0;
            _currentLineTimestamp = DateTime.MinValue;
            return part;
        }

        private RawLinePart CreatePart(int start, int count, bool isComplete)
        {
            byte[] bytes = _currentLine.GetRange(start, count).ToArray();
            int lineEndingLength = GetLineEndingLength(bytes);
            return new RawLinePart(bytes, isComplete, lineEndingLength, _currentLineTimestamp);
        }

        private static int GetLineEndingLength(byte[] bytes)
        {
            if (bytes.Length == 0)
                return 0;

            byte last = bytes[bytes.Length - 1];
            if (last != 0x0d && last != 0x0a)
                return 0;

            if (bytes.Length >= 2 && IsLineEndingPair(bytes[bytes.Length - 2], last))
                return 2;

            return 1;
        }

        private static bool IsLineEndingPair(byte first, byte second)
        {
            return (first == 0x0d && second == 0x0a) || (first == 0x0a && second == 0x0d);
        }
    }

    public readonly struct RawLinePart
    {
        public RawLinePart(byte[] bytes, bool isComplete, int lineEndingLength)
            : this(bytes, isComplete, lineEndingLength, DateTime.MinValue)
        {
        }

        public RawLinePart(byte[] bytes, bool isComplete, int lineEndingLength, DateTime timestamp)
        {
            Bytes = bytes;
            IsComplete = isComplete;
            LineEndingLength = lineEndingLength;
            Timestamp = timestamp;
        }

        public byte[] Bytes { get; }
        public bool IsComplete { get; }
        public int LineEndingLength { get; }
        public DateTime Timestamp { get; }
    }
}
