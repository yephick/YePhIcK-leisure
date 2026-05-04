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
            private readonly List<LineChunk> _chunks = new List<LineChunk>();
            private LineSelectionMap? _selectionMap;

            public CaptureLine(int index, string text, string hex, bool isComplete, int lineBreakLength, int documentStartOffset, int renderedDocumentStartOffset, string ansiText)
            {
                Index = index;
                _textBuilder.Append(text);
                _hexBuilder.Append(hex);
                _ansiTextBuilder.Append(ansiText ?? string.Empty);
                _chunks.Add(new LineChunk(0, text ?? string.Empty, 0, hex ?? string.Empty));
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
                int textStartOffset = _textBuilder.Length;
                int hexStartOffset = _hexBuilder.Length > 0 && !string.IsNullOrEmpty(hex)
                    ? _hexBuilder.Length + 1
                    : _hexBuilder.Length;

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

                if (!string.IsNullOrEmpty(text) || !string.IsNullOrEmpty(hex))
                    _chunks.Add(new LineChunk(textStartOffset, text ?? string.Empty, hexStartOffset, hex ?? string.Empty));

                IsComplete |= isComplete;
                LineBreakLength = Math.Max(LineBreakLength, Math.Max(0, lineBreakLength));
                _selectionMap = null;
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

            public bool TryGetHexSelection(int textStartOffset, int textEndOffset, Encoding encoding, out int hexStartOffset, out int hexEndOffset, bool includeLeadingBytes = false, bool includeTrailingBytes = false)
            {
                hexStartOffset = 0;
                hexEndOffset = 0;

                if (TextLength == 0 && includeLeadingBytes && includeTrailingBytes && !string.IsNullOrEmpty(Hex))
                {
                    hexEndOffset = HexLength;
                    return true;
                }

                if (textEndOffset <= textStartOffset || string.IsNullOrEmpty(Hex))
                    return false;

                LineSelectionMap map = GetOrBuildSelectionMap(encoding);
                int startSpanIndex = -1;
                int startToken = -1;
                int endToken = -1;
                for (int index = 0; index < map.Spans.Count; index++)
                {
                    VisibleByteSpan span = map.Spans[index];
                    if (span.TextEndOffset <= textStartOffset || span.TextStartOffset >= textEndOffset)
                        continue;

                    if (startToken < 0)
                    {
                        startSpanIndex = index;
                        startToken = span.TokenStartIndex;
                    }

                    endToken = span.TokenStartIndex;
                }

                if (startToken < 0 || endToken < startToken || map.Tokens.Count == 0)
                    return false;

                int effectiveStartToken = includeLeadingBytes
                    ? 0
                    : GetBoundaryStartToken(map.Spans, startSpanIndex, startToken);
                hexStartOffset = map.Tokens[Math.Max(0, Math.Min(effectiveStartToken, map.Tokens.Count - 1))].StartOffset;
                hexEndOffset = map.Tokens[endToken].EndOffset;
                if (includeTrailingBytes)
                    hexEndOffset = HexLength;
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

                LineSelectionMap map = GetOrBuildSelectionMap(encoding);
                if (map.Tokens.Count == 0)
                    return false;

                int normalizedHexStartOffset = Math.Max(0, Math.Min(hexStartOffset, Math.Max(0, HexLength - 1)));
                int normalizedHexEndOffset = Math.Max(normalizedHexStartOffset, Math.Min(hexEndOffset, Math.Max(0, HexLength - 1)));
                int startTokenIndex = FindTokenIndexForOffset(map.Tokens, normalizedHexStartOffset, preferPrevious: false);
                int endTokenIndexInclusive = FindTokenIndexForOffset(map.Tokens, normalizedHexEndOffset, preferPrevious: true);
                int endTokenIndexExclusive = Math.Min(map.Tokens.Count, endTokenIndexInclusive + 1);
                if (startTokenIndex < 0 || endTokenIndexExclusive <= startTokenIndex)
                    return false;

                if (!ContainsAnyVisibleToken(map.Tokens, startTokenIndex, endTokenIndexExclusive, map.AnsiSpans))
                    return false;

                int firstSpanIndex = -1;
                int lastSpanIndex = -1;
                for (int i = 0; i < map.Spans.Count; i++)
                {
                    VisibleByteSpan span = map.Spans[i];
                    if (IsInsideAnsiSpan(span.TokenStartIndex, map.AnsiSpans))
                        continue;
                    if (span.TokenEndIndexExclusive <= startTokenIndex || span.TokenStartIndex >= endTokenIndexExclusive)
                        continue;
                    if (firstSpanIndex < 0)
                        firstSpanIndex = i;
                    lastSpanIndex = i;
                }

                if (firstSpanIndex < 0 || lastSpanIndex < firstSpanIndex)
                    return false;

                textStartOffset = map.Spans[firstSpanIndex].TextStartOffset;
                textEndOffset = map.Spans[lastSpanIndex].TextEndOffset;
                return true;
            }

            private static bool ContainsAnyVisibleToken(IReadOnlyList<HexToken> tokens, int startTokenIndex, int endTokenIndexExclusive, IReadOnlyList<TokenRange> ansiSpans)
            {
                int upper = Math.Min(endTokenIndexExclusive, tokens.Count);
                for (int i = Math.Max(0, startTokenIndex); i < upper; i++)
                {
                    if (IsInsideAnsiSpan(i, ansiSpans))
                        continue;

                    byte value = tokens[i].Value;
                    if (value == 0x1B)
                        continue;

                    if (IsSkippedControl(value))
                        continue;

                    return true;
                }

                return false;
            }

            private static int GetBoundaryStartToken(IReadOnlyList<VisibleByteSpan> spans, int startSpanIndex, int fallbackStartToken)
            {
                if (startSpanIndex <= 0)
                    return 0;

                int previousVisibleEnd = spans[startSpanIndex - 1].TokenEndIndexExclusive;
                return Math.Min(previousVisibleEnd, fallbackStartToken);
            }

            private static bool IsInsideAnsiSpan(int tokenIndex, IReadOnlyList<TokenRange> ansiSpans)
            {
                for (int i = 0; i < ansiSpans.Count; i++)
                {
                    TokenRange span = ansiSpans[i];
                    if (tokenIndex >= span.Start && tokenIndex < span.EndExclusive)
                        return true;
                }

                return false;
            }

            private static int FindTokenIndexForOffset(IReadOnlyList<HexToken> tokens, int offset, bool preferPrevious)
            {
                if (tokens.Count == 0)
                    return -1;

                for (int i = 0; i < tokens.Count; i++)
                {
                    HexToken token = tokens[i];
                    if (offset >= token.StartOffset && offset < token.EndOffset)
                        return i;

                    if (offset < token.StartOffset)
                        return preferPrevious ? Math.Max(0, i - 1) : i;
                }

                return tokens.Count - 1;
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

            private LineSelectionMap GetOrBuildSelectionMap(Encoding encoding)
            {
                if (_selectionMap != null && _selectionMap.EncodingCodePage == encoding.CodePage)
                    return _selectionMap;

                var tokens = new List<HexToken>();
                var spans = new List<VisibleByteSpan>();
                var ansiSpans = new List<TokenRange>();
                int tokenBaseIndex = 0;

                foreach (LineChunk chunk in _chunks)
                {
                    List<HexToken> chunkTokens = ParseHexTokens(chunk.Hex, chunk.HexStartOffset);
                    int chunkTextOffset = 0;
                    bool sawEscape = false;

                    for (int index = 0; index < chunkTokens.Count; index++)
                    {
                        byte value = chunkTokens[index].Value;

                        if (value == 0x1B)
                        {
                            int ansiStart = tokenBaseIndex + index;
                            if (!sawEscape && chunkTextOffset < chunk.Text.Length)
                            {
                                RemoveChunkSpans(spans, chunk.TextStartOffset);
                                chunkTextOffset = 0;
                            }

                            sawEscape = true;
                            SkipEscape(chunkTokens, ref index);
                            ansiSpans.Add(new TokenRange(ansiStart, tokenBaseIndex + Math.Min(chunkTokens.Count, index + 1)));
                            continue;
                        }

                        if (IsSkippedControl(value))
                            continue;

                        string text = encoding.GetString(new[] { value });
                        if (string.IsNullOrEmpty(text))
                            continue;

                        if (!MatchesVisibleTextAtOffset(chunk.Text, text, chunkTextOffset))
                            continue;

                        int textStartOffset = chunk.TextStartOffset + chunkTextOffset;
                        spans.Add(new VisibleByteSpan(textStartOffset, textStartOffset + text.Length, chunkTokens[index].StartOffset, chunkTokens[index].EndOffset, tokenBaseIndex + index, tokenBaseIndex + index + 1));
                        chunkTextOffset += text.Length;
                    }

                    tokens.AddRange(chunkTokens);
                    tokenBaseIndex += chunkTokens.Count;
                }

                _selectionMap = new LineSelectionMap(encoding.CodePage, tokens, spans, ansiSpans);
                return _selectionMap;
            }

            private static void RemoveChunkSpans(List<VisibleByteSpan> spans, int chunkTextStartOffset)
            {
                for (int index = spans.Count - 1; index >= 0; index--)
                {
                    if (spans[index].TextStartOffset >= chunkTextStartOffset)
                        spans.RemoveAt(index);
                }
            }

            private static bool MatchesVisibleTextAtOffset(string lineText, string value, int startOffset)
            {
                if (string.IsNullOrEmpty(lineText) || string.IsNullOrEmpty(value))
                    return false;

                if (startOffset < 0 || startOffset + value.Length > lineText.Length)
                    return false;

                return string.CompareOrdinal(lineText, startOffset, value, 0, value.Length) == 0;
            }

            private static List<HexToken> ParseHexTokens(string hex)
            {
                return ParseHexTokens(hex, 0);
            }

            private static List<HexToken> ParseHexTokens(string hex, int offsetAdjustment)
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
                        tokens.Add(new HexToken(value, offsetAdjustment + start, offsetAdjustment + index + 2));

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

            private readonly struct LineChunk
            {
                public LineChunk(int textStartOffset, string text, int hexStartOffset, string hex)
                {
                    TextStartOffset = textStartOffset;
                    Text = text;
                    HexStartOffset = hexStartOffset;
                    Hex = hex;
                }

                public int TextStartOffset { get; }
                public string Text { get; }
                public int HexStartOffset { get; }
                public string Hex { get; }
            }

            private readonly struct VisibleByteSpan
            {
                public VisibleByteSpan(int textStartOffset, int textEndOffset, int hexStartOffset, int hexEndOffset, int tokenStartIndex, int tokenEndIndexExclusive)
                {
                    TextStartOffset = textStartOffset;
                    TextEndOffset = textEndOffset;
                    HexStartOffset = hexStartOffset;
                    HexEndOffset = hexEndOffset;
                    TokenStartIndex = tokenStartIndex;
                    TokenEndIndexExclusive = tokenEndIndexExclusive;
                }

                public int TextStartOffset { get; }
                public int TextEndOffset { get; }
                public int HexStartOffset { get; }
                public int HexEndOffset { get; }
                public int TokenStartIndex { get; }
                public int TokenEndIndexExclusive { get; }
            }

            private sealed class LineSelectionMap
            {
                public LineSelectionMap(int encodingCodePage, List<HexToken> tokens, List<VisibleByteSpan> spans, List<TokenRange> ansiSpans)
                {
                    EncodingCodePage = encodingCodePage;
                    Tokens = tokens;
                    Spans = spans;
                    AnsiSpans = ansiSpans;
                }

                public int EncodingCodePage { get; }
                public List<HexToken> Tokens { get; }
                public List<VisibleByteSpan> Spans { get; }
                public List<TokenRange> AnsiSpans { get; }
            }

            private readonly struct TokenRange
            {
                public TokenRange(int start, int endExclusive)
                {
                    Start = start;
                    EndExclusive = endExclusive;
                }

                public int Start { get; }
                public int EndExclusive { get; }
            }
        }
    }
}
