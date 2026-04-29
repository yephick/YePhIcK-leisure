using Microsoft.VisualStudio.TestTools.UnitTesting;
using System.Linq;
using System.Windows.Media;
using UartMonitor.Rendering;
using UartMonitor.Serial;
using UartMonitor;

namespace UartMonitor.Tests.Rendering
{
    [TestClass]
    public sealed class AnsiParserTests
    {
        [TestMethod]
        public void Parse_PlainText_UsesDefaultTerminalColors()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("ready").Single();

            Assert.AreEqual("ready", segment.Text);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.ForegroundBrush);
            AssertBrushColor(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.BackgroundBrush);
            Assert.IsNull(segment.Style.Background);
        }

        [TestMethod]
        public void Parse_SgrForegroundAndXtermBackground_AppliesStyle()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b[32m\x1b[48;5;234mready").Single();

            Assert.AreEqual("ready", segment.Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.Background);
        }

        [TestMethod]
        public void Parse_Sgr256ForegroundAndBackground_StylesRunCorrectly()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b[38;5;196m\x1b[48;5;234mready").Single();

            Assert.AreEqual("ready", segment.Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segment.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.Background);
        }

        [TestMethod]
        public void Parse_StyleCarryOverAcrossSegments_PersistsUntilReset()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mredplain").ToArray();

            Assert.AreEqual(1, segments.Length);
            Assert.AreEqual("redplain", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
        }

        [TestMethod]
        public void Parse_ResetThenNewStyle_DropsPreviousFormatting()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mred\x1b[0mplain\x1b[32mgreen").ToArray();

            Assert.AreEqual(3, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("plain", segments[1].Text);
            Assert.IsNull(segments[1].Style.Foreground);
            Assert.AreEqual("green", segments[2].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[2].Style.Foreground);
        }

        [TestMethod]
        public void Parse_AnsiResetAtChunkBoundary_ResetsStyle()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] firstChunk = parser.Parse("\x1b[31mred\x1b[0").ToArray();
            Assert.AreEqual(1, firstChunk.Length);
            Assert.AreEqual("red", firstChunk[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), firstChunk[0].Style.Foreground);

            LogSegment segment = parser.Parse("mplain").Single();

            Assert.AreEqual("plain", segment.Text);
            Assert.IsNull(segment.Style.Foreground);
        }

        [TestMethod]
        public void Parse_SplitXtermColorSequenceAcrossChunks_StylesText()
        {
            AnsiParser parser = new AnsiParser();

            Assert.AreEqual(0, parser.Parse("\x1b[48;5;23").Count);

            LogSegment segment = parser.Parse("4mready").Single();

            Assert.AreEqual("ready", segment.Text);
            Assert.IsNull(segment.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.Background);
        }

        [TestMethod]
        public void Parse_BackgroundOnly256Color_UsesExpectedColor()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b[48;5;234mready").Single();

            Assert.AreEqual("ready", segment.Text);
            Assert.IsNull(segment.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.Background);
        }

        [TestMethod]
        public void Parse_MalformedSgrParameter_DoesNotBreakText()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mred\x1b[abcplain").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("bcplain", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_IncompleteEscapeAtEnd_IsBufferedUntilCompleted()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] firstPass = parser.Parse("hello \x1b[").ToArray();
            Assert.AreEqual(1, firstPass.Length);
            Assert.AreEqual("hello ", firstPass[0].Text);

            LogSegment segment = parser.Parse("32mworld").Single();

            Assert.AreEqual("world", segment.Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.Foreground);
        }

        [TestMethod]
        public void Parse_InvalidNonControlEscape_IsIgnoredAsLiteralLoss()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("a\x1bXb").Single();

            Assert.AreEqual("aXb", segment.Text);
        }

        [TestMethod]
        public void Parse_UnsupportedOSCSequence_IsIgnored()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b]0;title\x07ready").Single();

            Assert.AreEqual("]0;title\u0007ready", segment.Text);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_BrokenXtermSequence_FallsBackWithoutCrashing()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[38;5mready").ToArray();

            Assert.AreEqual(1, segments.Length);
            Assert.AreEqual("ready", segments[0].Text);
            Assert.IsNull(segments[0].Style.Foreground);
        }

        [TestMethod]
        public void Parse_BadNumericSgrParameter_UsesDefaultReset()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mred\x1b[xyzmplain").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual("yzmplain", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_UnsupportedEscapeSequence_MixedWithStyledText_IsIgnored()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[32mready\x1b[2Jset").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("ready", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("set", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_AdjacentStyledRuns_DoNotMergeIncorrectly()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mred\x1b[32mgreen").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("green", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_ClearStyleFlags_LeaveColorsIntact()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31;1;4mstyled\x1b[22;24mplain").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.IsTrue(segments[0].Style.Bold);
            Assert.IsTrue(segments[0].Style.Underline);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.IsFalse(segments[1].Style.Bold);
            Assert.IsFalse(segments[1].Style.Underline);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_SampleSeedBanner_HasExpectedFirstStyledRun()
        {
            AnsiParser parser = new AnsiParser();

            string sample = UartMonitor.Serial.TestSample.GetTestSample(System.Text.Encoding.GetEncoding(28591));
            LogSegment banner = parser.Parse(sample).First(item => item.Text.StartsWith("UART initialized at verbosity level 8"));

            StringAssert.StartsWith(banner.Text, "UART initialized at verbosity level 8");
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), banner.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), banner.Style.Background);
        }

        [TestMethod]
        public void Parse_SampleSeedAnsiPrefix_PreservesStyledText()
        {
            AnsiParser parser = new AnsiParser();

            string sample = UartMonitor.Serial.TestSample.GetTestSample(System.Text.Encoding.GetEncoding(28591));
            LogSegment segment = parser.Parse(sample).First(item => item.Text.StartsWith("UART initialized at verbosity level 8"));

            StringAssert.StartsWith(segment.Text, "UART initialized at verbosity level 8");
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.Foreground);
            Assert.AreEqual(Color.FromRgb(0x1c, 0x1c, 0x1c), segment.Style.Background);
        }

        [TestMethod]
        public void Parse_SplitEscapeSequence_WaitsForCompleteSequence()
        {
            AnsiParser parser = new AnsiParser();

            Assert.AreEqual(0, parser.Parse("\x1b[31").Count);
            LogSegment segment = parser.Parse("merror").Single();

            Assert.AreEqual("error", segment.Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segment.Style.Foreground);
        }

        [TestMethod]
        public void Parse_Reset_RestoresDefaultStyle()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[31mred\x1b[0mgreen").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("red", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0xff, 0x00, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("green", segments[1].Text);
            Assert.IsNull(segments[1].Style.Foreground);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_TextDecorations_AppliesAndClears()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[1;4;9mstyled\x1b[22;24;29mplain").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.IsTrue(segments[0].Style.Bold);
            Assert.IsTrue(segments[0].Style.Underline);
            Assert.IsTrue(segments[0].Style.Strikeout);
            Assert.IsFalse(segments[1].Style.Bold);
            Assert.IsFalse(segments[1].Style.Underline);
            Assert.IsFalse(segments[1].Style.Strikeout);
        }

        [TestMethod]
        public void Parse_BoldUnderlineStrikeout_ApplyTogether()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b[1;4;9mstyled").Single();

            Assert.AreEqual("styled", segment.Text);
            Assert.IsTrue(segment.Style.Bold);
            Assert.IsTrue(segment.Style.Underline);
            Assert.IsTrue(segment.Style.Strikeout);
            Assert.IsNotNull(segment.Style.TextDecorations);
            Assert.AreEqual(2, segment.Style.TextDecorations.Count);
        }

        [TestMethod]
        public void Parse_AnsiCursorSequence_IsIgnored()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\x1b[32m\x1b[2Jready").Single();

            Assert.AreEqual("ready", segment.Text);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_EscapeReset_IsIgnored()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("\u001bc\x1b[32mready").Single();

            Assert.AreEqual("ready", segment.Text);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segment.Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_CrLfAndCr_AreNormalizedToSingleNewLine()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("a\r\nb\rc").Single();

            Assert.AreEqual("a\nb\nc", segment.Text);
        }

        [TestMethod]
        public void Parse_LfCr_AreNormalizedToSingleNewLine()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment segment = parser.Parse("a\n\rb").Single();

            Assert.AreEqual("a\nb", segment.Text);
        }

        [TestMethod]
        public void Normalize_SplitCrLfAcrossChunks_IsStillSingleNewLine()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a", normalizer.Normalize("a\r", true));
            Assert.AreEqual("\nb", normalizer.Normalize("\nb", true));
        }

        [TestMethod]
        public void Normalize_SplitCarriageReturnAcrossChunks_IsStillSingleNewLine()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a", normalizer.Normalize("a", true));
            Assert.AreEqual("\nb", normalizer.Normalize("\rb", true));
        }

        [TestMethod]
        public void Normalize_SplitLfCrAcrossChunks_IsStillSingleNewLine()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a", normalizer.Normalize("a\n", true));
            Assert.AreEqual("\nb", normalizer.Normalize("\rb", true));
        }

        [TestMethod]
        public void Normalize_StandaloneLineFeeds_PassThrough()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a\nb", normalizer.Normalize("a\nb", true));
        }

        [TestMethod]
        public void TestSample_ContainsExpectedLineBreaks()
        {
            string sample = UartMonitor.Serial.TestSample.GetTestSample(System.Text.Encoding.GetEncoding(28591));

            StringAssert.Contains(sample, "\r\n");
            StringAssert.Contains(sample, "\r");
            StringAssert.Contains(sample, "\n");
            StringAssert.Contains(sample, "\x1b[32m\x1b[48;5;234m\x1b[2JUART initialized at verbosity level 8");
            StringAssert.Contains(sample, "Sample critical error message");
            StringAssert.Contains(sample, "i2c.ixx#342:setupBus()");
            StringAssert.Contains(sample, "pll.ixx#123:_init()");
            StringAssert.Contains(sample, "PLL initialized");
        }

        [TestMethod]
        public void HexChunkFormatter_FormatsHexOnly()
        {
            string formatted = HexChunkFormatter.Format(new byte[] { 0x00, 0x1b, 0x41, 0xff });

            Assert.AreEqual("00 1B 41 FF\r\n", formatted);
            Assert.IsFalse(formatted.Contains("|"));
        }

        private static void AssertBrushColor(Color expected, Brush brush)
        {
            SolidColorBrush solidColorBrush = (SolidColorBrush)brush;
            Assert.AreEqual(expected, solidColorBrush.Color);
        }
    }
}
