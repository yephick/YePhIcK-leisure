using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace UartMonitor.Serial
{
    public sealed class CaptureLineIndex
    {
        private readonly List<CaptureLine> _lines = new List<CaptureLine>();

        public int Count => _lines.Count;

        public void Clear()
        {
            _lines.Clear();
        }

        public void AppendChunk(string textChunk, string hexChunk)
        {
            LinePart[] textLines = SplitLines(textChunk);
            LinePart[] hexLines = SplitLines(hexChunk);
            int count = Math.Max(textLines.Length, hexLines.Length);
            int documentOffset = _lines.Count > 0
                ? _lines[_lines.Count - 1].DocumentStartOffset + _lines[_lines.Count - 1].DocumentLength
                : 0;

            for (int index = 0; index < count; index++)
            {
                LinePart textLine = index < textLines.Length ? textLines[index] : LinePart.Empty;
                LinePart hexLine = index < hexLines.Length ? hexLines[index] : LinePart.Empty;

                if (_lines.Count > 0 && !_lines[_lines.Count - 1].IsComplete && index == 0)
                {
                    _lines[_lines.Count - 1].Append(textLine.Text, hexLine.Text, textLine.IsComplete || hexLine.IsComplete, Math.Max(textLine.LineBreakLength, hexLine.LineBreakLength), textLine.Text);
                    documentOffset += textLine.Text.Length + Math.Max(textLine.LineBreakLength, hexLine.LineBreakLength);
                    continue;
                }

                int lineBreakLength = Math.Max(textLine.LineBreakLength, hexLine.LineBreakLength);
                _lines.Add(new CaptureLine(_lines.Count, textLine.Text, hexLine.Text, textLine.IsComplete || hexLine.IsComplete, lineBreakLength, documentOffset, 0, textLine.Text));
                documentOffset += textLine.Text.Length + lineBreakLength;
            }
        }

        public void AppendLinePart(string text, string hex, bool isComplete, int lineBreakLength, string ansiText)
        {
            int documentOffset = _lines.Count > 0
                ? _lines[_lines.Count - 1].DocumentStartOffset + _lines[_lines.Count - 1].DocumentLength
                : 0;

            if (_lines.Count > 0 && !_lines[_lines.Count - 1].IsComplete)
            {
                _lines[_lines.Count - 1].Append(text, hex, isComplete, lineBreakLength, ansiText);
                return;
            }

            _lines.Add(new CaptureLine(_lines.Count, text, hex, isComplete, lineBreakLength, documentOffset, 0, ansiText));
        }

        public CaptureLine GetLine(int index)
        {
            if (_lines.Count == 0)
                return CaptureLine.Empty;

            index = Math.Max(0, Math.Min(index, _lines.Count - 1));
            return _lines[index];
        }

        public IReadOnlyList<CaptureLine> GetLines()
        {
            return _lines;
        }

        public int GetDocumentOffsetForLine(int lineIndex)
        {
            if (_lines.Count == 0)
                return 0;

            lineIndex = Math.Max(0, Math.Min(lineIndex, _lines.Count - 1));
            return _lines[lineIndex].DocumentStartOffset;
        }

        public int GetRenderedDocumentOffsetForLine(int lineIndex)
        {
            return GetRenderedTextDocumentOffsetForLine(lineIndex);
        }

        public int GetRenderedTextDocumentOffsetForLine(int lineIndex)
        {
            if (_lines.Count == 0)
                return 0;

            lineIndex = Math.Max(0, Math.Min(lineIndex, _lines.Count - 1));
            return _lines[lineIndex].RenderedTextDocumentStartOffset;
        }

        public int GetRenderedHexDocumentOffsetForLine(int lineIndex)
        {
            if (_lines.Count == 0)
                return 0;

            lineIndex = Math.Max(0, Math.Min(lineIndex, _lines.Count - 1));
            return _lines[lineIndex].RenderedHexDocumentStartOffset;
        }

        public int GetLineIndexForDocumentOffset(int documentOffset)
        {
            if (_lines.Count == 0)
                return 0;

            for (int index = 0; index < _lines.Count; index++)
            {
                CaptureLine line = _lines[index];
                int lineStart = line.DocumentStartOffset;
                int lineEnd = lineStart + line.TextLength;
                if (documentOffset >= lineStart && documentOffset < lineEnd)
                    return index;
            }

            return _lines.Count - 1;
        }

        public int GetLineIndexForRenderedDocumentOffset(int documentOffset)
        {
            return GetLineIndexForRenderedTextDocumentOffset(documentOffset);
        }

        public int GetLineIndexForRenderedTextDocumentOffset(int documentOffset)
        {
            if (_lines.Count == 0)
                return 0;

            for (int index = 0; index < _lines.Count; index++)
            {
                CaptureLine line = _lines[index];
                int lineStart = line.RenderedTextDocumentStartOffset;
                int lineEnd = lineStart + line.TextLength + 2;
                if (documentOffset >= lineStart && documentOffset < lineEnd)
                    return index;
            }

            return _lines.Count - 1;
        }

        public int GetLineIndexForRenderedHexDocumentOffset(int documentOffset)
        {
            if (_lines.Count == 0)
                return 0;

            for (int index = 0; index < _lines.Count; index++)
            {
                CaptureLine line = _lines[index];
                int lineStart = line.RenderedHexDocumentStartOffset;
                int lineEnd = lineStart + line.HexLength + 2;
                if (documentOffset >= lineStart && documentOffset < lineEnd)
                    return index;
            }

            return _lines.Count - 1;
        }

        public int GetLineLocalOffset(int lineIndex, int documentOffset)
        {
            int lineStartOffset = GetDocumentOffsetForLine(lineIndex);
            CaptureLine line = GetLine(lineIndex);
            return Math.Max(0, Math.Min(documentOffset - lineStartOffset, line.TextLength));
        }

        public int GetRenderedLineLocalOffset(int lineIndex, int documentOffset)
        {
            int lineStartOffset = GetRenderedTextDocumentOffsetForLine(lineIndex);
            CaptureLine line = GetLine(lineIndex);
            return Math.Max(0, Math.Min(documentOffset - lineStartOffset, line.TextLength));
        }

        public int GetRenderedHexLineLocalOffset(int lineIndex, int documentOffset)
        {
            int lineStartOffset = GetRenderedHexDocumentOffsetForLine(lineIndex);
            CaptureLine line = GetLine(lineIndex);
            return Math.Max(0, Math.Min(documentOffset - lineStartOffset, line.HexLength));
        }

        public int FindBestLineIndex(string selectedText, int preferredIndex)
        {
            if (_lines.Count == 0)
                return 0;

            preferredIndex = Math.Max(0, Math.Min(preferredIndex, _lines.Count - 1));

            if (string.IsNullOrEmpty(selectedText))
                return preferredIndex;

            for (int radius = 0; radius < _lines.Count; radius++)
            {
                int left = preferredIndex - radius;
                if (left >= 0 && _lines[left].Text.Contains(selectedText))
                    return left;

                int right = preferredIndex + radius;
                if (right < _lines.Count && _lines[right].Text.Contains(selectedText))
                    return right;
            }

            return preferredIndex;
        }

        private static LinePart[] SplitLines(string text)
        {
            var lines = new List<LinePart>();
            int start = 0;

            for (int index = 0; index < text.Length; index++)
            {
                char current = text[index];
                if (current != '\r' && current != '\n')
                    continue;

                int end = index;
                int lineBreakLength = 1;
                if (index + 1 < text.Length && IsLineBreakPair(current, text[index + 1]))
                {
                    lineBreakLength = 2;
                    index++;
                }

                lines.Add(new LinePart(text.Substring(start, end - start), true, lineBreakLength));
                start = index + 1;
            }

            if (start < text.Length)
                lines.Add(new LinePart(text.Substring(start, text.Length - start), false, 0));

            return lines.ToArray();
        }

        private static bool IsLineBreakPair(char first, char second)
        {
            return (first == '\r' && second == '\n') || (first == '\n' && second == '\r');
        }

        private readonly struct LinePart
        {
            public static readonly LinePart Empty = new LinePart(string.Empty, false, 0);

            public LinePart(string text, bool isComplete, int lineBreakLength)
            {
                Text = text;
                IsComplete = isComplete;
                LineBreakLength = lineBreakLength;
            }

            public string Text { get; }
            public bool IsComplete { get; }
            public int LineBreakLength { get; }
        }

        public sealed class CaptureLine
        {
            public static readonly CaptureLine Empty = new CaptureLine(-1, string.Empty, string.Empty, false, 0, 0, 0, string.Empty);
            private readonly StringBuilder _textBuilder = new StringBuilder();
            private readonly StringBuilder _hexBuilder = new StringBuilder();
            private readonly StringBuilder _ansiTextBuilder = new StringBuilder();

            public CaptureLine(int index, string text, string hex, bool isComplete, int lineBreakLength, int documentStartOffset, int renderedDocumentStartOffset, string ansiText)
            {
                Index = index;
                _textBuilder.Append(text);
                _hexBuilder.Append(hex);
                _ansiTextBuilder.Append(ansiText ?? string.Empty);
                IsComplete = isComplete;
                LineBreakLength = Math.Max(0, lineBreakLength);
                DocumentStartOffset = Math.Max(0, documentStartOffset);
                SetRenderedDocumentStartOffsets(renderedDocumentStartOffset, renderedDocumentStartOffset);
            }

            public CaptureLine(int index, string text, string hex, bool isComplete)
                : this(index, text, hex, isComplete, 0, 0, 0, text)
            {
            }

            public CaptureLine(int index, string text, string hex, bool isComplete, int lineBreakLength)
                : this(index, text, hex, isComplete, lineBreakLength, 0, 0, text)
            {
            }

            public int Index { get; }
            public bool IsComplete { get; private set; }
            public string Text => _textBuilder.ToString();
            public string Hex => _hexBuilder.ToString();
            public string AnsiText => _ansiTextBuilder.ToString();
            public bool HasText => _textBuilder.Length > 0;
            public bool HasHex => _hexBuilder.Length > 0;
            public int TextLength => _textBuilder.Length;
            public int HexLength => _hexBuilder.Length;
            public int LineBreakLength { get; private set; }
            public int DocumentLength => TextLength + LineBreakLength;
            public int DocumentStartOffset { get; private set; }
            public int RenderedDocumentStartOffset => RenderedTextDocumentStartOffset;
            public int RenderedTextDocumentStartOffset { get; private set; }
            public int RenderedHexDocumentStartOffset { get; private set; }

            public void SetRenderedDocumentStartOffset(int renderedDocumentStartOffset)
            {
                SetRenderedDocumentStartOffsets(renderedDocumentStartOffset, renderedDocumentStartOffset);
            }

            public void SetRenderedDocumentStartOffsets(int renderedTextDocumentStartOffset, int renderedHexDocumentStartOffset)
            {
                RenderedTextDocumentStartOffset = Math.Max(0, renderedTextDocumentStartOffset);
                RenderedHexDocumentStartOffset = Math.Max(0, renderedHexDocumentStartOffset);
            }

            public void Append(string text, string hex, bool isComplete, int lineBreakLength, string ansiText)
            {
                if (!string.IsNullOrEmpty(text))
                {
                    if (_textBuilder.Length > 0)
                        _textBuilder.Append(text);
                    else
                        _textBuilder.Append(text);
                }

                if (!string.IsNullOrEmpty(hex))
                {
                    if (_hexBuilder.Length > 0)
                    {
                        _hexBuilder.Append(' ');
                        _hexBuilder.Append(hex);
                    }
                    else
                        _hexBuilder.Append(hex);
                }

                if (!string.IsNullOrEmpty(ansiText))
                    _ansiTextBuilder.Append(ansiText);

                IsComplete |= isComplete;
                LineBreakLength = Math.Max(LineBreakLength, Math.Max(0, lineBreakLength));
            }

            public bool TryGetHexSelection(string selectedText, Encoding encoding, out int hexStartOffset, out int hexEndOffset)
            {
                hexStartOffset = 0;
                hexEndOffset = 0;

                if (string.IsNullOrEmpty(selectedText) || string.IsNullOrEmpty(Hex))
                    return false;

                byte[] lineBytes = ParseHexBytes(Hex);
                byte[] selectedBytes = encoding.GetBytes(selectedText);

                int byteStart = FindSubsequence(lineBytes, selectedBytes);
                if (byteStart < 0)
                    return false;

                hexStartOffset = ByteIndexToHexOffset(byteStart, Hex);
                hexEndOffset = ByteIndexToHexOffset(byteStart + selectedBytes.Length, Hex);
                return true;
            }

            public bool TryGetHexSelection(int textStartOffset, int textEndOffset, Encoding encoding, out int hexStartOffset, out int hexEndOffset)
            {
                hexStartOffset = 0;
                hexEndOffset = 0;

                if (textEndOffset <= textStartOffset || string.IsNullOrEmpty(Hex))
                    return false;

                List<VisibleByteSpan> spans = BuildVisibleByteSpans(encoding);
                VisibleByteSpan? first = null;
                VisibleByteSpan? last = null;

                foreach (VisibleByteSpan span in spans)
                {
                    if (span.TextEndOffset <= textStartOffset || span.TextStartOffset >= textEndOffset)
                        continue;

                    first ??= span;
                    last = span;
                }

                if (first == null || last == null)
                    return false;

                hexStartOffset = first.Value.HexStartOffset;
                hexEndOffset = last.Value.HexEndOffset;
                return true;
            }

            public bool TryGetTextSelectionFromHex(string selectedHex, Encoding encoding, out int textStartOffset, out int textEndOffset)
            {
                textStartOffset = 0;
                textEndOffset = 0;

                if (string.IsNullOrEmpty(selectedHex) || string.IsNullOrEmpty(Text))
                    return false;

                byte[] selectedBytes = ParseHexBytes(selectedHex);
                if (selectedBytes.Length == 0)
                    return false;

                string selectedText = encoding.GetString(selectedBytes);
                if (string.IsNullOrEmpty(selectedText))
                    return false;

                int startIndex = Text.IndexOf(selectedText, StringComparison.Ordinal);
                if (startIndex < 0)
                    return false;

                textStartOffset = startIndex;
                textEndOffset = startIndex + selectedText.Length;
                return true;
            }

            public bool TryGetTextSelectionFromHex(int hexStartOffset, int hexEndOffset, Encoding encoding, out int textStartOffset, out int textEndOffset)
            {
                textStartOffset = 0;
                textEndOffset = 0;

                if (hexEndOffset <= hexStartOffset || string.IsNullOrEmpty(Text))
                    return false;

                List<VisibleByteSpan> spans = BuildVisibleByteSpans(encoding);
                VisibleByteSpan? first = null;
                VisibleByteSpan? last = null;

                foreach (VisibleByteSpan span in spans)
                {
                    if (span.HexEndOffset <= hexStartOffset || span.HexStartOffset >= hexEndOffset)
                        continue;

                    first ??= span;
                    last = span;
                }

                if (first == null || last == null)
                    return false;

                textStartOffset = first.Value.TextStartOffset;
                textEndOffset = last.Value.TextEndOffset;
                return true;
            }

            public bool ContainsText(string selectedText)
            {
                return !string.IsNullOrEmpty(selectedText) && Text.Contains(selectedText);
            }

            private static byte[] ParseHexBytes(string hex)
            {
                var bytes = new List<byte>();
                ReadOnlySpan<char> span = hex.AsSpan();
                int index = 0;

                while (index < span.Length)
                {
                    while (index < span.Length && char.IsWhiteSpace(span[index]))
                        index++;

                    if (index + 1 >= span.Length)
                        break;

                    if (!IsHexDigit(span[index]) || !IsHexDigit(span[index + 1]))
                    {
                        index++;
                        continue;
                    }

                    string token = span.Slice(index, 2).ToString();
                    if (byte.TryParse(token, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out byte value))
                        bytes.Add(value);

                    index += 2;
                }

                return bytes.ToArray();
            }

            private List<VisibleByteSpan> BuildVisibleByteSpans(Encoding encoding)
            {
                List<HexToken> tokens = ParseHexTokens(Hex);
                var spans = new List<VisibleByteSpan>();
                int textOffset = 0;

                for (int index = 0; index < tokens.Count; index++)
                {
                    byte value = tokens[index].Value;

                    if (value == 0x1B)
                    {
                        SkipEscape(tokens, ref index);
                        continue;
                    }

                    if (IsSkippedControl(value))
                        continue;

                    string text = encoding.GetString(new[] { value });
                    if (string.IsNullOrEmpty(text))
                        continue;

                    spans.Add(new VisibleByteSpan(textOffset, textOffset + text.Length, tokens[index].StartOffset, tokens[index].EndOffset));
                    textOffset += text.Length;
                }

                return spans;
            }

            private static List<HexToken> ParseHexTokens(string hex)
            {
                var tokens = new List<HexToken>();
                ReadOnlySpan<char> span = hex.AsSpan();
                int index = 0;

                while (index < span.Length)
                {
                    while (index < span.Length && char.IsWhiteSpace(span[index]))
                        index++;

                    if (index + 1 >= span.Length)
                        break;

                    if (!IsHexDigit(span[index]) || !IsHexDigit(span[index + 1]))
                    {
                        index++;
                        continue;
                    }

                    int start = index;
                    string token = span.Slice(index, 2).ToString();
                    if (byte.TryParse(token, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out byte value))
                        tokens.Add(new HexToken(value, start, index + 2));

                    index += 2;
                }

                return tokens;
            }

            private static void SkipEscape(IReadOnlyList<HexToken> tokens, ref int index)
            {
                if (index + 1 >= tokens.Count)
                    return;

                byte next = tokens[index + 1].Value;
                if (next == (byte)'[')
                {
                    index += 2;
                    while (index < tokens.Count)
                    {
                        byte value = tokens[index].Value;
                        if (value >= 0x40 && value <= 0x7E)
                            return;

                        index++;
                    }

                    index = tokens.Count - 1;
                    return;
                }

                index++;
            }

            private static bool IsSkippedControl(byte value)
            {
                return value < 0x20 && value != 0x09;
            }

            private static int FindSubsequence(byte[] source, byte[] pattern)
            {
                if (pattern.Length == 0 || pattern.Length > source.Length)
                    return -1;

                for (int index = 0; index <= source.Length - pattern.Length; index++)
                {
                    bool match = true;
                    for (int patternIndex = 0; patternIndex < pattern.Length; patternIndex++)
                    {
                        if (source[index + patternIndex] != pattern[patternIndex])
                        {
                            match = false;
                            break;
                        }
                    }

                    if (match)
                        return index;
                }

                return -1;
            }

            private static int ByteIndexToHexOffset(int byteIndex, string hex)
            {
                if (byteIndex <= 0)
                    return 0;

                int currentByte = 0;
                for (int index = 0; index < hex.Length; index++)
                {
                    if (char.IsWhiteSpace(hex[index]))
                        continue;

                    if (index + 1 < hex.Length && IsHexDigit(hex[index]) && IsHexDigit(hex[index + 1]))
                    {
                        if (currentByte == byteIndex)
                            return index;

                        currentByte++;
                        index++;
                    }
                }

                return hex.Length;
            }

            private static bool IsHexDigit(char value)
            {
                return (value >= '0' && value <= '9')
                    || (value >= 'a' && value <= 'f')
                    || (value >= 'A' && value <= 'F');
            }

            private readonly struct HexToken
            {
                public HexToken(byte value, int startOffset, int endOffset)
                {
                    Value = value;
                    StartOffset = startOffset;
                    EndOffset = endOffset;
                }

                public byte Value { get; }
                public int StartOffset { get; }
                public int EndOffset { get; }
            }

            private readonly struct VisibleByteSpan
            {
                public VisibleByteSpan(int textStartOffset, int textEndOffset, int hexStartOffset, int hexEndOffset)
                {
                    TextStartOffset = textStartOffset;
                    TextEndOffset = textEndOffset;
                    HexStartOffset = hexStartOffset;
                    HexEndOffset = hexEndOffset;
                }

                public int TextStartOffset { get; }
                public int TextEndOffset { get; }
                public int HexStartOffset { get; }
                public int HexEndOffset { get; }
            }
        }
    }
}
