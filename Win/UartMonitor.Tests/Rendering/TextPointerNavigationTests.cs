using Microsoft.VisualStudio.TestTools.UnitTesting;
using System;
using System.Threading;
using System.Windows.Documents;
using UartMonitor.Rendering;

namespace UartMonitor.Tests.Rendering
{
    [TestClass]
    public sealed class TextPointerNavigationTests
    {
        [TestMethod]
        public void GetAtTextOffset_UsesVisibleTextOffsetsAcrossMultipleRuns()
        {
            Exception? failure = null;
            Thread thread = new Thread(() =>
            {
                try
                {
                    FlowDocument document = new FlowDocument();
                    Paragraph paragraph = new Paragraph();
                    Span line = new Span();
                    line.Inlines.Add(new Run("0       "));
                    line.Inlines.Add(new Run("02"));
                    paragraph.Inlines.Add(line);
                    document.Blocks.Add(paragraph);

                    TextPointer start = TextPointerNavigation.GetAtTextOffset(line.ContentStart, 8);
                    TextPointer end = TextPointerNavigation.GetAtTextOffset(line.ContentStart, 10);

                    Assert.AreEqual("02", new TextRange(start, end).Text);
                }
                catch (Exception ex)
                {
                    failure = ex;
                }
            });
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start();
            thread.Join();

            if (failure != null)
                throw failure;
        }
    }
}
