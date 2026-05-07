using Microsoft.VisualStudio.Shell;
using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Controls.Primitives;
using System.Windows.Threading;
using UartMonitor.Rendering;
using UartMonitor.Serial;
using UartMonitor.Settings;

namespace UartMonitor
{
    public partial class MyToolWindowControl : UserControl, IDisposable
    {
        private readonly AnsiParser _ansiParser = new AnsiParser();
        private readonly SerialReader _reader = new SerialReader();
        private readonly HexStreamFormatter _hexFormatter = new HexStreamFormatter();
        private readonly RawLineSplitter _rawLineSplitter = new RawLineSplitter();
        private readonly CaptureLineIndex _lineIndex = new CaptureLineIndex();
        private readonly UartMonitorUserSettings _settings = UartMonitorUserSettings.Load();
        private readonly List<TextPointer> _textLineStarts = new List<TextPointer>();
        private readonly List<TextPointer> _hexLineStarts = new List<TextPointer>();
        private readonly List<TextRange> _mirrorHighlights = new List<TextRange>();
        private readonly List<HoverHighlightRange> _hexHoverHighlights = new List<HoverHighlightRange>();
        private ToolTip? _hexHoverToolTip;
        private int _hexHoverLineIndex = -1;
        private int _hexHoverStartOffset = -1;
        private int _hexHoverEndOffset = -1;
        private readonly DispatcherTimer _selectionDebounceTimer;
        private readonly object _processingGate = new object();
        private RichTextBox? _pendingSelectionSource;
        private RichTextBox? _pendingSelectionTarget;
        private bool _syncingScroll;
        private bool _syncingSelection;
        private bool _suppressSelectionEvents;
        private bool _suspendUiRefresh;
        private bool _refreshQueued;
        private Encoding _selectedEncoding = Encoding.UTF8;
        private double _visibleTextColumns;
        private double _hexColumns;

        public MyToolWindowControl()
        {
            InitializeComponent();
            InitializeLogDocument();
            _selectionDebounceTimer = new DispatcherTimer(DispatcherPriority.Background)
            {
                Interval = TimeSpan.FromMilliseconds(100)
            };
            _selectionDebounceTimer.Tick += SelectionDebounceTimer_Tick;
            HexLogBox.MouseMove += HexLogBox_MouseMove;
            HexLogBox.MouseLeave += HexLogBox_MouseLeave;

            BaudComboBox.ItemsSource = new[] { 4608000, 4000000, 3000000, 2000000, 1000000, 921600, 750000, 500000, 460800, 400000, 300000, 225000, 200000, 153600, 115200, 57600, 38400, 19200, 9600 };
            BaudComboBox.Text = _settings.BaudRate;

            EncodingComboBox.ItemsSource = GetEncodings();
            EncodingComboBox.SelectedItem = EncodingComboBox.Items.OfType<EncodingChoice>().FirstOrDefault(choice => string.Equals(choice.DisplayName, _settings.EncodingDisplayName, StringComparison.OrdinalIgnoreCase))
                ?? EncodingComboBox.Items.OfType<EncodingChoice>().FirstOrDefault(choice => choice.Encoding.CodePage == 28591)
                ?? EncodingComboBox.Items.OfType<EncodingChoice>().FirstOrDefault(choice => choice.DisplayName.StartsWith("ISO-8859-1: 1998", StringComparison.OrdinalIgnoreCase));
            EncodingComboBox.SelectionChanged += EncodingComboBox_SelectionChanged;

            FontFamilyComboBox.ItemsSource = GetFontFamilies();
            FontFamilyComboBox.SelectedItem = FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault(choice => choice.FontFamily.Source.Equals(_settings.FontFamily, StringComparison.OrdinalIgnoreCase))
                ?? FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault(choice => choice.FontFamily.Source.Equals("Consolas", StringComparison.OrdinalIgnoreCase))
                ?? FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault(choice => choice.IsMonospace)
                ?? FontFamilyComboBox.Items.OfType<FontFamilyChoice>().FirstOrDefault();

            FontSizeComboBox.ItemsSource = new[] { 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24 };
            FontSizeComboBox.Text = _settings.FontSize;

            TabSizeComboBox.ItemsSource = new[] { 2, 3, 4, 6, 8 };
            TabSizeComboBox.SelectedItem = GetAllowedTabSize(_settings.TabSize);

            DataBitsComboBox.ItemsSource = new[] { 5, 6, 7, 8 };
            DataBitsComboBox.SelectedItem = _settings.DataBits;

            StopBitsComboBox.ItemsSource = new[]
            {
                new StopBitsChoice("1", StopBits.One),
                new StopBitsChoice("1.5", StopBits.OnePointFive),
                new StopBitsChoice("2", StopBits.Two)
            };
            StopBitsComboBox.SelectedItem = StopBitsComboBox.Items.OfType<StopBitsChoice>().FirstOrDefault(choice => choice.DisplayName == _settings.StopBits)
                ?? StopBitsComboBox.Items.OfType<StopBitsChoice>().First();

            ParityComboBox.ItemsSource = Enum.GetValues(typeof(Parity)).Cast<Parity>().ToArray();
            ParityComboBox.SelectedItem = Enum.TryParse(_settings.Parity, out Parity savedParity) ? savedParity : Parity.None;

            FlowControlComboBox.ItemsSource = new[]
            {
                new FlowControlChoice("None", Handshake.None),
                new FlowControlChoice("XON/XOFF", Handshake.XOnXOff),
                new FlowControlChoice("RTS/CTS", Handshake.RequestToSend),
                new FlowControlChoice("RTS/CTS + XON/XOFF", Handshake.RequestToSendXOnXOff)
            };
            FlowControlComboBox.SelectedItem = FlowControlComboBox.Items.OfType<FlowControlChoice>().FirstOrDefault(choice => choice.DisplayName == _settings.FlowControl)
                ?? FlowControlComboBox.Items.OfType<FlowControlChoice>().First(choice => choice.Handshake == Handshake.XOnXOff);

            AutoScrollCheckBox.IsChecked = _settings.AutoScroll;
            HexBytesCheckBox.IsChecked = _settings.HexBytes;
            PanelSyncCheckBox.IsChecked = _settings.PanelSync;

            _reader.ChunkReceived += Reader_ChunkReceived;
            _reader.StatusChanged += Reader_StatusChanged;
            _reader.Error += Reader_Error;

            ApplyFontSettings();
            ApplyHexPaneVisibility();
            UpdateSelectedEncodingSnapshot();
            RefreshPorts();
            ApplySavedPort();
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

        private void TabSizeComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            RefreshRenderedDocuments();
        }

