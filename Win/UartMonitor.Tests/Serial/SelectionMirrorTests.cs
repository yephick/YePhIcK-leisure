using Microsoft.VisualStudio.TestTools.UnitTesting;
using UartMonitor.Serial;

namespace UartMonitor.Tests.Serial
{
    [TestClass]
    public sealed class SelectionMirrorTests
    {
        [TestMethod]
        public void GetSelectionLineRange_SingleLineSelection_ReturnsSameLine()
        {
            string text = "first\r\nsecond\r\nthird";

            (int startLine, int endLine) = SelectionMirror.GetSelectionLineRange(text, 8, 12);

            Assert.AreEqual(1, startLine);
            Assert.AreEqual(1, endLine);
        }

        [TestMethod]
        public void GetSelectionLineRange_MultiLineSelection_ReturnsSpannedLines()
        {
            string text = "first\r\nsecond\r\nthird";

            (int startLine, int endLine) = SelectionMirror.GetSelectionLineRange(text, 2, 14);

            Assert.AreEqual(0, startLine);
            Assert.AreEqual(1, endLine);
        }

        [TestMethod]
        public void GetLineRangeOffsets_ReturnsExactTextRangeForRequestedLines()
        {
            string text = "first\r\nsecond\r\nthird";

            (int startOffset, int endOffset) = SelectionMirror.GetLineRangeOffsets(text, 1, 1);

            Assert.AreEqual(7, startOffset);
            Assert.AreEqual(15, endOffset);
            Assert.AreEqual("second\r\n", text.Substring(startOffset, endOffset - startOffset));
        }

        [TestMethod]
        public void GetLineRangeOffsets_ClampsOutOfRangeValues()
        {
            string text = "first\r\nsecond\r\nthird";

            (int startOffset, int endOffset) = SelectionMirror.GetLineRangeOffsets(text, -4, 99);

            Assert.AreEqual(0, startOffset);
            Assert.AreEqual(text.Length, endOffset);
        }

        [TestMethod]
        public void GetSelectionLineRange_HandlesSplitCrLfAsOneLineBreak()
        {
            string text = "first\r\nsecond\r\nthird";

            (int startLine, int endLine) = SelectionMirror.GetSelectionLineRange(text, 5, 7);

            Assert.AreEqual(0, startLine);
            Assert.AreEqual(1, endLine);
        }

        [TestMethod]
        public void GetMirroredSelectionOffsets_PreservesRelativeSpanWithinLine()
        {
            string source = "abcd\r\nefgh\r\nijkl";
            string target = "00 11 22 33 44 55\r\n66 77 88 99 AA BB\r\nCC DD EE FF";

            (int startOffset, int endOffset) = SelectionMirror.GetMirroredSelectionOffsets(source, 1, 3, target);

            Assert.IsTrue(startOffset < endOffset);
            StringAssert.Contains(target.Substring(startOffset, endOffset - startOffset), "11");
        }

        [TestMethod]
        public void GetMirroredSelectionOffsets_RangeAcrossLines_ClampsAndPreservesOrder()
        {
            string source = "abcd\r\nefgh\r\nijkl";
            string target = "00 11 22 33\r\n44 55 66 77\r\n88 99 AA BB";

            (int startOffset, int endOffset) = SelectionMirror.GetMirroredSelectionOffsets(source, 2, 10, target);

            Assert.IsTrue(startOffset < endOffset);
            Assert.IsTrue(endOffset <= target.Length);
        }

        [TestMethod]
        public void GetMirroredSelectionOffsets_SingleVisibleCharacter_SelectsMatchingHexByte()
        {
            string source = "231¦ · mft.ixx#75:cmd()\r\n";
            string target = "32 33 31 A6 20 B7 20 6D 66 74 2E 69 78 78 23 37 35 3A 63 6D 64 28 29\r\n";

            (int startOffset, int endOffset) = SelectionMirror.GetMirroredSelectionOffsets(source, 2, 3, target);

            Assert.AreEqual("31", target.Substring(startOffset, 2));
            Assert.IsTrue(endOffset >= startOffset + 2);
            Assert.IsFalse(target.Substring(startOffset, endOffset - startOffset).Contains("1B 5B"));
        }
        [TestMethod]
        public void GetMirroredSelectionOffsets_LastSelectedHexByte_IncludesBothHexDigits()
        {
            string source = "ABC\r\n";
            string target = "41 42 43\r\n";

            (int startOffset, int endOffset) = SelectionMirror.GetMirroredSelectionOffsets(source, 2, 3, target);

            Assert.AreEqual("43", target.Substring(startOffset, endOffset - startOffset));
        }

        [TestMethod]
        public void GetMirroredSelectionOffsets_MultiLineSelection_IncludesRowsBetweenBoundaries()
        {
            string source = "trace\r\n\r\nsetup\r\n";
            string target = "74 72 61 63 65 0D 0A\r\n0D 0A\r\n73 65 74 75 70 0D 0A\r\n";

            (int startOffset, int endOffset) = SelectionMirror.GetMirroredSelectionOffsets(source, 2, 10, target);
            string selected = target.Substring(startOffset, endOffset - startOffset);

            StringAssert.Contains(selected, "0D 0A");
            StringAssert.Contains(selected, "73 65");
        }
    }
}
