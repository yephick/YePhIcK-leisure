using Microsoft.VisualStudio.Shell;
using System;
using System.IO.Ports;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Controls.Primitives;
using UartMonitor.Rendering;
using UartMonitor.Serial;

namespace UartMonitor
{
    public partial class MyToolWindowControl : UserControl, IDisposable
    {
        private readonly AnsiParser _ansiParser = new AnsiParser();
        private readonly SerialReader _reader = new SerialReader();
        private bool _syncingScroll;
        private double _visibleTextColumns;
        private double _hexColumns;

        public MyToolWindowControl()
        {
            InitializeComponent();
            InitializeLogDocument();

            BaudComboBox.ItemsSource = new[] { 4608000, 4000000, 3000000, 2000000, 1000000, 921600, 750000, 500000, 460800, 400000, 300000, 225000, 200000, 153600, 115200, 57600, 38400, 19200, 9600 };
            BaudComboBox.Text = "115200";

            EncodingComboBox.ItemsSource = GetEncodings();
            EncodingComboBox.SelectedItem = EncodingComboBox.Items.OfType<EncodingChoice>().FirstOrDefault(choice => choice.Encoding.CodePage == 28591)
                ?? EncodingComboBox.Items.OfType<EncodingChoice>().FirstOrDefault(choice => choice.DisplayName.StartsWith("ISO-8859-1: 1998", StringComparison.OrdinalIgnoreCase));

            FontFamilyComboBox.ItemsSource = GetFontFamilies();
            FontFamilyComboBox.SelectedItem = FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault(choice => choice.FontFamily.Source.Equals("Consolas", StringComparison.OrdinalIgnoreCase))
                ?? FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault(choice => choice.IsMonospace)
                ?? FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault();

            FontSizeComboBox.ItemsSource = new[] { 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24 };
            FontSizeComboBox.Text = "16";

            DataBitsComboBox.ItemsSource = new[] { 5, 6, 7, 8 };
            DataBitsComboBox.SelectedItem = 8;

            StopBitsComboBox.ItemsSource = new[]
            {
                new StopBitsChoice("1", StopBits.One),
                new StopBitsChoice("1.5", StopBits.OnePointFive),
                new StopBitsChoice("2", StopBits.Two)
            };
            StopBitsComboBox.SelectedIndex = 0;

            ParityComboBox.ItemsSource = Enum.GetValues(typeof(Parity)).Cast<Parity>().ToArray();
            ParityComboBox.SelectedItem = Parity.None;

            FlowControlComboBox.ItemsSource = new[]
            {
                new FlowControlChoice("None", Handshake.None),
                new FlowControlChoice("XON/XOFF", Handshake.XOnXOff),
                new FlowControlChoice("RTS/CTS", Handshake.RequestToSend),
                new FlowControlChoice("RTS/CTS + XON/XOFF", Handshake.RequestToSendXOnXOff)
            };
            FlowControlComboBox.SelectedItem = FlowControlComboBox.Items.OfType<FlowControlChoice>().First(choice => choice.Handshake == Handshake.XOnXOff);

            _reader.ChunkReceived += Reader_ChunkReceived;
            _reader.StatusChanged += Reader_StatusChanged;
            _reader.Error += Reader_Error;

            ApplyFontSettings();
            RefreshPorts();
        }

        private void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            RefreshPorts();
        }

