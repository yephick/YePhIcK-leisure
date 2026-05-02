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
    [TestCategory("DesignDriven")]
    public sealed class DesignDrivenSelectionMappingTests
    {
        private sealed class LineData
        {
            public CaptureLineIndex.CaptureLine Line { get; set; } = CaptureLineIndex.CaptureLine.Empty;
            public byte[] Bytes { get; set; } = Array.Empty<byte>();
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
        public void Design_LeftToRight_SingleLine_RandomizedRequirements()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 12).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(8100 + pass);
                for (int i = 0; i < 50; i++)
                {
                    LineData line = candidates[random.Next(candidates.Length)];
                    int startChar = random.Next(0, line.VisibleChars.Count - 2);
                    int endChar = random.Next(startChar + 1, Math.Min(line.VisibleChars.Count, startChar + 12));

                    int textStart = line.VisibleChars[startChar].TextOffset;
                    int textEnd = line.VisibleChars[endChar].TextOffset + 1;

                    bool mapped = line.Line.TryGetHexSelection(textStart, textEnd, encoding, out int hexStartOffset, out int hexEndOffset);
                    Assert.IsTrue(mapped, $"No mapping line='{line.Line.Text}' hex='{line.Line.Hex}' textRange={textStart}-{textEnd} selected='{line.Line.Text.Substring(Math.Min(textStart, line.Line.Text.Length), Math.Max(0, Math.Min(textEnd, line.Line.Text.Length) - Math.Min(textStart, line.Line.Text.Length)))}'");

                    string selectedHex = ExtractHexSpan(line.Line.Hex, hexStartOffset, hexEndOffset);
                    string expectedHex = BytesToHex(line.Bytes
                        .Skip(line.VisibleChars[startChar].ByteIndex)
                        .Take(line.VisibleChars[endChar].ByteIndex - line.VisibleChars[startChar].ByteIndex + 1));

                    Assert.AreEqual(expectedHex, selectedHex);
                }
            }
        }

        [TestMethod]
        public void Design_LeftToRight_MultiLine_RandomizedRequirementsWithEol()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 8).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(8200 + pass);
                for (int i = 0; i < 30; i++)
                {
                    int startLine = random.Next(0, candidates.Length - 2);
                    int endLine = random.Next(startLine + 1, Math.Min(candidates.Length, startLine + 5));

                    for (int lineIndex = startLine; lineIndex <= endLine; lineIndex++)
                    {
                        LineData line = candidates[lineIndex];
                        int rowStart = lineIndex == startLine ? line.VisibleChars[random.Next(0, Math.Max(1, line.VisibleChars.Count - 2))].TextOffset : 0;
                        int rowEnd = lineIndex == endLine ? line.VisibleChars[random.Next(Math.Min(1, line.VisibleChars.Count - 1), line.VisibleChars.Count)].TextOffset + 1 : line.VisibleChars.Count;
                        rowEnd = Math.Max(rowStart + 1, rowEnd);

                        bool mapped = line.Line.TryGetHexSelection(rowStart, rowEnd, encoding, out int hexStartOffset, out int hexEndOffset);
                        Assert.IsTrue(mapped, $"No mapping for line {lineIndex}");
                        Assert.IsTrue(hexEndOffset > hexStartOffset, $"Empty mapping for line {lineIndex}");
                    }
                }
            }
        }

        [TestMethod]
        public void Design_RightToLeft_SingleLine_RandomizedRequirements()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 6 && line.Line.HexLength > 24).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(8300 + pass);
                for (int i = 0; i < 60; i++)
                {
                    LineData line = candidates[random.Next(candidates.Length)];
                    int tokenCount = line.Bytes.Length;
                    int startToken = random.Next(0, tokenCount - 2);
                    int endToken = random.Next(startToken + 1, Math.Min(tokenCount, startToken + 20));

                    int hexStart = HexTokenStartOffset(startToken);
                    int hexEnd = HexTokenEndOffset(endToken - 1);

                    bool mapped = line.Line.TryGetTextSelectionFromHex(hexStart, hexEnd, encoding, out int textStart, out int textEnd);
                    string expected = ExpectedTextForHexRange(line, startToken, endToken, encoding);

                    if (string.IsNullOrEmpty(expected))
                    {
                        Assert.IsFalse(mapped, $"Expected no mapping line='{line.Line.Text}' hex='{line.Line.Hex}' tokens={startToken}-{endToken} offsets={hexStart}-{hexEnd} map={textStart}-{textEnd}");
                        continue;
                    }

                    Assert.IsTrue(mapped);
                    Assert.IsTrue(textStart >= 0);
                    Assert.IsTrue(textEnd <= line.Line.Text.Length);
                    string actual = line.Line.Text.Substring(textStart, textEnd - textStart);
                    Assert.AreEqual(expected, actual, $"line='{line.Line.Text}' hex='{line.Line.Hex}' tokens={startToken}-{endToken} offsets={hexStart}-{hexEnd} map={textStart}-{textEnd}");
                }
            }
        }

        [TestMethod]
        public void Design_RightToLeft_MultiLine_RandomizedRequirements()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            List<LineData> lines = BuildSampleLines(encoding);
            LineData[] candidates = lines.Where(line => line.VisibleChars.Count >= 5).ToArray();

            for (int pass = 0; pass < 5; pass++)
            {
                var random = new Random(8400 + pass);
                for (int i = 0; i < 30; i++)
                {
                    int startLine = random.Next(0, candidates.Length - 2);
                    int endLine = random.Next(startLine + 1, Math.Min(candidates.Length, startLine + 5));

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
                            Assert.IsFalse(mapped, $"Expected no mapping lineIndex={lineIndex} line='{line.Line.Text}' hex='{line.Line.Hex}' tokens={startToken}-{endToken} offsets={hexStart}-{hexEnd} map={textStart}-{textEnd}");
                            continue;
                        }

                        Assert.IsTrue(mapped);
                        Assert.IsTrue(textStart >= 0);
                        Assert.IsTrue(textEnd <= line.Line.Text.Length);
                        string actual = line.Line.Text.Substring(textStart, textEnd - textStart);
                        Assert.AreEqual(expected, actual, $"lineIndex={lineIndex} line='{line.Line.Text}' hex='{line.Line.Hex}' tokens={startToken}-{endToken} offsets={hexStart}-{hexEnd} map={textStart}-{textEnd}");
                    }
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

                    if (index.Count == 0)
                        continue;

                    CaptureLineIndex.CaptureLine line = index.GetLine(index.Count - 1);
                    if (lines.Count < index.Count)
                    {
                        current.Line = line;
                        current.Bytes = (byte[])part.Bytes.Clone();
                        current.VisibleChars = BuildVisibleCharMap(part.Bytes, renderedText, encoding, 0, 0);
                        lines.Add(current);
                    }
                    else
                    {
                        current.Line = line;
                        current.Bytes = current.Bytes.Concat(part.Bytes).ToArray();
                        current.VisibleChars.AddRange(BuildVisibleCharMap(part.Bytes, renderedText, encoding, byteBase, textBase));
                    }
                }
            }

            return lines.Where(line => line.Bytes.Length > 0).ToList();
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

        private static int HexTokenStartOffset(int tokenIndex) => tokenIndex * 3;
        private static int HexTokenEndOffset(int tokenIndex) => tokenIndex * 3 + 2;

        private static string ExtractHexSpan(string hex, int startOffset, int endOffset)
        {
            int start = Math.Max(0, Math.Min(startOffset, hex.Length));
            int end = Math.Max(start, Math.Min(endOffset, hex.Length));
            return hex.Substring(start, end - start).Trim();
        }

        private static string BytesToHex(IEnumerable<byte> bytes)
        {
            return string.Join(" ", bytes.Select(b => b.ToString("X2", CultureInfo.InvariantCulture)));
        }
    }
}
