using Microsoft.VisualStudio.TestTools.UnitTesting;
using System;
using System.Linq;
using System.Text;
using System.Windows.Media;
using UartMonitor.Rendering;
using UartMonitor.Serial;

namespace UartMonitor.Tests.Serial
{
    [TestClass]
    public sealed class CaptureLineIndexTests
    {
        [TestMethod]
        public void AppendChunk_GrowsRowsInOrder()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("a\r\nb\r\n", "31 0D 0A\r\n32 0D 0A\r\n");
            index.AppendChunk("c\r\n", "33 0D 0A\r\n");

            Assert.AreEqual(3, index.Count);
            Assert.AreEqual("a", index.GetLine(0).Text);
            Assert.AreEqual("32 0D 0A", index.GetLine(1).Hex);
            Assert.AreEqual("c", index.GetLine(2).Text);
        }

        [TestMethod]
        public void AppendChunk_MergesPartialTrailingLine()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("hello ", "68 65 6C 6C 6F 20");
            index.AppendChunk("world\r\n", "77 6F 72 6C 64 0D 0A");

            Assert.AreEqual(1, index.Count);
            Assert.AreEqual("hello world", index.GetLine(0).Text);
            Assert.IsTrue(index.GetLine(0).IsComplete);
        }

        [TestMethod]
        public void GetLine_ClampsToAvailableRows()
        {
            CaptureLineIndex index = new CaptureLineIndex();
            index.AppendChunk("first\r\nsecond\r\n", "31 0D 0A\r\n32 0D 0A\r\n");

            Assert.AreEqual("first", index.GetLine(-1).Text);
            Assert.AreEqual("second", index.GetLine(99).Text);
        }

        [TestMethod]
        public void FindBestLineIndex_PicksContainingRowNearPreferredIndex()
        {
            CaptureLineIndex index = new CaptureLineIndex();
            index.AppendChunk("231Ã‚Â¦ Ã‚Â· mft.ixx#75:cmd()\r\n4\t31Ã‚Â¦ Ã‚Â· mft.ixx#110:read()\r\n", "32 33 31\r\n34 09 1B\r\n");

            int line = index.FindBestLineIndex("read()", 0);

            Assert.AreEqual(1, line);
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_FindsVisibleByteSpan()
        {
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "31Ã‚Â¦ Ã‚Â· mft.ixx#110:read()",
                "34 09 1B 5B 33 36 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 31 31 30 3A 72 65 61 64 28 29 1B 5B 33 32 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 0A 0D",
                true);

            bool matched = line.TryGetHexSelection("read()", Encoding.UTF8, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("72 65 61 64 28 29", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_IgnoresAnsiPrefix()
        {
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "31Ã‚Â¦ Ã‚Â· mft.ixx#110:read()",
                "34 09 1B 5B 33 36 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 31 31 30 3A 72 65 61 64 28 29 1B 5B 33 32 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 0A 0D",
                true);

            bool matched = line.TryGetHexSelection("3", Encoding.UTF8, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("33", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_MapsTextColumnsWithoutSearchingOtherOccurrences()
        {
            Encoding encoding = Encoding.ASCII;
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "231 | mft.ixx#75:cmd()",
                "32 33 31 20 7C 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29 1B 5B 33 32 6D",
                true);

            int start = line.Text.IndexOf("mft.ixx#", StringComparison.Ordinal);
            bool matched = line.TryGetHexSelection(start, start + 8, encoding, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("6D 66 74 2E 69 78 78 23", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_MapsPlainAsciiAtLineStart()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "267\t31\u00A6 \u00B7 mft.ixx#75:cmd()",
                "32 36 37 09 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29 0A 0D",
                true);

            bool matched = line.TryGetHexSelection(0, 3, encoding, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("32 36 37", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_MapsPlainAsciiAfterTabWithoutDrift()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "267\t31\u00A6 \u00B7 mft.ixx#75:cmd()",
                "32 36 37 09 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29 0A 0D",
                true);

            int start = line.Text.IndexOf("31", StringComparison.Ordinal);
            bool matched = line.TryGetHexSelection(start, start + 2, encoding, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("33 31", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_IncludesHiddenBytesInsideVisibleSelection()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "ABC",
                "41 00 42 07 43",
                true);

            bool matched = line.TryGetHexSelection(0, 3, encoding, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("41 00 42 07 43", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetTextSelectionFromHex_FindsVisibleTextSpan()
        {
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "31Ã‚Â¦ Ã‚Â· mft.ixx#110:read()",
                "34 09 1B 5B 33 36 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 31 31 30 3A 72 65 61 64 28 29 1B 5B 33 32 6D 1B 5B 34 38 3B 35 3B 32 33 34 6D 0A 0D",
                true);

            bool matched = line.TryGetTextSelectionFromHex("72 65 61 64 28 29", Encoding.UTF8, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("read()", line.Text.Substring(startOffset, endOffset - startOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetTextSelectionFromHex_ReturnsFalseForAnsiOnlyBytes()
        {
            Encoding encoding = Encoding.GetEncoding(28591);
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "231Â¦ Â· mft.ixx#75:cmd()",
                "32 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29 1B 5B 33 32 6D",
                true);

            int ansiStart = line.Hex.IndexOf("1B", StringComparison.Ordinal);
            bool matched = line.TryGetTextSelectionFromHex(ansiStart, line.Hex.Length, encoding, out int startOffset, out int endOffset);

            Assert.IsFalse(matched);
            Assert.AreEqual(0, startOffset);
            Assert.AreEqual(0, endOffset);
        }

        [TestMethod]
        public void CaptureLine_ExposesBasicDocumentMetadata()
        {
            var line = new CaptureLineIndex.CaptureLine(7, "hello", "31 32", true);

            Assert.AreEqual(7, line.Index);
            Assert.IsTrue(line.HasText);
            Assert.IsTrue(line.HasHex);
            Assert.AreEqual(5, line.TextLength);
            Assert.AreEqual(5, line.HexLength);
            Assert.IsTrue(line.ContainsText("ell"));
        }

        [TestMethod]
        public void CaptureLine_ReportsStartOfVisibleTextWithinHexLine()
        {
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "231Â¦ Â· mft.ixx#75:cmd()",
                "32 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29",
                true);

            bool matched = line.TryGetHexSelection("cmd()", Encoding.UTF8, out int startOffset, out int endOffset);

            Assert.IsTrue(matched);
            Assert.AreEqual("63 6D 64 28 29", ExtractHexSpan(line.Hex, startOffset, endOffset));
        }

        [TestMethod]
        public void CaptureLine_TryGetHexSelection_ReturnsFalseForEmptySelection()
        {
            var line = new CaptureLineIndex.CaptureLine(0, "hello", "68 65 6C 6C 6F", true);

            bool matched = line.TryGetHexSelection(string.Empty, Encoding.UTF8, out int startOffset, out int endOffset);

            Assert.IsFalse(matched);
            Assert.AreEqual(0, startOffset);
            Assert.AreEqual(0, endOffset);
        }

        [TestMethod]
        public void CaptureLineIndex_TracksLineBreakLengthPerRow()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("a\r\nb\nc\rd", "61 0D 0A 62 0A 63 0D 64");

            Assert.AreEqual(4, index.Count);
            Assert.AreEqual(3, index.GetLine(0).DocumentLength);
            Assert.AreEqual(2, index.GetLine(1).DocumentLength);
            Assert.AreEqual(2, index.GetLine(2).DocumentLength);
            Assert.AreEqual(1, index.GetLine(3).DocumentLength);
            Assert.AreEqual(0, index.GetDocumentOffsetForLine(0));
            Assert.AreEqual(3, index.GetDocumentOffsetForLine(1));
            Assert.AreEqual(5, index.GetDocumentOffsetForLine(2));
            Assert.AreEqual(7, index.GetDocumentOffsetForLine(3));
        }

        [TestMethod]
        public void CaptureLineIndex_LineLocalOffset_IgnoresCurrentRowBreakPadding()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("hello\r\nworld\r\n", "68 65 6C 6C 6F 0D 0A 77 6F 72 6C 64 0D 0A");
            index.GetLine(0).SetRenderedDocumentStartOffset(0);
            index.GetLine(1).SetRenderedDocumentStartOffset(6);

            Assert.AreEqual(0, index.GetRenderedLineLocalOffset(0, 0));
            Assert.AreEqual(5, index.GetRenderedLineLocalOffset(0, 5));
            Assert.AreEqual(1, index.GetRenderedLineLocalOffset(1, 7));
            Assert.AreEqual(5, index.GetRenderedLineLocalOffset(1, 12));
        }

        [TestMethod]
        public void CaptureLineIndex_RenderedOffsets_TrackRebuiltDocumentPositions()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("a\r\nb\r\nc", "61 0D 0A 62 0D 0A 63");

            index.GetLine(0).SetRenderedDocumentStartOffset(0);
            index.GetLine(1).SetRenderedDocumentStartOffset(3);
            index.GetLine(2).SetRenderedDocumentStartOffset(6);

            Assert.AreEqual(0, index.GetRenderedDocumentOffsetForLine(0));
            Assert.AreEqual(3, index.GetRenderedDocumentOffsetForLine(1));
            Assert.AreEqual(6, index.GetRenderedDocumentOffsetForLine(2));
            Assert.AreEqual(0, index.GetLineIndexForRenderedDocumentOffset(0));
            Assert.AreEqual(1, index.GetLineIndexForRenderedDocumentOffset(3));
            Assert.AreEqual(2, index.GetLineIndexForRenderedDocumentOffset(6));
            Assert.AreEqual(0, index.GetLineLocalOffset(0, 0));
            Assert.AreEqual(0, index.GetRenderedLineLocalOffset(1, 3));
            Assert.AreEqual(1, index.GetRenderedLineLocalOffset(2, 7));
        }

        [TestMethod]
        public void CaptureLineIndex_RenderedOffsets_TrackTextAndHexDocumentsSeparately()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendChunk("short\r\nmuch-longer\r\n", "73 68 6F 72 74 0D 0A\r\n6D 75 63 68 2D 6C 6F 6E 67 65 72 0D 0A\r\n");
            index.GetLine(0).SetRenderedDocumentStartOffsets(0, 0);
            index.GetLine(1).SetRenderedDocumentStartOffsets(7, 24);

            Assert.AreEqual(1, index.GetLineIndexForRenderedTextDocumentOffset(7));
            Assert.AreEqual(1, index.GetLineIndexForRenderedHexDocumentOffset(24));
            Assert.AreEqual(0, index.GetRenderedLineLocalOffset(1, 7));
            Assert.AreEqual(0, index.GetRenderedHexLineLocalOffset(1, 24));
        }

        [TestMethod]
        public void RawLineSplitter_AppendsLineEndingBytesToTheCurrentLine()
        {
            var splitter = new RawLineSplitter();

            RawLinePart[] first = new System.Collections.Generic.List<RawLinePart>(splitter.Append(new byte[] { 0x32, 0x33, 0x31 })).ToArray();
            RawLinePart[] second = new System.Collections.Generic.List<RawLinePart>(splitter.Append(new byte[] { 0x0A, 0x0D, 0x34 })).ToArray();

            Assert.AreEqual(1, first.Length);
            Assert.IsFalse(first[0].IsComplete);
            Assert.AreEqual("32 33 31", BytesToHex(first[0].Bytes));
            Assert.AreEqual(2, second.Length);
            Assert.IsTrue(second[0].IsComplete);
            Assert.AreEqual("0A 0D", BytesToHex(second[0].Bytes));
            Assert.AreEqual(2, second[0].LineEndingLength);
            Assert.IsFalse(second[1].IsComplete);
            Assert.AreEqual("34", BytesToHex(second[1].Bytes));
        }

        [TestMethod]
        public void CaptureLineIndex_AppendsRawTextAndHexFragmentsToTheSameLine()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendLinePart("231Â¦ Â· mft.ixx#75:cmd()", "32 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29", false, 0, "231Â¦ Â· mft.ixx#75:cmd()");
            index.AppendLinePart(string.Empty, "0A 0D", true, 2, string.Empty);

            Assert.AreEqual(1, index.Count);
            Assert.AreEqual("231Â¦ Â· mft.ixx#75:cmd()", index.GetLine(0).Text);
            Assert.AreEqual("32 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29 0A 0D", index.GetLine(0).Hex);
        }

        [TestMethod]
        public void CaptureLineIndex_CompleteVisualEmptyLineKeepsOnlyLineEndingHex()
        {
            CaptureLineIndex index = new CaptureLineIndex();

            index.AppendLinePart("before", "62 65 66 6F 72 65 0A 0D", true, 2, "before");
            index.AppendLinePart(string.Empty, "30 09 1B 5B 33 37 6D 1B 5B 33 32 6D 0A 0D", true, 2, string.Empty);
            index.AppendLinePart("after", "61 66 74 65 72 0A 0D", true, 2, "after");

            Assert.AreEqual(3, index.Count);
            Assert.AreEqual(string.Empty, index.GetLine(1).Text);
            Assert.AreEqual("0A 0D", index.GetLine(1).Hex);
        }

        [TestMethod]
        public void CaptureLine_UsesStoredTextSegmentsForRendering()
        {
            AnsiStyle red = new AnsiStyle { Foreground = Color.FromRgb(0xff, 0x00, 0x00) };
            AnsiStyle green = new AnsiStyle { Foreground = Color.FromRgb(0x00, 0xff, 0x00) };
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "redgreen",
                "72 65 64 67 72 65 65 6E",
                true,
                0,
                0,
                0,
                "\x1b[31mred\x1b[32mgreen",
                new[]
                {
                    new LogSegment("red", red),
                    new LogSegment("green", green)
                });

            LogSegment[] segments = line.GetTextRenderSegments().ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("green", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void CaptureLine_HexRenderSegments_ColorVisibleBytesAndMarkAnsiSequences()
        {
            AnsiStyle style = new AnsiStyle
            {
                Foreground = Color.FromRgb(0xff, 0x00, 0x00),
                Background = Color.FromRgb(0x00, 0x00, 0x40)
            };
            var line = new CaptureLineIndex.CaptureLine(
                0,
                "red",
                "1B 5B 33 31 6D 72 65 64",
                true,
                0,
                0,
                0,
                "\x1b[31mred",
                new[] { new LogSegment("red", style) });

            CaptureLineIndex.CaptureLine.HexRenderSegment[] segments = line.GetHexRenderSegments(Encoding.ASCII).ToArray();

            Assert.AreEqual("1B 5B 33 31 6D 72 65 64", string.Concat(segments.Select(segment => segment.Text)));
            Assert.IsTrue(segments[0].IsAnsiSequence);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x00, 0x00, 0x40), segments[0].Style.Background);
            Assert.IsFalse(segments[1].IsAnsiSequence);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[1].Style.Foreground);
        }

        private static string ExtractHexSpan(string hex, int startOffset, int endOffset)
        {
            int startIndex = Math.Max(0, Math.Min(startOffset, hex.Length));
            int endIndex = Math.Max(startIndex, Math.Min(endOffset, hex.Length));
            return hex.Substring(startIndex, endIndex - startIndex).Trim();
        }

        private static string BytesToHex(byte[] bytes)
        {
            return string.Join(" ", Array.ConvertAll(bytes, value => value.ToString("X2")));
        }
    }
}