        private void FontFamilyComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            ApplyFontSettings();
        }

        private void FontSizeComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            ApplyFontSettings();
        }

        private void FontSizeComboBox_LostFocus(object sender, RoutedEventArgs e)
        {
            ApplyFontSettings();
        }

        private void ConnectButton_Click(object sender, RoutedEventArgs e)
        {
            string? portName = PortComboBox.SelectedItem as string;
            if (string.IsNullOrWhiteSpace(portName))
            {
                SetStatus("No COM port selected");
                return;
            }

            int baudRate = ParseBaudRate();
            Encoding encoding = GetSelectedEncoding();
            int dataBits = GetSelectedDataBits();
            StopBits stopBits = GetSelectedStopBits();
            Parity parity = GetSelectedParity();
            Handshake handshake = GetSelectedHandshake();

            var options = new SerialPortOptions
            {
                PortName = portName!,
                BaudRate = baudRate,
                DataBits = dataBits,
                Parity = parity,
                StopBits = stopBits,
                Handshake = handshake,
                Encoding = encoding,
                MergeLineEndings = MergeLineEndingsCheckBox.IsChecked == true
            };

            try
            {
                _reader.Connect(options);
            }
            catch (Exception ex)
            {
                SetStatus(ex.Message);
            }

            UpdateConnectionButtons();
        }

        private void DisconnectButton_Click(object sender, RoutedEventArgs e)
        {
            _reader.Disconnect();
            UpdateConnectionButtons();
        }

        private void TestButton_Click(object sender, RoutedEventArgs e)
        {
            ClearLog();
            _ansiParser.Reset();
            HexLogBox.Clear();

            Encoding encoding = GetSelectedEncoding();
            foreach (byte[] chunk in UartMonitor.Serial.TestSample.GetTestChunks())
                ProcessChunk(chunk, encoding);

            LogBox.ScrollToEnd();
            HexLogBox.ScrollToEnd();
        }

        private void ClearButton_Click(object sender, RoutedEventArgs e)
        {
            ClearLog();
            HexLogBox.Clear();
            _ansiParser.Reset();
        }

        private void HexClearButton_Click(object sender, RoutedEventArgs e)
        {
            HexLogBox.Clear();
        }

        private void Reader_ChunkReceived(object sender, SerialReader.ChunkReceivedEventArgs e)
        {
#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();

                ProcessChunk(e.Bytes, GetSelectedEncoding());
            });
        }

        private void Reader_StatusChanged(object sender, string status)
        {
#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                SetStatus(status);
                UpdateConnectionButtons();
            });
        }

        private void Reader_Error(object sender, Exception ex)
        {
#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                SetStatus(ex.Message);
                UpdateConnectionButtons();
            });
        }

        private void RefreshPorts()
        {
            string? selected = PortComboBox.SelectedItem as string;
            string[] ports = SerialPort.GetPortNames().OrderBy(port => port).ToArray();

            PortComboBox.ItemsSource = ports;

            if (!string.IsNullOrEmpty(selected) && ports.Contains(selected))
                PortComboBox.SelectedItem = selected;
            else if (ports.Length > 0)
                PortComboBox.SelectedIndex = 0;

            UpdateConnectionButtons();
        }

        private void UpdateConnectionButtons()
        {
            ConnectButton.IsEnabled = !_reader.IsConnected;
            DisconnectButton.IsEnabled = _reader.IsConnected;
        }

        private void SetStatus(string text)
        {
            StatusTextBlock.Text = text;
        }

        private void AppendSegment(LogSegment segment)
        {
            Paragraph paragraph = EnsureActiveParagraph();
            Run run = new Run(segment.Text);

            if (segment.Style.Foreground is Color foreground)
                run.Foreground = CreateBrush(foreground);

            if (segment.Style.Background is Color background)
                run.Background = CreateBrush(background);

            run.FontWeight = segment.Style.FontWeight;
            run.TextDecorations = segment.Style.TextDecorations;

            paragraph.Inlines.Add(run);
        }

        private void ProcessChunk(byte[] bytes, Encoding encoding)
        {
            AppendHexChunk(bytes);

            string text = encoding.GetString(bytes);
            int visibleColumns = 0;
            foreach (LogSegment segment in _ansiParser.Parse(text))
            {
                visibleColumns += segment.Text.Length;
                AppendSegment(segment);
            }

            _visibleTextColumns += visibleColumns;
            _hexColumns += bytes.Length * 3.0;

            if (AutoScrollCheckBox.IsChecked == true)
            {
                LogBox.ScrollToEnd();
                HexLogBox.ScrollToEnd();
            }
        }

        private void AppendHexChunk(byte[] bytes)
        {
            HexLogBox.AppendText(HexChunkFormatter.Format(bytes));
        }

        private void ClearLog()
        {
            InitializeLogDocument();
        }

        private void InitializeLogDocument()
        {
            const double sharedFontSize = 16;
            const double sharedLineHeight = 16;
            FlowDocument document = new FlowDocument
            {
                Background = new SolidColorBrush(Color.FromRgb(0x1c, 0x1c, 0x1c)),
                Foreground = new SolidColorBrush(Color.FromRgb(0x00, 0xff, 0x00)),
                PagePadding = new Thickness(0),
                PageWidth = 10000,
                FontFamily = LogBox.FontFamily,
                FontSize = sharedFontSize,
                LineStackingStrategy = LineStackingStrategy.BlockLineHeight
            };

            document.Blocks.Add(CreateParagraph(sharedLineHeight));
            LogBox.Document = document;
            LogBox.FontSize = sharedFontSize;
            HexLogBox.FontSize = sharedFontSize;
        }

        private Paragraph EnsureActiveParagraph()
        {
            if (LogBox.Document == null)
                InitializeLogDocument();

            FlowDocument document = LogBox.Document!;

            if (document.Blocks.LastBlock is Paragraph paragraph)
                return paragraph;

            paragraph = CreateParagraph(LogBox.FontSize);
            document.Blocks.Add(paragraph);
            return paragraph;
        }

        private static Paragraph CreateParagraph(double lineHeight)
        {
            return new Paragraph
            {
                Margin = new Thickness(0),
                Padding = new Thickness(0),
                LineHeight = lineHeight,
                LineStackingStrategy = LineStackingStrategy.BlockLineHeight
            };
        }

        private static SolidColorBrush CreateBrush(Color color)
        {
            SolidColorBrush brush = new SolidColorBrush(color);
            brush.Freeze();
            return brush;
        }

        private void ApplyFontSettings()
        {
            if (FontFamilyComboBox.SelectedItem is FontFamilyChoice familyChoice)
            {
                FontFamily fontFamily = familyChoice.FontFamily;
                LogBox.FontFamily = fontFamily;
                HexLogBox.FontFamily = fontFamily;
                if (LogBox.Document != null)
                    LogBox.Document.FontFamily = fontFamily;
            }

            if (TryGetFontSize(out double fontSize))
            {
                LogBox.FontSize = fontSize;
                HexLogBox.FontSize = fontSize;
                if (LogBox.Document != null)
                    LogBox.Document.FontSize = fontSize;
                if (LogBox.Document?.Blocks.LastBlock is Paragraph paragraph)
                    paragraph.LineHeight = fontSize;
            }
        }

        public void Dispose()
        {
            _reader.Dispose();
        }

        private int ParseBaudRate()
        {
            if (BaudComboBox.Text != null && int.TryParse(BaudComboBox.Text.Trim(), out int baudRate) && baudRate > 0)
                return baudRate;

            return BaudComboBox.SelectedItem is int selectedBaud ? selectedBaud : 115200;
        }

        private Encoding GetSelectedEncoding()
        {
            if (EncodingComboBox.SelectedItem is EncodingChoice choice)
                return choice.Encoding;

            return Encoding.UTF8;
        }

        private int GetSelectedDataBits()
        {
            return DataBitsComboBox.SelectedItem is int dataBits ? dataBits : 8;
        }

        private StopBits GetSelectedStopBits()
        {
            return StopBitsComboBox.SelectedItem is StopBitsChoice choice ? choice.StopBits : StopBits.One;
        }

        private Parity GetSelectedParity()
        {
            return ParityComboBox.SelectedItem is Parity parity ? parity : Parity.None;
        }

        private Handshake GetSelectedHandshake()
        {
            return FlowControlComboBox.SelectedItem is FlowControlChoice choice ? choice.Handshake : Handshake.XOnXOff;
        }

        private static EncodingChoice[] GetEncodings()
        {
            return Encoding.GetEncodings()
                .Select(info => new EncodingChoice(info.DisplayName, Encoding.GetEncoding(info.CodePage)))
                .OrderBy(choice => choice.DisplayName)
                .ToArray();
        }

        private static FontFamilyChoice[] GetFontFamilies()
        {
            return Fonts.SystemFontFamilies
                .Select(fontFamily => new FontFamilyChoice(fontFamily, IsMonospace(fontFamily)))
                .OrderByDescending(choice => choice.IsMonospace)
                .ThenBy(choice => choice.FontFamily.Source, StringComparer.OrdinalIgnoreCase)
                .ToArray();
        }

        private static bool IsMonospace(FontFamily fontFamily)
        {
            string name = fontFamily.Source.ToLowerInvariant();

            if (name.Contains("mono") || name.Contains("code") || name.Contains("consolas") || name.Contains("courier") || name.Contains("fixed") || name.Contains("terminal") || name.Contains("console"))
                return true;

            return false;
        }

        private bool TryGetFontSize(out double fontSize)
        {
            if (FontSizeComboBox.Text != null && double.TryParse(FontSizeComboBox.Text.Trim(), out fontSize) && fontSize > 0)
                return true;

            if (FontSizeComboBox.SelectedItem is int selectedSize)
            {
                fontSize = selectedSize;
                return true;
            }

            fontSize = 16;
            return false;
        }

        private sealed class EncodingChoice
        {
            public EncodingChoice(string displayName, Encoding encoding)
            {
                DisplayName = displayName;
                Encoding = encoding;
            }

            public string DisplayName { get; }
            public Encoding Encoding { get; }
            public override string ToString() => DisplayName;
        }

        private sealed class StopBitsChoice
        {
            public StopBitsChoice(string displayName, StopBits stopBits)
            {
                DisplayName = displayName;
                StopBits = stopBits;
            }

            public string DisplayName { get; }
            public StopBits StopBits { get; }
            public override string ToString() => DisplayName;
        }

        private sealed class FlowControlChoice
        {
            public FlowControlChoice(string displayName, Handshake handshake)
            {
                DisplayName = displayName;
                Handshake = handshake;
            }

            public string DisplayName { get; }
            public Handshake Handshake { get; }
            public override string ToString() => DisplayName;
        }

        private sealed class FontFamilyChoice
        {
            public FontFamilyChoice(FontFamily fontFamily, bool isMonospace)
            {
                FontFamily = fontFamily;
                IsMonospace = isMonospace;
            }

            public FontFamily FontFamily { get; }
            public bool IsMonospace { get; }
            public override string ToString() => FontFamily.Source;
        }

        private void Pane_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            if (PanelSyncCheckBox.IsChecked != true)
                return;

            if (_syncingScroll)
                return;

            if (e.OriginalSource is not DependencyObject sourceElement)
                return;

            bool sourceIsLog = IsDescendantOf(sourceElement, LogBox);
            bool sourceIsHex = IsDescendantOf(sourceElement, HexLogBox);

            if (!sourceIsLog && !sourceIsHex)
                return;

            ScrollViewer? source = GetScrollViewer(sourceElement);
            ScrollViewer? target = sourceIsLog ? GetScrollViewer(HexLogBox) : GetScrollViewer(LogBox);

            if (source == null || target == null)
                return;

            try
            {
                _syncingScroll = true;
                double horizontalOffset = GetMirroredHorizontalOffset(sourceIsHex, source.HorizontalOffset);
                target.ScrollToHorizontalOffset(Math.Max(horizontalOffset, 0));
                target.ScrollToVerticalOffset(source.VerticalOffset);
            }
            finally
            {
                _syncingScroll = false;
            }
        }

        private double GetMirroredHorizontalOffset(bool sourceIsHex, double sourceOffset)
        {
            double ratio = UartMonitor.Serial.AlignmentMath.GetHorizontalScaleFactor(_visibleTextColumns, _hexColumns);
            return sourceIsHex ? sourceOffset / ratio : sourceOffset * ratio;
        }

        private static ScrollViewer? GetScrollViewer(DependencyObject? root)
        {
            if (root == null)
                return null;

            if (root is ScrollViewer viewer)
                return viewer;

            for (int i = 0, count = VisualTreeHelper.GetChildrenCount(root); i < count; i++)
            {
                ScrollViewer? childViewer = GetScrollViewer(VisualTreeHelper.GetChild(root, i));
                if (childViewer != null)
                    return childViewer;
            }

            return null;
        }

        private static bool IsDescendantOf(DependencyObject child, DependencyObject ancestor)
        {
            DependencyObject? current = child;

            while (current != null)
            {
                if (ReferenceEquals(current, ancestor))
                    return true;

                current = VisualTreeHelper.GetParent(current);
            }

            return false;
        }
    }
}