        private void HexBytesCheckBox_CheckedChanged(object sender, RoutedEventArgs e)
        {
            ApplyHexPaneVisibility();
        }

        private void EncodingComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            UpdateSelectedEncodingSnapshot();
        }

        private void LogBox_SelectionChanged(object sender, RoutedEventArgs e)
        {
            if (_syncingSelection || _suppressSelectionEvents)
                return;

            ClearMirrorHighlights();
            ClearSelection(HexLogBox);
            if (LogBox.Selection.IsEmpty || HexBytesCheckBox.IsChecked != true)
                return;

            QueueSelectionSync(LogBox, HexLogBox);
        }

        private void HexLogBox_SelectionChanged(object sender, RoutedEventArgs e)
        {
            if (_syncingSelection || _suppressSelectionEvents)
                return;

            ClearMirrorHighlights();
            ClearSelection(LogBox);
            if (HexLogBox.Selection.IsEmpty)
                return;

            QueueSelectionSync(HexLogBox, LogBox);
        }

        private void SelectionPane_GotKeyboardFocus(object sender, System.Windows.Input.KeyboardFocusChangedEventArgs e)
        {
        }

        private void SelectionPane_PreviewMouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
        }

        private void HexLogBox_MouseMove(object sender, System.Windows.Input.MouseEventArgs e)
        {
            UpdateHexHover(e.GetPosition(HexLogBox));
        }

        private void HexLogBox_MouseLeave(object sender, System.Windows.Input.MouseEventArgs e)
        {
            ClearHexHover();
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
                Encoding = encoding
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
            _lineIndex.Clear();
            _rawLineSplitter.Clear();

            Encoding encoding = GetSelectedEncoding();
            _suspendUiRefresh = true;
            try
            {
                foreach (byte[] chunk in UartMonitor.Serial.TestSample.GetTestChunks())
                    ProcessChunk(chunk, encoding);
            }
            finally
            {
                _suspendUiRefresh = false;
            }

            RefreshRenderedDocuments();

            LogBox.ScrollToEnd();
            HexLogBox.ScrollToEnd();
        }

        private void ClearButton_Click(object sender, RoutedEventArgs e)
        {
            ClearLog();
            _ansiParser.Reset();
            _lineIndex.Clear();
            _rawLineSplitter.Clear();
        }

        private void Reader_ChunkReceived(object sender, SerialReader.ChunkReceivedEventArgs e)
        {
            byte[] chunk = (byte[])e.Bytes.Clone();
            Encoding encoding = _selectedEncoding;
            _ = Task.Run(() =>
            {
                ProcessChunkCore(chunk, encoding);
                RequestUiRefresh();
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

        private void QueueSelectionSync(RichTextBox source, RichTextBox target)
        {
            _pendingSelectionSource = source;
            _pendingSelectionTarget = target;
            _selectionDebounceTimer.Stop();
            _selectionDebounceTimer.Start();
        }

        private void SelectionDebounceTimer_Tick(object? sender, EventArgs e)
        {
            _selectionDebounceTimer.Stop();

            if (_pendingSelectionSource == null || _pendingSelectionTarget == null)
                return;

            SyncSelection(_pendingSelectionSource, _pendingSelectionTarget);
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
            ProcessChunkCore(bytes, encoding);

            if (_suspendUiRefresh)
                return;

            RefreshRenderedDocuments();
        }

        private void ProcessChunkCore(byte[] bytes, Encoding encoding)
        {
            lock (_processingGate)
            {
                foreach (RawLinePart part in _rawLineSplitter.Append(bytes))
                {
                    foreach (RawLinePart logicalPart in SplitClearScreenParts(part))
                    {
                        byte[] textBytes = logicalPart.LineEndingLength > 0
                            ? logicalPart.Bytes.Take(logicalPart.Bytes.Length - logicalPart.LineEndingLength).ToArray()
                            : logicalPart.Bytes;
                        string text = encoding.GetString(textBytes);
                        LogSegment[] segments = _ansiParser.Parse(text).ToArray();
                        string renderedText = string.Concat(segments.Select(segment => segment.Text));
                        string hex = FormatHexLine(logicalPart.Bytes);
                        _lineIndex.AppendLinePart(renderedText, hex, logicalPart.IsComplete, logicalPart.LineEndingLength, text, segments);
                    }
                }
            }
        }

        private static IEnumerable<RawLinePart> SplitClearScreenParts(RawLinePart part)
        {
            byte[] bytes = part.Bytes;
            int start = 0;

            while (start < bytes.Length)
            {
                int clearEnd = FindClearScreenEnd(bytes, start);
                if (clearEnd < 0)
                    break;

                int count = clearEnd - start;
                byte[] clearBytes = new byte[count];
                Buffer.BlockCopy(bytes, start, clearBytes, 0, count);
                yield return new RawLinePart(clearBytes, isComplete: true, lineEndingLength: 0);
                start = clearEnd;
            }

            if (start < bytes.Length)
            {
                byte[] remaining = new byte[bytes.Length - start];
                Buffer.BlockCopy(bytes, start, remaining, 0, remaining.Length);
                yield return new RawLinePart(remaining, part.IsComplete, part.LineEndingLength);
            }
        }

        private static int FindClearScreenEnd(byte[] bytes, int start)
        {
            for (int index = start; index + 3 < bytes.Length; index++)
            {
                if (bytes[index] == 0x1B && bytes[index + 1] == 0x5B && bytes[index + 2] == 0x32 && bytes[index + 3] == 0x4A)
                    return index + 4;
            }

            return -1;
        }

        private void RefreshRenderedDocuments()
        {
            _suppressSelectionEvents = true;
            try
            {
                lock (_processingGate)
                {
                    RebuildDocumentsFromRows();
                    int tabSize = GetSelectedTabSize();
                    _visibleTextColumns = _lineIndex.GetLines().Sum(line => TabExpansion.GetExpandedLength(line.Text, tabSize));
                    _hexColumns = _lineIndex.GetLines().Sum(line => line.HexLength);
                }

                if (AutoScrollCheckBox.IsChecked == true)
                {
                    LogBox.ScrollToEnd();
                    HexLogBox.ScrollToEnd();
                }
            }
            finally
            {
                _suppressSelectionEvents = false;
            }

        }

        private void RequestUiRefresh()
        {
            lock (_processingGate)
            {
                if (_refreshQueued)
                    return;
                _refreshQueued = true;
            }

#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();

                lock (_processingGate)
                    _refreshQueued = false;

                if (_suspendUiRefresh)
                    return;

                RefreshRenderedDocuments();
            });
        }

        private void ClearLog()
        {
            InitializeLogDocument();
            _hexFormatter.Flush();
            _rawLineSplitter.Clear();
        }

        private static string FormatHexLine(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0)
                return string.Empty;

            StringBuilder builder = new StringBuilder(bytes.Length * 3);
            for (int index = 0; index < bytes.Length; index++)
            {
                if (index > 0)
                    builder.Append(' ');

                builder.Append(bytes[index].ToString("X2"));
            }

            return builder.ToString();
        }

        private void RebuildDocumentsFromRows()
        {
            if (LogBox.Document == null || HexLogBox.Document == null)
                InitializeLogDocument();

            FlowDocument textDocument = LogBox.Document!;
            FlowDocument hexDocument = HexLogBox.Document!;

            textDocument.Blocks.Clear();
            hexDocument.Blocks.Clear();
            _mirrorHighlights.Clear();
            _hexHoverHighlights.Clear();

            Paragraph textParagraph = CreateParagraph(LogBox.FontSize);
            Paragraph hexParagraph = CreateParagraph(HexLogBox.FontSize);
            textDocument.Blocks.Add(textParagraph);
            hexDocument.Blocks.Add(hexParagraph);
            _textLineStarts.Clear();
            _hexLineStarts.Clear();

            int renderedTextOffset = 0;
            int renderedHexOffset = 0;
            int tabSize = GetSelectedTabSize();
            foreach (CaptureLineIndex.CaptureLine line in _lineIndex.GetLines())
            {
                line.SetRenderedDocumentStartOffsets(renderedTextOffset, renderedHexOffset);
                Span textSpan = new Span();
                int textColumn = 0;
                foreach (LogSegment segment in line.GetTextRenderSegments())
                {
                    if (string.IsNullOrEmpty(segment.Text))
                        continue;

                    string text = TabExpansion.Expand(segment.Text, tabSize, ref textColumn);
                    Run run = new Run(text);
                    if (segment.Style.Foreground is Color foreground)
                        run.Foreground = CreateBrush(foreground);
                    if (segment.Style.Background is Color background)
                        run.Background = CreateBrush(background);
                    run.FontWeight = segment.Style.FontWeight;
                    run.TextDecorations = segment.Style.TextDecorations;
                    textSpan.Inlines.Add(run);
                }

                Span hexSpan = new Span();
                foreach (CaptureLineIndex.CaptureLine.HexRenderSegment segment in line.GetHexRenderSegments(GetSelectedEncoding()))
                {
                    if (string.IsNullOrEmpty(segment.Text))
                        continue;

                    Run run = new Run(segment.Text);
                    AnsiStyle style = segment.IsAnsiSequence ? CreateDimmedAnsiStyle(segment.Style) : segment.Style;
                    if (style.Foreground is Color foreground)
                        run.Foreground = CreateBrush(foreground);
                    if (style.Background is Color background)
                        run.Background = CreateBrush(background);
                    run.FontWeight = style.FontWeight;
                    run.TextDecorations = style.TextDecorations;
                    hexSpan.Inlines.Add(run);
                }

                textParagraph.Inlines.Add(textSpan);
                hexParagraph.Inlines.Add(hexSpan);
                _textLineStarts.Add(textSpan.ContentStart);
                _hexLineStarts.Add(hexSpan.ContentStart);
                textParagraph.Inlines.Add(new LineBreak());
                hexParagraph.Inlines.Add(new LineBreak());
                renderedTextOffset += TabExpansion.GetExpandedLength(line.Text, tabSize) + 2;
                renderedHexOffset += line.HexLength + 2;
            }
        }

        private void InitializeLogDocument()
        {
            _textLineStarts.Clear();
            _hexLineStarts.Clear();
            _mirrorHighlights.Clear();
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
            InitializeHexDocument(sharedFontSize, sharedLineHeight);
        }

        private void InitializeHexDocument(double sharedFontSize, double sharedLineHeight)
        {
            FlowDocument document = new FlowDocument
            {
                Background = new SolidColorBrush(Color.FromRgb(0x17, 0x17, 0x17)),
                Foreground = new SolidColorBrush(Color.FromRgb(0xd0, 0xd0, 0xd0)),
                PagePadding = new Thickness(0),
                PageWidth = 10000,
                FontFamily = HexLogBox.FontFamily,
                FontSize = sharedFontSize,
                LineStackingStrategy = LineStackingStrategy.BlockLineHeight
            };

            document.Blocks.Add(CreateParagraph(sharedLineHeight));
            HexLogBox.Document = document;
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

        private static AnsiStyle CreateDimmedAnsiStyle(AnsiStyle style)
        {
            AnsiStyle dimmed = style.Clone();
            Color foreground = style.Foreground ?? Color.FromRgb(0xd0, 0xd0, 0xd0);
            dimmed.Foreground = Color.FromRgb(
                (byte)Math.Max(0, foreground.R * 3 / 5),
                (byte)Math.Max(0, foreground.G * 3 / 5),
                (byte)Math.Max(0, foreground.B * 3 / 5));
            dimmed.Bold = false;
            return dimmed;
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
                if (HexLogBox.Document != null)
                    HexLogBox.Document.FontFamily = fontFamily;
            }

            if (TryGetFontSize(out double fontSize))
            {
                LogBox.FontSize = fontSize;
                HexLogBox.FontSize = fontSize;
                if (LogBox.Document != null)
                    LogBox.Document.FontSize = fontSize;
                if (HexLogBox.Document != null)
                    HexLogBox.Document.FontSize = fontSize;
                if (LogBox.Document?.Blocks.LastBlock is Paragraph paragraph)
                    paragraph.LineHeight = fontSize;
                if (HexLogBox.Document?.Blocks.LastBlock is Paragraph hexParagraph)
                    hexParagraph.LineHeight = fontSize;
            }
        }

        private void ApplyHexPaneVisibility()
        {
            if (HexPane == null || HexGridSplitter == null || SplitterColumn == null || HexColumn == null || PanelSyncCheckBox == null)
                return;

            bool showHex = HexBytesCheckBox.IsChecked == true;
            HexPane.Visibility = showHex ? Visibility.Visible : Visibility.Collapsed;
            HexGridSplitter.Visibility = showHex ? Visibility.Visible : Visibility.Collapsed;
            SplitterColumn.Width = showHex ? new GridLength(4) : new GridLength(0);
            HexColumn.Width = showHex ? new GridLength(2, GridUnitType.Star) : new GridLength(0);
            PanelSyncCheckBox.IsEnabled = showHex;

            if (!showHex)
            {
                ClearHexHover();
                ClearMirrorHighlights();
                ClearSelection(HexLogBox);
            }
        }

        public void Dispose()
        {
            SaveSettings();
            _reader.Dispose();
        }

        private void ApplySavedPort()
        {
            if (!string.IsNullOrWhiteSpace(_settings.PortName) && PortComboBox.Items.Contains(_settings.PortName))
                PortComboBox.SelectedItem = _settings.PortName;
        }

        private void SyncSelection(RichTextBox source, RichTextBox target)
        {
            if (_syncingSelection)
                return;

            try
            {
                _syncingSelection = true;
                _syncingScroll = true;
                ScrollViewer? targetScrollViewer = GetScrollViewer(target);
                double targetHorizontalOffset = targetScrollViewer?.HorizontalOffset ?? 0;
                double targetVerticalOffset = targetScrollViewer?.VerticalOffset ?? 0;

                string sourceText = new TextRange(source.Document.ContentStart, source.Document.ContentEnd).Text;
                string targetText = new TextRange(target.Document.ContentStart, target.Document.ContentEnd).Text;
                if (sourceText.Length == 0 || targetText.Length == 0 || _lineIndex.Count == 0)
                    return;

                bool sourceIsHex = ReferenceEquals(source, HexLogBox);
                int sourceStartLineIndex = GetLineIndexFromPointer(source, source.Selection.Start);
                int sourceEndLineIndex = GetLineIndexFromPointer(source, source.Selection.End);

                sourceStartLineIndex = Math.Max(0, Math.Min(sourceStartLineIndex, Math.Max(0, _lineIndex.Count - 1)));
                sourceEndLineIndex = Math.Max(0, Math.Min(sourceEndLineIndex, Math.Max(0, _lineIndex.Count - 1)));
                if (sourceEndLineIndex < sourceStartLineIndex)
                {
                    int temp = sourceStartLineIndex;
                    sourceStartLineIndex = sourceEndLineIndex;
                    sourceEndLineIndex = temp;
                }

                TextPointer? firstTargetPointer = null;
                TextPointer? lastTargetPointer = null;
                var targetHighlightRanges = new List<TextRange>();

                for (int lineIndex = sourceStartLineIndex; lineIndex <= sourceEndLineIndex; lineIndex++)
                {
                    CaptureLineIndex.CaptureLine sourceLine = _lineIndex.GetLine(lineIndex);
                    int lineLength = sourceIsHex ? sourceLine.HexLength : sourceLine.TextLength;
                    int rowStartOffset = lineIndex == sourceStartLineIndex
                        ? GetRowLocalOffset(source, lineIndex, source.Selection.Start)
                        : 0;
                    int rowEndOffset = lineIndex == sourceEndLineIndex
                        ? GetRowLocalOffset(source, lineIndex, source.Selection.End)
                        : lineLength;

                    if (rowEndOffset < rowStartOffset)
                        rowEndOffset = rowStartOffset;
                    bool includeLeadingBytes = !sourceIsHex && rowStartOffset == 0 && sourceStartLineIndex != sourceEndLineIndex;
                    bool includeTrailingBytes = !sourceIsHex && lineIndex < sourceEndLineIndex;
                    bool emptySelectedTextRowWithHex = !sourceIsHex
                        && rowStartOffset == rowEndOffset
                        && sourceStartLineIndex != sourceEndLineIndex
                        && lineIndex > sourceStartLineIndex
                        && lineIndex < sourceEndLineIndex
                        && sourceLine.HexLength > 0;
                    if (rowStartOffset == rowEndOffset && !emptySelectedTextRowWithHex)
                        continue;

                    if (emptySelectedTextRowWithHex)
                    {
                        includeLeadingBytes = true;
                        includeTrailingBytes = true;
                    }

                    if (!TryMapOffsets(sourceLine, sourceIsHex, rowStartOffset, rowEndOffset, includeLeadingBytes, includeTrailingBytes, out int targetStartOffset, out int targetEndOffset))
                        continue;
                    if (targetEndOffset <= targetStartOffset)
                        continue;

                    TextPointer? lineStartPointer = GetLineStartPointer(target, lineIndex);
                    if (lineStartPointer == null)
                        continue;

                    CaptureLineIndex.CaptureLine targetLine = _lineIndex.GetLine(lineIndex);
                    int targetLineLength = sourceIsHex ? targetLine.TextLength : targetLine.HexLength;
                    int safeStart = Math.Max(0, Math.Min(targetStartOffset, targetLineLength));
                    int safeEnd = Math.Max(safeStart, Math.Min(targetEndOffset, targetLineLength));
                    int pointerStart = GetPointerOffset(target, targetLine, safeStart);
                    int pointerEnd = GetPointerOffset(target, targetLine, safeEnd);
                    TextPointer startPointer = TextPointerNavigation.GetAtTextOffset(lineStartPointer, pointerStart);
                    TextPointer endPointer = TextPointerNavigation.GetAtTextOffset(lineStartPointer, pointerEnd);

                    firstTargetPointer ??= startPointer;
                    lastTargetPointer = endPointer;
                    targetHighlightRanges.Add(new TextRange(startPointer, endPointer));
                }

                if (firstTargetPointer == null || lastTargetPointer == null)
                    return;

                ApplyMirrorHighlights(target, targetHighlightRanges);
                targetScrollViewer?.ScrollToHorizontalOffset(targetHorizontalOffset);
                targetScrollViewer?.ScrollToVerticalOffset(targetVerticalOffset);
            }
            finally
            {
                _syncingScroll = false;
                _syncingSelection = false;
            }
        }

        private void ApplyMirrorHighlights(RichTextBox target, IEnumerable<TextRange> ranges)
        {
            ClearMirrorHighlights();
            SolidColorBrush brush = CreateBrush(Color.FromRgb(0x66, 0x66, 0x66));

            foreach (TextRange range in ranges)
            {
                if (range.IsEmpty)
                    continue;

                range.ApplyPropertyValue(TextElement.BackgroundProperty, brush);
                _mirrorHighlights.Add(range);
            }
        }

        private void ClearMirrorHighlights()
        {
            if (_mirrorHighlights.Count == 0)
                return;

            foreach (TextRange range in _mirrorHighlights)
            {
                if (!range.IsEmpty)
                    range.ApplyPropertyValue(TextElement.BackgroundProperty, null);
            }

            _mirrorHighlights.Clear();
        }

        private void UpdateHexHover(Point point)
        {
            TextPointer? pointer = HexLogBox.GetPositionFromPoint(point, snapToText: true);
            if (pointer == null || _lineIndex.Count == 0)
            {
                ClearHexHover();
                return;
            }

            int lineIndex = GetLineIndexFromPointer(HexLogBox, pointer);
            CaptureLineIndex.CaptureLine line = _lineIndex.GetLine(lineIndex);
            int hexOffset = GetRowLocalOffset(HexLogBox, lineIndex, pointer);
            if (!line.TryGetHexHoverInfo(hexOffset, GetSelectedEncoding(), out CaptureLineIndex.CaptureLine.HexHoverInfo hoverInfo))
            {
                ClearHexHover();
                return;
            }

            TextPointer? lineStart = GetLineStartPointer(HexLogBox, lineIndex);
            if (lineStart == null)
                return;

            TextPointer start = TextPointerNavigation.GetAtTextOffset(lineStart, hoverInfo.StartOffset);
            TextPointer end = TextPointerNavigation.GetAtTextOffset(lineStart, hoverInfo.EndOffset);
            if (new TextRange(start, end).IsEmpty)
            {
                ClearHexHover();
                return;
            }

            if (lineIndex == _hexHoverLineIndex && hoverInfo.StartOffset == _hexHoverStartOffset && hoverInfo.EndOffset == _hexHoverEndOffset)
                return;

            ClearHexHover();
            _hexHoverLineIndex = lineIndex;
            _hexHoverStartOffset = hoverInfo.StartOffset;
            _hexHoverEndOffset = hoverInfo.EndOffset;
            ApplyHexHover(new TextRange(start, end));
            ShowHexHoverToolTip(hoverInfo.Tooltip);
        }

        private void ApplyHexHover(TextRange range)
        {
            SolidColorBrush brush = CreateBrush(Color.FromRgb(0x3f, 0x3f, 0x3f));
            object previousBackground = range.GetPropertyValue(TextElement.BackgroundProperty);
            range.ApplyPropertyValue(TextElement.BackgroundProperty, brush);
            _hexHoverHighlights.Add(new HoverHighlightRange(range, previousBackground));
        }

        private void ClearHexHover()
        {
            CloseHexHoverToolTip();
            _hexHoverLineIndex = -1;
            _hexHoverStartOffset = -1;
            _hexHoverEndOffset = -1;

            if (_hexHoverHighlights.Count == 0)
                return;

            foreach (HoverHighlightRange highlight in _hexHoverHighlights)
            {
                if (highlight.Range.IsEmpty)
                    continue;

                object previousBackground = highlight.PreviousBackground == DependencyProperty.UnsetValue
                    ? null!
                    : highlight.PreviousBackground;
                highlight.Range.ApplyPropertyValue(TextElement.BackgroundProperty, previousBackground);
            }

            _hexHoverHighlights.Clear();
        }

        private void ShowHexHoverToolTip(string text)
        {
            CloseHexHoverToolTip();

            ToolTip toolTip = new ToolTip
            {
                Content = text,
                Placement = PlacementMode.Mouse,
                PlacementTarget = HexLogBox,
                StaysOpen = true,
                IsOpen = true
            };

            _hexHoverToolTip = toolTip;
        }

        private void CloseHexHoverToolTip()
        {
            if (_hexHoverToolTip == null)
                return;

            _hexHoverToolTip.IsOpen = false;
            _hexHoverToolTip = null;
        }

        private void ClearSelection(RichTextBox box)
        {
            if (box.Selection.IsEmpty)
                return;

            try
            {
                _suppressSelectionEvents = true;
                TextPointer caret = box.Selection.End;
                box.Selection.Select(caret, caret);
            }
            finally
            {
                _suppressSelectionEvents = false;
            }
        }

        private void SelectOffsets(RichTextBox box, int lineIndex, int rowStartOffset, int rowEndOffset)
        {
            TextPointer? lineStart = GetLineStartPointer(box, lineIndex);
            if (lineStart == null)
                return;

            CaptureLineIndex.CaptureLine line = _lineIndex.GetLine(lineIndex);
            int lineLength = ReferenceEquals(box, HexLogBox) ? line.HexLength : line.TextLength;
            int startOffset = Math.Max(0, Math.Min(rowStartOffset, lineLength));
            int endOffset = Math.Max(startOffset, Math.Min(rowEndOffset, lineLength));
            TextPointer start = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(box, line, startOffset));
            TextPointer end = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(box, line, endOffset));

            if (start != null && end != null)
                box.Selection.Select(start, end);
        }

        private bool TryMapOffsets(CaptureLineIndex.CaptureLine sourceLine, bool sourceIsHex, int rowStartOffset, int rowEndOffset, bool includeLeadingBytes, bool includeTrailingBytes, out int targetStartOffset, out int targetEndOffset)
        {
            if (sourceIsHex)
                return sourceLine.TryGetTextSelectionFromHex(rowStartOffset, rowEndOffset, GetSelectedEncoding(), out targetStartOffset, out targetEndOffset);

            return sourceLine.TryGetHexSelection(rowStartOffset, rowEndOffset, GetSelectedEncoding(), out targetStartOffset, out targetEndOffset, includeLeadingBytes, includeTrailingBytes);
        }

        private int GetRowLocalOffset(RichTextBox box, int lineIndex, TextPointer position)
        {
            TextPointer? lineStart = GetLineStartPointer(box, lineIndex);
            if (lineStart == null)
                return 0;

            if (position.CompareTo(lineStart) <= 0)
                return 0;

            CaptureLineIndex.CaptureLine line = _lineIndex.GetLine(lineIndex);
            int lineLength = ReferenceEquals(box, HexLogBox) ? line.HexLength : line.TextLength;
            int offset = NormalizeRowOffset(new TextRange(lineStart, position).Text);
            if (ReferenceEquals(box, LogBox))
                offset = TabExpansion.ExpandedOffsetToModelOffset(line.Text, offset, GetSelectedTabSize());
            return Math.Max(0, Math.Min(offset, lineLength));
        }

        private int GetLineIndexFromPointer(RichTextBox box, TextPointer position)
        {
            IReadOnlyList<TextPointer> starts = ReferenceEquals(box, HexLogBox) ? _hexLineStarts : _textLineStarts;
            if (starts.Count == 0)
                return 0;

            for (int index = starts.Count - 1; index >= 0; index--)
            {
                if (position.CompareTo(starts[index]) >= 0)
                    return index;
            }

            return 0;
        }

        private static int NormalizeRowOffset(string value)
        {
            if (string.IsNullOrEmpty(value))
                return 0;

            int length = value.Length;
            while (length > 0)
            {
                char tail = value[length - 1];
                if (tail != '\r' && tail != '\n')
                    break;
                length--;
            }

            return length;
        }

        private TextPointer? GetLineStartPointer(RichTextBox box, int lineIndex)
        {
            IReadOnlyList<TextPointer> starts = ReferenceEquals(box, HexLogBox) ? _hexLineStarts : _textLineStarts;
            if (lineIndex < 0 || lineIndex >= starts.Count)
                return null;

            return starts[lineIndex];
        }

        private static string GetRowSelectionText(string lineText, int rowStartOffset, int rowEndOffset)
        {
            if (string.IsNullOrEmpty(lineText))
                return string.Empty;

            int start = Math.Max(0, Math.Min(rowStartOffset, lineText.Length));
            int end = Math.Max(start, Math.Min(rowEndOffset, lineText.Length));
            return lineText.Substring(start, end - start);
        }

        private int GetPointerOffset(RichTextBox box, CaptureLineIndex.CaptureLine line, int rowOffset)
        {
            if (ReferenceEquals(box, LogBox))
                return TabExpansion.ModelOffsetToExpandedOffset(line.Text, rowOffset, GetSelectedTabSize());

            return Math.Max(0, Math.Min(rowOffset, line.HexLength));
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

        private void UpdateSelectedEncodingSnapshot()
        {
            _selectedEncoding = GetSelectedEncoding();
        }

        private int GetSelectedDataBits()
        {
            return DataBitsComboBox.SelectedItem is int dataBits ? dataBits : 8;
        }

        private int GetSelectedTabSize()
        {
            return TabSizeComboBox.SelectedItem is int tabSize ? GetAllowedTabSize(tabSize) : 8;
        }

        private static int GetAllowedTabSize(int value)
        {
            return TabExpansion.GetAllowedTabSize(value);
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

        private void SaveSettings()
        {
            _settings.PortName = PortComboBox.SelectedItem as string;
            _settings.BaudRate = BaudComboBox.Text?.Trim() ?? _settings.BaudRate;
            _settings.EncodingDisplayName = (EncodingComboBox.SelectedItem as EncodingChoice)?.DisplayName ?? _settings.EncodingDisplayName;
            _settings.FontFamily = (FontFamilyComboBox.SelectedItem as FontFamilyChoice)?.FontFamily.Source ?? _settings.FontFamily;
            _settings.FontSize = FontSizeComboBox.Text?.Trim() ?? _settings.FontSize;
            _settings.TabSize = GetSelectedTabSize();
            _settings.DataBits = GetSelectedDataBits();
            _settings.StopBits = (StopBitsComboBox.SelectedItem as StopBitsChoice)?.DisplayName ?? _settings.StopBits;
            _settings.Parity = GetSelectedParity().ToString();
            _settings.FlowControl = (FlowControlComboBox.SelectedItem as FlowControlChoice)?.DisplayName ?? _settings.FlowControl;
            _settings.AutoScroll = AutoScrollCheckBox.IsChecked == true;
            _settings.HexBytes = HexBytesCheckBox.IsChecked == true;
            _settings.PanelSync = PanelSyncCheckBox.IsChecked == true;

            _settings.Save();
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

        private sealed class HoverHighlightRange
        {
            public HoverHighlightRange(TextRange range, object previousBackground)
            {
                Range = range;
                PreviousBackground = previousBackground;
            }

            public TextRange Range { get; }
            public object PreviousBackground { get; }
        }

        private void Pane_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            if (PanelSyncCheckBox.IsChecked != true || HexBytesCheckBox.IsChecked != true)
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
