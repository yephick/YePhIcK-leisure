namespace UartMonitor.Rendering
{
    public sealed class LogSegment
    {
        public LogSegment(string text, AnsiStyle style)
        {
            Text = text;
            Style = style;
        }

        public string Text { get; }
        public AnsiStyle Style { get; }
    }
}
