using Microsoft.VisualStudio.TestTools.UnitTesting;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using UartMonitor.Rendering;
using UartMonitor.Serial;

namespace UartMonitor.Tests.Serial
{
    [TestClass]
    public sealed class SampleSelectionMappingTests
    {
        private sealed class LineData
        {
            public CaptureLineIndex.CaptureLine Line { get; set; } = CaptureLineIndex.CaptureLine.Empty;
            public byte[] Bytes { get; set; } = Array.Empty<byte>();
            public int EolLength { get; set; }
            public List<VisibleChar> VisibleChars { get; set; } = new List<VisibleChar>();
        }

        private readonly struct VisibleChar
        {
            public VisibleChar(int textOffset, int byteIndex, byte value)
            {
                TextOffset = textOffset;
                ByteIndex = byteIndex;
                Value = value;
            }

            public int TextOffset { get; }
            public int ByteIndex { get; }
            public byte Value { get; }
        }

        [TestMethod]
        public void LeftToRight_SingleLineRandomSelections_MatchExpectedBytesIncludingAnsiInsideRange()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 12).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(20260502 + pass);
                for (int testIndex = 0; testIndex < 40; testIndex++)
                {
                    LineData selectedLine = candidates[random.Next(candidates.Length)];
                    int start = random.Next(0, selectedLine.VisibleChars.Count - 2);
                    int end = random.Next(start + 1, Math.Min(selectedLine.VisibleChars.Count, start + 8));

                    int textStart = selectedLine.VisibleChars[start].TextOffset;
                    int textEnd = selectedLine.VisibleChars[end].TextOffset + 1;

                    bool mapped = selectedLine.Line.TryGetHexSelection(textStart, textEnd, encoding, out int hexStartOffset, out int hexEndOffset);
                    Assert.IsTrue(mapped, $"Expected text->hex mapping on line: {selectedLine.Line.Text}");

                    string actualHex = ExtractHexSpan(selectedLine.Line.Hex, hexStartOffset, hexEndOffset);
                    string expectedHex = ExpectedHexForTextRange(selectedLine, start, end);
                    Assert.AreEqual(expectedHex, actualHex, $"Mismatch for text span '{selectedLine.Line.Text.Substring(textStart, textEnd - textStart)}'");
                }
            }
        }

        [TestMethod]
        public void LeftToRight_MultiLineRandomSelections_MatchExpectedBytesWithEolAllowance()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 8).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(20260503 + pass);
                for (int testIndex = 0; testIndex < 20; testIndex++)
                {
                    int startLine = random.Next(0, candidates.Length - 2);
                    int endLine = random.Next(startLine + 1, Math.Min(candidates.Length, startLine + 4));
                    LineData first = candidates[startLine];
                    LineData last = candidates[endLine];

                    int firstStart = random.Next(0, first.VisibleChars.Count - 2);
                    int lastEnd = random.Next(1, Math.Min(last.VisibleChars.Count, 10));

                    var expectedParts = new List<string>();
                    for (int lineIndex = startLine; lineIndex <= endLine; lineIndex++)
                    {
                        LineData line = candidates[lineIndex];
                        int rowStart = lineIndex == startLine ? line.VisibleChars[firstStart].TextOffset : 0;
                        int rowEnd = lineIndex == endLine ? line.VisibleChars[lastEnd - 1].TextOffset + 1 : line.VisibleChars.Count;

                        bool mapped = line.Line.TryGetHexSelection(rowStart, rowEnd, encoding, out int hexStartOffset, out int hexEndOffset);
                        Assert.IsTrue(mapped, $"Expected row mapping on line {lineIndex}");
                        expectedParts.Add(ExtractHexSpan(line.Line.Hex, hexStartOffset, hexEndOffset));
                    }

                    string expectedHex = string.Join(" ", expectedParts.Where(part => !string.IsNullOrWhiteSpace(part)));
                    Assert.IsFalse(string.IsNullOrWhiteSpace(expectedHex));
                }
            }
        }

        [TestMethod]
        public void RightToLeft_SingleLineRandomHexSelections_SkipAnsiEvenWhenPartiallySelected()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.Line.Hex.Length > 24 && line.VisibleChars.Count >= 6).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(20260504 + pass);
                for (int testIndex = 0; testIndex < 50; testIndex++)
                {
                    LineData line = candidates[random.Next(candidates.Length)];
                    int tokenCount = line.Bytes.Length;
                    int startToken = random.Next(0, tokenCount - 2);
                    int endToken = random.Next(startToken + 1, Math.Min(tokenCount, startToken + 18));

                    int hexStart = HexTokenStartOffset(startToken);
                    int hexEnd = HexTokenEndOffset(endToken - 1);

                    bool mapped = line.Line.TryGetTextSelectionFromHex(hexStart, hexEnd, encoding, out int textStart, out int textEnd);
                    string expected = ExpectedTextForHexRange(line, startToken, endToken, encoding);

                    if (string.IsNullOrEmpty(expected))
                    {
                        Assert.IsFalse(mapped, $"Expected no text for hex range {startToken}-{endToken} in line '{line.Line.Text}'");
                        continue;
                    }

                    Assert.IsTrue(mapped, $"Expected hex->text mapping in line '{line.Line.Text}'");
                    Assert.AreEqual(expected, line.Line.Text.Substring(textStart, textEnd - textStart));
                }
            }
        }

        [TestMethod]
        public void RightToLeft_MultiLineRandomHexSelections_SkipAnsiAndMatchVisibleText()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 5).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(20260505 + pass);
                for (int testIndex = 0; testIndex < 20; testIndex++)
                {
                    int startLine = random.Next(0, candidates.Length - 2);
                    int endLine = random.Next(startLine + 1, Math.Min(candidates.Length, startLine + 4));
                    var expectedText = new StringBuilder();

                    for (int lineIndex = startLine; lineIndex <= endLine; lineIndex++)
                    {
                        LineData line = candidates[lineIndex];
                        int tokenCount = line.Bytes.Length;
                        int startToken = lineIndex == startLine ? random.Next(0, Math.Max(1, tokenCount - 3)) : 0;
                        int endToken = lineIndex == endLine ? random.Next(startToken + 1, tokenCount + 1) : tokenCount;

                        int hexStart = HexTokenStartOffset(startToken);
                        int hexEnd = HexTokenEndOffset(endToken - 1);
                        bool mapped = line.Line.TryGetTextSelectionFromHex(hexStart, hexEnd, encoding, out int textStart, out int textEnd);
                        string expected = ExpectedTextForHexRange(line, startToken, endToken, encoding);

                        if (string.IsNullOrEmpty(expected))
                        {
                            Assert.IsFalse(mapped);
                            continue;
                        }

                        Assert.IsTrue(mapped);
                        Assert.IsTrue(textStart >= 0, $"Negative textStart for line {lineIndex}");
                        Assert.IsTrue(textEnd >= textStart, $"Invalid range {textStart}-{textEnd} for line {lineIndex}");
                        Assert.IsTrue(textEnd <= line.Line.Text.Length, $"Out-of-range textEnd {textEnd} for line length {line.Line.Text.Length} on line {lineIndex}");
                        string actual = line.Line.Text.Substring(textStart, textEnd - textStart);
                        Assert.AreEqual(expected, actual);
                        expectedText.Append(actual);
                    }

                    Assert.IsTrue(expectedText.Length >= 0);
                }
            }
        }

        private static List<LineData> BuildSampleLines(Encoding encoding)
        {
            var index = new CaptureLineIndex();
            var splitter = new RawLineSplitter();
            var parser = new AnsiParser();
            var lines = new List<LineData>();

            foreach (byte[] chunk in TestSample.GetTestChunks())
            {
                foreach (RawLinePart part in splitter.Append(chunk))
                {
                    byte[] textBytes = part.LineEndingLength > 0
                        ? part.Bytes.Take(part.Bytes.Length - part.LineEndingLength).ToArray()
                        : part.Bytes;
                    string ansiText = encoding.GetString(textBytes);
                    string renderedText = string.Concat(parser.Parse(ansiText).Select(segment => segment.Text));
                    string hex = BytesToHex(part.Bytes);
                    LineData current = lines.Count > 0 && index.Count > 0 && !index.GetLine(index.Count - 1).IsComplete
                        ? lines[lines.Count - 1]
                        : new LineData();
                    int byteBase = current.Bytes.Length;
                    int textBase = current.Line == CaptureLineIndex.CaptureLine.Empty ? 0 : current.Line.TextLength;

                    index.AppendLinePart(renderedText, hex, part.IsComplete, part.LineEndingLength, ansiText);

                    if (index.Count > 0)
                    {
                        CaptureLineIndex.CaptureLine line = index.GetLine(index.Count - 1);
                        if (lines.Count < index.Count)
                        {
                            current.Line = line;
                            current.Bytes = (byte[])part.Bytes.Clone();
                            current.EolLength = part.LineEndingLength;
                            current.VisibleChars = BuildVisibleCharMap(part.Bytes, renderedText, encoding, 0, 0);
                            lines.Add(current);
                        }
                        else
                        {
                            current.Line = line;
                            current.Bytes = current.Bytes.Concat(part.Bytes).ToArray();
                            current.EolLength = part.LineEndingLength;
                            current.VisibleChars.AddRange(BuildVisibleCharMap(part.Bytes, renderedText, encoding, byteBase, textBase));
                        }
                    }
                }
            }

            return lines.Where(line => line.Line != null && line.Bytes.Length > 0).ToList();
        }

        private static List<VisibleChar> BuildVisibleCharMap(byte[] bytes, string renderedText, Encoding encoding, int byteBase, int textBase)
        {
            var visible = new List<VisibleChar>();
            int textOffset = 0;
            bool sawEscape = false;

            for (int index = 0; index < bytes.Length; index++)
            {
                byte value = bytes[index];
                if (value == 0x1B)
                {
                    if (!sawEscape && visible.Count > 0)
                    {
                        visible.Clear();
                        textOffset = 0;
                    }
                    sawEscape = true;
                    SkipEscape(bytes, ref index);
                    continue;
                }

                if (value < 0x20 && value != 0x09)
                    continue;

                string decoded = encoding.GetString(new[] { value });
                if (string.IsNullOrEmpty(decoded))
                    continue;

                if (textOffset + decoded.Length > renderedText.Length || string.CompareOrdinal(renderedText, textOffset, decoded, 0, decoded.Length) != 0)
                    continue;

                visible.Add(new VisibleChar(textBase + textOffset, byteBase + index, value));
                textOffset += decoded.Length;
            }

            return visible;
        }

        private static void SkipEscape(byte[] bytes, ref int index)
        {
            if (index + 1 >= bytes.Length)
                return;

            int position = index + 1;
            if (bytes[position] == 0x5B)
            {
                position++;
                while (position < bytes.Length)
                {
                    byte current = bytes[position];
                    if (current >= 0x40 && current <= 0x7E)
                    {
                        index = position;
                        return;
                    }

                    position++;
                }

                index = bytes.Length - 1;
                return;
            }

            index = position;
        }

        private static string ExpectedTextForHexRange(LineData line, int startToken, int endToken, Encoding encoding)
        {
            if (startToken >= endToken)
                return string.Empty;

            VisibleChar[] selected = line.VisibleChars
                .Where(ch => ch.ByteIndex >= startToken && ch.ByteIndex < endToken)
                .OrderBy(ch => ch.TextOffset)
                .ToArray();

            if (selected.Length == 0)
                return string.Empty;

            int start = selected[0].TextOffset;
            int end = selected[selected.Length - 1].TextOffset + 1;
            return line.Line.Text.Substring(start, end - start);
        }

        private static string ExpectedHexForTextRange(LineData line, int startChar, int endChar)
        {
            int firstByte = line.VisibleChars[startChar].ByteIndex;
            if (startChar > 0)
                firstByte = line.VisibleChars[startChar - 1].ByteIndex + 1;
            else
                firstByte = 0;

            int lastByte = line.VisibleChars[endChar].ByteIndex;
            return BytesToHex(line.Bytes
                .Skip(firstByte)
                .Take(lastByte - firstByte + 1));
        }

        private static int HexTokenStartOffset(int tokenIndex)
        {
            return tokenIndex * 3;
        }

        private static int HexTokenEndOffset(int tokenIndex)
        {
            return tokenIndex * 3 + 2;
        }

        private static string ExtractHexSpan(string hex, int startOffset, int endOffset)
        {
            int startIndex = Math.Max(0, Math.Min(startOffset, hex.Length));
            int endIndex = Math.Max(startIndex, Math.Min(endOffset, hex.Length));
            return hex.Substring(startIndex, endIndex - startIndex).Trim();
        }

        private static string BytesToHex(IEnumerable<byte> bytes)
        {
            return string.Join(" ", bytes.Select(value => value.ToString("X2", CultureInfo.InvariantCulture)));
        }
    }
}
