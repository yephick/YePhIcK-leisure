using System.Windows;
using System.Windows.Media;

namespace UartMonitor.Rendering
{
    public sealed class AnsiStyle
    {
        private static readonly Color DefaultForeground = Color.FromRgb(0x00, 0xff, 0x00);
        private static readonly Color DefaultBackground = Color.FromRgb(0x1c, 0x1c, 0x1c);

        public Color? Foreground { get; set; }
        public Color? Background { get; set; }
        public bool Bold { get; set; }
        public bool Underline { get; set; }
        public bool Blink { get; set; }
        public bool Strikeout { get; set; }

        public Brush ForegroundBrush
        {
            get { return CreateBrush(Foreground ?? DefaultForeground); }
        }

        public Brush BackgroundBrush
        {
            get { return CreateBrush(Background ?? DefaultBackground); }
        }

        public FontWeight FontWeight
        {
            get { return Bold ? FontWeights.Bold : FontWeights.Normal; }
        }

        public TextDecorationCollection? TextDecorations
        {
            get
            {
                TextDecorationCollection decorations = new TextDecorationCollection();

                if (Underline)
                    decorations.Add(System.Windows.TextDecorations.Underline[0]);

                if (Strikeout)
                    decorations.Add(System.Windows.TextDecorations.Strikethrough[0]);

                return decorations.Count == 0 ? null : decorations;
            }
        }

        public AnsiStyle Clone()
        {
            return new AnsiStyle
            {
                Foreground = Foreground,
                Background = Background,
                Bold = Bold,
                Underline = Underline,
                Blink = Blink,
                Strikeout = Strikeout
            };
        }

        public void Reset()
        {
            Foreground = null;
            Background = null;
            Bold = false;
            Underline = false;
            Blink = false;
            Strikeout = false;
        }

        private static Brush CreateBrush(Color color)
        {
            SolidColorBrush brush = new SolidColorBrush(color);
            brush.Freeze();
            return brush;
        }
    }
}
