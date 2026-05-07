using Microsoft.VisualStudio.TestTools.UnitTesting;
using System;
using System.IO;
using System.Linq;
using System.Windows.Media;
using UartMonitor.Rendering;
using UartMonitor.Serial;
using UartMonitor.Settings;

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
        public void Parse_PrintableTabPrefixBeforeAnsi_IsRendered()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("0\t\x1b[32mready").ToArray();

            Assert.AreEqual(2, segments.Length);
            Assert.AreEqual("0\t", segments[0].Text);
            Assert.IsNull(segments[0].Style.Foreground);
            Assert.AreEqual("ready", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.Foreground);
        }

        [TestMethod]
        public void Parse_ClearScreenAfterPrintablePrefix_RendersMarker()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("0\t\x1b[2Jready").ToArray();

            Assert.AreEqual(3, segments.Length);
            Assert.AreEqual("0\t", segments[0].Text);
            Assert.AreEqual("<ANSI clear screen>", segments[1].Text);
            Assert.AreEqual("ready", segments[2].Text);
            Assert.AreEqual("0\t<ANSI clear screen>ready", string.Concat(segments.Select(segment => segment.Text)));
        }

        [TestMethod]
        public void TabExpansion_ExpandsTabsToConfiguredStopsAndMapsOffsets()
        {
            int column = 0;

            string expanded = TabExpansion.Expand("ab\tc\t", 4, ref column);

            Assert.AreEqual("ab  c   ", expanded);
            Assert.AreEqual(8, column);
            Assert.AreEqual(8, TabExpansion.GetExpandedLength("ab\tc\t", 4));
            Assert.AreEqual(4, TabExpansion.ModelOffsetToExpandedOffset("ab\tc", 3, 4));
            Assert.AreEqual(2, TabExpansion.ExpandedOffsetToModelOffset("ab\tc", 3, 4));
            Assert.AreEqual(3, TabExpansion.ExpandedOffsetToModelOffset("ab\tc", 4, 4));
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
        public void Parse_ClearScreenSequence_MixedWithStyledText_RendersMarker()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[32mready\x1b[2Jset").ToArray();

            Assert.AreEqual(3, segments.Length);
            Assert.AreEqual("ready", segments[0].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[0].Style.Foreground);
            Assert.AreEqual("<ANSI clear screen>", segments[1].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.Foreground);
            Assert.AreEqual("set", segments[2].Text);
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[2].Style.Foreground);
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
        public void Parse_ClearScreenSequence_RendersMarker()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\x1b[32m\x1b[2Jready").ToArray();

            Assert.AreEqual("<ANSI clear screen>ready", string.Concat(segments.Select(item => item.Text)));
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segments[0].Style.ForegroundBrush);
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_EscapeReset_RendersInlineMarker()
        {
            AnsiParser parser = new AnsiParser();

            LogSegment[] segments = parser.Parse("\u001bc\x1b[32mready").ToArray();

            Assert.AreEqual("<ANSI reset terminal>ready", string.Concat(segments.Select(item => item.Text)));
            AssertBrushColor(Color.FromRgb(0x00, 0xff, 0x00), segments[1].Style.ForegroundBrush);
        }

        [TestMethod]
        public void Parse_LeadingBinaryFraming_IsSkippedBeforeFirstRenderableContent()
        {
            AnsiParser parser = new AnsiParser();

            byte[] bytes = new byte[]
            {
                0x00, 0x80, 0x26, 0x01, 0x00, 0x85, 0xB3, 0x4B, 0x2A, 0x02, 0x00, 0x00, 0x00,
                0x1B, 0x63, 0x1B, 0x5B, 0x33, 0x32, 0x6D, 0x1B, 0x5B, 0x34, 0x38, 0x3B, 0x35, 0x3B, 0x32, 0x33, 0x34, 0x6D,
                0x1B, 0x5B, 0x32, 0x4A,
                0x55, 0x41, 0x52, 0x54, 0x20, 0x69, 0x6E, 0x69, 0x74, 0x69, 0x61, 0x6C, 0x69, 0x7A, 0x65, 0x64
            };

            string text = System.Text.Encoding.GetEncoding(28591).GetString(bytes);
            LogSegment[] segments = parser.Parse(text).ToArray();

            Assert.AreEqual("<ANSI reset terminal>", segments[0].Text);
            Assert.AreEqual("<ANSI clear screen>", segments[1].Text);
            StringAssert.StartsWith(segments[2].Text, "UART initialized");
            Assert.AreEqual(Color.FromRgb(0x00, 0xff, 0x00), segments[2].Style.Foreground);
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

            Assert.AreEqual("a", normalizer.Normalize("a\r"));
            Assert.AreEqual("\nb", normalizer.Normalize("\nb"));
        }

        [TestMethod]
        public void Normalize_SplitCarriageReturnAcrossChunks_IsStillSingleNewLine()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a", normalizer.Normalize("a"));
            Assert.AreEqual("\nb", normalizer.Normalize("\rb"));
        }

        [TestMethod]
        public void Normalize_SplitLfCrAcrossChunks_IsStillSingleNewLine()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a", normalizer.Normalize("a\n"));
            Assert.AreEqual("\nb", normalizer.Normalize("\rb"));
        }

        [TestMethod]
        public void Normalize_StandaloneLineFeeds_PassThrough()
        {
            LineEndingNormalizer normalizer = new LineEndingNormalizer();

            Assert.AreEqual("a\nb", normalizer.Normalize("a\nb"));
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
        public void HorizontalScaleFactor_DefaultsToThreeWhenUnknown()
        {
            Assert.AreEqual(3.0, UartMonitor.Serial.AlignmentMath.GetHorizontalScaleFactor(0, 0));
            Assert.AreEqual(3.0, UartMonitor.Serial.AlignmentMath.GetHorizontalScaleFactor(12, 0));
        }

        [TestMethod]
        public void HorizontalScaleFactor_UsesRawToVisibleWidthRatio()
        {
            Assert.AreEqual(2.5, UartMonitor.Serial.AlignmentMath.GetHorizontalScaleFactor(20, 50));
            Assert.AreEqual(4.0, UartMonitor.Serial.AlignmentMath.GetHorizontalScaleFactor(10, 40));
        }

        [TestMethod]
        public void Settings_SaveAndLoad_RoundTripsKeyValues()
        {
            string tempDir = Path.Combine(Path.GetTempPath(), "uartmonitor-tests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tempDir);

            try
            {
                UartMonitorUserSettings.OverrideFilePathForTests = Path.Combine(tempDir, "settings.txt");

                UartMonitorUserSettings settings = new UartMonitorUserSettings
                {
                    PortName = "COM7",
                    BaudRate = "3000000",
                    EncodingDisplayName = "ISO-8859-1: 1998 (Latin-1, West Europe)",
                    FontFamily = "Consolas",
                    FontSize = "16",
                    TabSize = 6,
                    DataBits = 8,
                    StopBits = "1",
                    Parity = "None",
                    FlowControl = "XON/XOFF",
                    AutoScroll = false,
                    HexBytes = false,
                    HexSplitRatio = 0.42,
                    PanelSync = true
                };

                settings.Save();

                UartMonitorUserSettings loaded = UartMonitorUserSettings.Load();

                Assert.AreEqual("COM7", loaded.PortName);
                Assert.AreEqual("3000000", loaded.BaudRate);
                Assert.AreEqual("Consolas", loaded.FontFamily);
                Assert.AreEqual("16", loaded.FontSize);
                Assert.AreEqual(6, loaded.TabSize);
                Assert.AreEqual(8, loaded.DataBits);
                Assert.IsFalse(loaded.AutoScroll);
                Assert.IsFalse(loaded.HexBytes);
                Assert.AreEqual(0.42, loaded.HexSplitRatio);
                Assert.IsTrue(loaded.PanelSync);
            }
            finally
            {
                UartMonitorUserSettings.OverrideFilePathForTests = null;
                try { Directory.Delete(tempDir, true); } catch { }
            }
        }

        private static void AssertBrushColor(Color expected, Brush brush)
        {
            SolidColorBrush solidColorBrush = (SolidColorBrush)brush;
            Assert.AreEqual(expected, solidColorBrush.Color);
        }
    }
}
