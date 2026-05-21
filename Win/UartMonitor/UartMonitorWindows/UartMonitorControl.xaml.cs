using Microsoft.VisualStudio.Shell;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO.Ports;
using System.Management;
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
        private readonly MonitorSettingsState _state;
        private readonly int[] _baudRates = { 4608000, 4000000, 3000000, 2000000, 1000000, 921600, 750000, 500000, 460800, 400000, 300000, 225000, 200000, 153600, 115200, 57600, 38400, 19200, 9600 };
        private readonly int[] _dataBitsOptions = { 5, 6, 7, 8 };
        private readonly int[] _fontSizeOptions = { 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24 };
        private readonly int[] _tabSizeOptions = { 2, 3, 4, 6, 8 };
        private readonly EncodingChoice[] _encodingOptions = GetEncodings();
        private readonly FontFamilyChoice[] _fontFamilyOptions = GetFontFamilies();
        private readonly StopBitsChoice[] _stopBitsOptions =
        {
            new StopBitsChoice("1", StopBits.One),
            new StopBitsChoice("1.5", StopBits.OnePointFive),
            new StopBitsChoice("2", StopBits.Two)
        };
        private readonly FlowControlChoice[] _flowControlOptions =
        {
            new FlowControlChoice("None", Handshake.None),
            new FlowControlChoice("XON/XOFF", Handshake.XOnXOff),
            new FlowControlChoice("RTS/CTS", Handshake.RequestToSend),
            new FlowControlChoice("RTS/CTS + XON/XOFF", Handshake.RequestToSendXOnXOff)
        };
        private readonly List<TextPointer> _textLineStarts = new List<TextPointer>();
        private readonly List<TextPointer> _hexLineStarts = new List<TextPointer>();
        private readonly List<TimestampHoverRange> _timestampHoverRanges = new List<TimestampHoverRange>();
        private readonly List<HoverHighlightRange> _mirrorHighlights = new List<HoverHighlightRange>();
        private readonly List<HoverHighlightRange> _hexHoverHighlights = new List<HoverHighlightRange>();
        private readonly List<HoverHighlightRange> _searchHighlights = new List<HoverHighlightRange>();
        private ToolTip? _hexHoverToolTip;
        private ToolTip? _timestampToolTip;
        private ManagementEventWatcher? _deviceChangeWatcher;
        private Window? _settingsDialog;
        private ComboBox? _settingsPortComboBox;
        private int _hexHoverLineIndex = -1;
        private int _hexHoverStartOffset = -1;
        private int _hexHoverEndOffset = -1;
        private readonly DispatcherTimer _selectionDebounceTimer;
        private readonly DispatcherTimer _portRefreshDebounceTimer;
        private readonly object _processingGate = new object();
        private RichTextBox? _pendingSelectionSource;
        private RichTextBox? _pendingSelectionTarget;
        private bool _syncingScroll;
        private bool _syncingSelection;
        private bool _suppressSelectionEvents;
        private bool _suspendUiRefresh;
        private bool _refreshQueued;
        private Encoding _selectedEncoding = Encoding.UTF8;
        private DateTime _connectTimestamp = DateTime.MinValue;
        private double _visibleTextColumns;
        private double _hexColumns;
        private bool _isDisposed;
        private bool _suppressSearchEvents;
        private string _lastSearchQuery = string.Empty;
        private bool _lastSearchWasHex;
        private int _currentSearchLineIndex = -1;
        private int _currentSearchStartOffset = -1;
        private int _currentSearchEndOffset = -1;

        public MyToolWindowControl()
        {
            _state = MonitorSettingsState.Load(_encodingOptions, _fontFamilyOptions, _stopBitsOptions, _flowControlOptions);
            InitializeComponent();
            InitializeLogDocument();
            _selectionDebounceTimer = new DispatcherTimer(DispatcherPriority.Background)
            {
                Interval = TimeSpan.FromMilliseconds(100)
            };
            _selectionDebounceTimer.Tick += SelectionDebounceTimer_Tick;
            _portRefreshDebounceTimer = new DispatcherTimer(DispatcherPriority.Background)
            {
                Interval = TimeSpan.FromMilliseconds(250)
            };
            _portRefreshDebounceTimer.Tick += PortRefreshDebounceTimer_Tick;
            LogBox.MouseMove += LogBox_MouseMove;
            LogBox.MouseLeave += LogBox_MouseLeave;
            HexLogBox.MouseMove += HexLogBox_MouseMove;
            HexLogBox.MouseLeave += HexLogBox_MouseLeave;

            EncodingComboBox.ItemsSource = _encodingOptions;
            EncodingComboBox.SelectedItem = _state.EncodingChoice;
            EncodingComboBox.SelectionChanged += EncodingComboBox_SelectionChanged;
            TimestampsButton.IsChecked = _state.Timestamps;
            SearchHexCheckBox.IsChecked = false;

            _reader.ChunkReceived += Reader_ChunkReceived;
            _reader.StatusChanged += Reader_StatusChanged;
            _reader.Error += Reader_Error;

#if !DEBUG
            TestButton.Visibility = Visibility.Collapsed;
#endif

            ApplyFontSettings();
            ApplyHexPaneVisibility();
            ApplySavedHexSplitRatio();
            UpdateSelectedEncodingSnapshot();
            RefreshPorts();
            ApplySavedPort();
            StartDeviceChangeWatcher();
        }

        private void SettingsButton_Click(object sender, RoutedEventArgs e)
        {
            ComboBox portComboBox = CreateDialogComboBox(PortComboBox.ItemsSource, _state.PortName, width: 110, toolTip: "COM port used for the UART connection.");
            ComboBox baudComboBox = CreateDialogComboBox(_baudRates, _state.BaudRate, width: 110, isEditable: true, toolTip: "Serial baud rate. This must match the device.");
            ComboBox dataBitsComboBox = CreateDialogComboBox(_dataBitsOptions, _state.DataBits, width: 110, toolTip: "Number of data bits per serial frame.");
            ComboBox stopBitsComboBox = CreateDialogComboBox(_stopBitsOptions, _state.StopBitsChoice, width: 110, toolTip: "Number of stop bits per serial frame.");
            ComboBox parityComboBox = CreateDialogComboBox(Enum.GetValues(typeof(Parity)).Cast<Parity>().ToArray(), _state.Parity, width: 110, toolTip: "Parity mode expected by the device.");
            ComboBox flowControlComboBox = CreateDialogComboBox(_flowControlOptions, _state.FlowControlChoice, width: 160, toolTip: "Hardware/software flow-control mode for the serial port.");
            ComboBox fontFamilyComboBox = CreateDialogComboBox(_fontFamilyOptions, _state.FontFamilyChoice, width: 170, toolTip: "Font used by the text and HEX panes.");
            ComboBox fontSizeComboBox = CreateDialogComboBox(_fontSizeOptions, _state.FontSize, width: 80, isEditable: true, toolTip: "Font size used by the text and HEX panes.");
            ComboBox tabSizeComboBox = CreateDialogComboBox(_tabSizeOptions, _state.TabSize, width: 80, toolTip: "Number of columns used when rendering tab characters.");
            CheckBox timestampsCheckBox = CreateDialogCheckBox("Timestamps", _state.Timestamps, "Show timestamps for captured UART lines.");
            CheckBox autoScrollCheckBox = CreateDialogCheckBox("Auto-scroll", _state.AutoScroll, "Keep the latest received text visible as new data arrives.");
            CheckBox hexBytesCheckBox = CreateDialogCheckBox("HEX bytes", _state.HexBytes, "Show or hide the HEX bytes pane.");
            CheckBox panelSyncCheckBox = CreateDialogCheckBox("Panel sync", _state.PanelSync, "Synchronize scrolling between the text and HEX panes. This can reduce responsiveness on large captures.");
            CheckBox hexToolTipsCheckBox = CreateDialogCheckBox("HEX mouse-over tooltips", _state.HexMouseOverToolTips, "Show byte/ANSI explanations when hovering over the HEX pane.");

            Button refreshButton = new Button
            {
                Content = "Refresh ports",
                MinWidth = 96,
                Margin = new Thickness(6, 0, 0, 0)
            };
            refreshButton.Click += (buttonSender, buttonArgs) =>
            {
                RefreshPorts();
            };

            StackPanel portPanel = new StackPanel { Orientation = Orientation.Horizontal };
            portPanel.Children.Add(portComboBox);
            portPanel.Children.Add(refreshButton);

            TabControl tabs = new TabControl
            {
                MinWidth = 340,
                Margin = new Thickness(0, 0, 0, 10),
                Background = CreateBrush(Color.FromRgb(0x1c, 0x1c, 0x1c)),
                Foreground = CreateBrush(Color.FromRgb(0xf0, 0xf0, 0xf0))
            };
            tabs.Items.Add(CreateSettingsTab("UART", new[]
            {
                    CreateDialogRow("Port:", portPanel, "COM port used for the UART connection."),
                    CreateDialogRow("Baud:", baudComboBox, "Serial baud rate. This must match the device."),
                    CreateDialogRow("Data bits:", dataBitsComboBox, "Number of data bits per serial frame."),
                    CreateDialogRow("Stop bits:", stopBitsComboBox, "Number of stop bits per serial frame."),
                    CreateDialogRow("Parity:", parityComboBox, "Parity mode expected by the device."),
                    CreateDialogRow("Flow control:", flowControlComboBox, "Hardware/software flow-control mode for the serial port.")
                }));
            tabs.Items.Add(CreateSettingsTab("Text", new FrameworkElement[]
            {
                CreateDialogRow("Font:", fontFamilyComboBox, "Font used by the text and HEX panes."),
                CreateDialogRow("Size:", fontSizeComboBox, "Font size used by the text and HEX panes."),
                CreateDialogRow("Tab width:", tabSizeComboBox, "Number of columns used when rendering tab characters."),
                timestampsCheckBox
            }));
            tabs.Items.Add(CreateSettingsTab("Behavior", new FrameworkElement[]
            {
                autoScrollCheckBox,
                hexBytesCheckBox,
                panelSyncCheckBox,
                hexToolTipsCheckBox
            }));

            Window dialog = CreateSettingsDialog("Settings", tabs);
            _settingsDialog = dialog;
            _settingsPortComboBox = portComboBox;
            try
            {
                if (dialog.ShowDialog() != true)
                    return;

                _state.PortName = portComboBox.SelectedItem as string;
                if (!string.IsNullOrWhiteSpace(_state.PortName))
                    PortComboBox.SelectedItem = _state.PortName;
                _state.BaudRate = baudComboBox.Text?.Trim() ?? _state.BaudRate;
                _state.DataBits = dataBitsComboBox.SelectedItem is int dataBits ? dataBits : _state.DataBits;
                _state.StopBitsChoice = stopBitsComboBox.SelectedItem as StopBitsChoice ?? _state.StopBitsChoice;
                _state.Parity = parityComboBox.SelectedItem is Parity parity ? parity : _state.Parity;
                _state.FlowControlChoice = flowControlComboBox.SelectedItem as FlowControlChoice ?? _state.FlowControlChoice;
                _state.FontFamilyChoice = fontFamilyComboBox.SelectedItem as FontFamilyChoice ?? _state.FontFamilyChoice;
                _state.FontSize = fontSizeComboBox.Text?.Trim() ?? _state.FontSize;
                _state.TabSize = tabSizeComboBox.SelectedItem is int tabSize ? GetAllowedTabSize(tabSize) : _state.TabSize;
                _state.AutoScroll = autoScrollCheckBox.IsChecked == true;
                _state.HexBytes = hexBytesCheckBox.IsChecked == true;
                _state.PanelSync = panelSyncCheckBox.IsChecked == true;
                _state.HexMouseOverToolTips = hexToolTipsCheckBox.IsChecked == true;
                _state.Timestamps = timestampsCheckBox.IsChecked == true;
                _state.Save();

                ApplyStateToQuickControls();
                ApplyFontSettings();
                ApplyHexPaneVisibility();
                RefreshRenderedDocuments();
            }
            finally
            {
                _settingsPortComboBox = null;
                _settingsDialog = null;
            }
        }
        private ComboBox CreateDialogComboBox(System.Collections.IEnumerable? itemsSource, object? selectedItem, double width, bool isEditable = false, string? toolTip = null)
        {
            ComboBox comboBox = new ComboBox
            {
                ItemsSource = itemsSource,
                SelectedItem = selectedItem,
                Width = width,
                MinHeight = 22,
                IsEditable = isEditable,
                FontSize = 11,
                ToolTip = toolTip
            };

            if (isEditable && selectedItem is string text)
                comboBox.Text = text;

            return comboBox;
        }

        private CheckBox CreateDialogCheckBox(string text, bool isChecked, string toolTip)
        {
            return new CheckBox
            {
                Content = text,
                IsChecked = isChecked,
                Foreground = CreateBrush(Color.FromRgb(0xd6, 0xd6, 0xd6)),
                Margin = new Thickness(0, 0, 0, 8),
                FontSize = 11,
                ToolTip = toolTip
            };
        }

        private TabItem CreateSettingsTab(string header, IEnumerable<FrameworkElement> rows)
        {
            StackPanel content = new StackPanel { Margin = new Thickness(10) };
            foreach (FrameworkElement row in rows)
                content.Children.Add(row);

            return new TabItem
            {
                Header = header,
                Content = content
            };
        }

        private FrameworkElement CreateDialogRow(string label, UIElement editor, string toolTip)
        {
            Grid row = new Grid { Margin = new Thickness(0, 0, 0, 8), ToolTip = toolTip };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(84) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            TextBlock textBlock = new TextBlock
            {
                Text = label,
                Foreground = CreateBrush(Color.FromRgb(0xd6, 0xd6, 0xd6)),
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 8, 0)
            };
            Grid.SetColumn(textBlock, 0);
            row.Children.Add(textBlock);

            Grid.SetColumn(editor, 1);
            row.Children.Add(editor);
            return row;
        }

        private Window CreateSettingsDialog(string title, UIElement body)
        {
            StackPanel content = new StackPanel { Margin = new Thickness(12) };
            content.Children.Add(body);

            StackPanel buttons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Right,
                Margin = new Thickness(0, 6, 0, 0)
            };
            Button okButton = CreateDialogButton("OK");
            okButton.Margin = new Thickness(0, 0, 6, 0);
            okButton.IsDefault = true;
            Button cancelButton = CreateDialogButton("Cancel");
            cancelButton.IsCancel = true;
            buttons.Children.Add(okButton);
            buttons.Children.Add(cancelButton);
            content.Children.Add(buttons);

            TextBlock titleText = new TextBlock
            {
                Text = title,
                Foreground = CreateBrush(Color.FromRgb(0xf0, 0xf0, 0xf0)),
                FontWeight = FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(10, 0, 0, 0)
            };
            Button closeButton = CreateDialogButton("X");
            closeButton.Width = 28;
            closeButton.MinWidth = 28;
            closeButton.Height = 24;
            closeButton.HorizontalAlignment = HorizontalAlignment.Right;

            DockPanel titleBar = new DockPanel
            {
                Height = 32,
                Background = CreateBrush(Color.FromRgb(0x2a, 0x2a, 0x2a)),
                LastChildFill = true
            };
            DockPanel.SetDock(closeButton, Dock.Right);
            titleBar.Children.Add(closeButton);
            titleBar.Children.Add(titleText);

            Border border = new Border
            {
                Background = CreateBrush(Color.FromRgb(0x1c, 0x1c, 0x1c)),
                BorderBrush = CreateBrush(Color.FromRgb(0x4a, 0x4a, 0x4a)),
                BorderThickness = new Thickness(1)
            };
            DockPanel root = new DockPanel();
            DockPanel.SetDock(titleBar, Dock.Top);
            root.Children.Add(titleBar);
            root.Children.Add(content);
            border.Child = root;

            Window dialog = new Window
            {
                Title = title,
                Content = border,
                Owner = Window.GetWindow(this),
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                SizeToContent = SizeToContent.WidthAndHeight,
                ResizeMode = ResizeMode.NoResize,
                WindowStyle = WindowStyle.None,
                Background = CreateBrush(Color.FromRgb(0x1c, 0x1c, 0x1c))
            };
            okButton.Click += (buttonSender, buttonArgs) => dialog.DialogResult = true;
            closeButton.Click += (buttonSender, buttonArgs) => dialog.DialogResult = false;
            titleBar.MouseLeftButtonDown += (titleSender, titleArgs) => dialog.DragMove();
            return dialog;
        }

        private Button CreateDialogButton(string text)
        {
            return new Button
            {
                Content = text,
                MinWidth = 72,
                Padding = new Thickness(8, 2, 8, 2),
                Foreground = CreateBrush(Color.FromRgb(0xf0, 0xf0, 0xf0)),
                Background = CreateBrush(Color.FromRgb(0x2a, 0x2a, 0x2a)),
                BorderBrush = CreateBrush(Color.FromRgb(0x5a, 0x5a, 0x5a))
            };
        }

        private void HexGridSplitter_DragCompleted(object sender, DragCompletedEventArgs e)
        {
            SaveHexSplitRatio();
        }

        private void EncodingComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            if (EncodingComboBox.SelectedItem is EncodingChoice choice)
                _state.EncodingChoice = choice;

            UpdateSelectedEncodingSnapshot();
        }

        private void TimestampsButton_Click(object sender, RoutedEventArgs e)
        {
            _state.Timestamps = TimestampsButton.IsChecked == true;
            _state.Save();
            RefreshRenderedDocuments();
        }

        private void SearchButton_Click(object sender, RoutedEventArgs e)
        {
            SearchBoxBorder.Visibility = SearchBoxBorder.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;
            if (SearchBoxBorder.Visibility == Visibility.Visible)
            {
                SearchTextBox.Focus();
                SearchTextBox.SelectAll();
                if (!string.IsNullOrWhiteSpace(SearchTextBox.Text))
                    PerformSearch(NormalizeSearchQuery(SearchTextBox.Text, SearchHexCheckBox.IsChecked == true), SearchHexCheckBox.IsChecked == true, true, restart: true);
            }
            else
            {
                ClearSearchSelection();
            }
        }

        private void SearchTextBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
        {
            if (_suppressSearchEvents)
                return;

            bool searchHex = SearchHexCheckBox.IsChecked == true;
            string query = NormalizeSearchQuery(SearchTextBox.Text, searchHex);
            if (searchHex)
                query = query.ToUpperInvariant();
            if (!string.Equals(query, SearchTextBox.Text, StringComparison.Ordinal))
            {
                _suppressSearchEvents = true;
                try
                {
                    int caretIndex = Math.Min(query.Length, SearchTextBox.CaretIndex);
                    SearchTextBox.Text = query;
                    SearchTextBox.CaretIndex = caretIndex;
                }
                finally
                {
                    _suppressSearchEvents = false;
                }
            }

            if (string.IsNullOrWhiteSpace(query))
                ClearSearchSelection();
            else
                PerformSearch(query, searchHex, true, restart: HasSearchContextChanged(query, searchHex));
        }

        private void SearchTextBox_KeyDown(object sender, System.Windows.Input.KeyEventArgs e)
        {
            if (e.Key == System.Windows.Input.Key.Escape)
            {
                _suppressSearchEvents = true;
                try
                {
                    SearchTextBox.Clear();
                }
                finally
                {
                    _suppressSearchEvents = false;
                }

                SearchBoxBorder.Visibility = Visibility.Collapsed;
                ClearSearchSelection();
                e.Handled = true;
                return;
            }

            if (e.Key == System.Windows.Input.Key.Enter)
            {
                bool searchHex = SearchHexCheckBox.IsChecked == true;
                string query = NormalizeSearchQuery(SearchTextBox.Text, searchHex);
                if (searchHex)
                    query = query.ToUpperInvariant();
                PerformSearch(query, searchHex, true, restart: HasSearchContextChanged(query, searchHex));
                e.Handled = true;
            }
        }

        private void LogBox_SelectionChanged(object sender, RoutedEventArgs e)
        {
            if (_syncingSelection || _suppressSelectionEvents)
                return;

            ClearMirrorHighlights();
            ClearSelection(HexLogBox);
            if (LogBox.Selection.IsEmpty || !_state.HexBytes)
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

        private void LogBox_MouseMove(object sender, System.Windows.Input.MouseEventArgs e)
        {
            UpdateTimestampHover(e.GetPosition(LogBox));
        }

        private void LogBox_MouseLeave(object sender, System.Windows.Input.MouseEventArgs e)
        {
            ClearTimestampHover();
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
                _connectTimestamp = DateTime.Now;
                _reader.Connect(options);
                _state.PortName = portName;
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
            _connectTimestamp = DateTime.Now;

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
            DateTime timestamp = DateTime.Now;
            _ = Task.Run(() =>
            {
                ProcessChunkCore(chunk, encoding, timestamp);
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
            string? nextSelection = PortSelection.ChoosePortSelection(selected, _state.PortName, ports);

            PortComboBox.ItemsSource = ports;

            if (!string.IsNullOrEmpty(nextSelection))
                PortComboBox.SelectedItem = nextSelection;
            else if (ports.Length > 0)
                PortComboBox.SelectedIndex = 0;
            else
                PortComboBox.SelectedItem = null;

            _state.PortName = PortComboBox.SelectedItem as string ?? _state.PortName;
            SyncSettingsDialogPorts(ports);
            UpdateConnectionButtons();
        }

        private void SyncSettingsDialogPorts(string[] ports)
        {
            if (_settingsDialog == null || _settingsPortComboBox == null || !_settingsDialog.IsVisible)
                return;

            string? selected = _settingsPortComboBox.SelectedItem as string;
            string? nextSelection = PortSelection.ChoosePortSelection(selected, PortComboBox.SelectedItem as string ?? _state.PortName, ports);
            _settingsPortComboBox.ItemsSource = ports;

            if (!string.IsNullOrEmpty(nextSelection))
                _settingsPortComboBox.SelectedItem = nextSelection;
            else if (ports.Length > 0)
                _settingsPortComboBox.SelectedIndex = 0;
            else
                _settingsPortComboBox.SelectedItem = null;
        }

        private void UpdateConnectionButtons()
        {
            ConnectButton.IsEnabled = !_reader.IsConnected;
            DisconnectButton.IsEnabled = _reader.IsConnected;
        }

        private void SetStatus(string text)
        {
            ConnectButton.ToolTip = string.IsNullOrWhiteSpace(text) ? "Connect" : $"Connect ({text})";
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
            ProcessChunkCore(bytes, encoding, DateTime.Now);

            if (_suspendUiRefresh)
                return;

            RefreshRenderedDocuments();
        }

        private void ProcessChunkCore(byte[] bytes, Encoding encoding, DateTime timestamp)
        {
            lock (_processingGate)
            {
                foreach (RawLinePart part in _rawLineSplitter.Append(bytes, timestamp))
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
                        DateTime lineTimestamp = _lineIndex.Count == 0
                            ? GetLineZeroTimestamp(logicalPart.Timestamp)
                            : GetLineTimestamp(logicalPart.Timestamp);
                        _lineIndex.AppendLinePart(renderedText, hex, logicalPart.IsComplete, logicalPart.LineEndingLength, text, segments, lineTimestamp);
                    }
                }
            }
        }

        private DateTime GetLineZeroTimestamp(DateTime fallback)
        {
            if (_connectTimestamp != DateTime.MinValue)
                return _connectTimestamp;

            return GetLineTimestamp(fallback);
        }

        private static DateTime GetLineTimestamp(DateTime fallback)
        {
            return fallback == DateTime.MinValue ? DateTime.Now : fallback;
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
                yield return new RawLinePart(clearBytes, isComplete: true, lineEndingLength: 0, part.Timestamp);
                start = clearEnd;
            }

            if (start < bytes.Length)
            {
                byte[] remaining = new byte[bytes.Length - start];
                Buffer.BlockCopy(bytes, start, remaining, 0, remaining.Length);
                yield return new RawLinePart(remaining, part.IsComplete, part.LineEndingLength, part.Timestamp);
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

                if (_state.AutoScroll)
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
            _searchHighlights.Clear();
            _timestampHoverRanges.Clear();

            Paragraph textParagraph = CreateParagraph(LogBox.FontSize);
            Paragraph hexParagraph = CreateParagraph(HexLogBox.FontSize);
            textDocument.Blocks.Add(textParagraph);
            hexDocument.Blocks.Add(hexParagraph);
            _textLineStarts.Clear();
            _hexLineStarts.Clear();

            int renderedTextOffset = 0;
            int renderedHexOffset = 0;
            int tabSize = GetSelectedTabSize();
            DateTime previousTimestamp = DateTime.MinValue;
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

                if (_state.Timestamps)
                {
                    AddTimestampPrefix(textParagraph, line, previousTimestamp);
                    previousTimestamp = GetDisplayTimestamp(line);
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

        private void AddTimestampPrefix(Paragraph paragraph, CaptureLineIndex.CaptureLine line, DateTime previousTimestamp)
        {
            DateTime timestamp = GetDisplayTimestamp(line);
            DateTime previous = previousTimestamp == DateTime.MinValue ? timestamp : previousTimestamp;
            TimeSpan increment = timestamp - previous;
            if (increment < TimeSpan.Zero)
                increment = TimeSpan.Zero;

            Span timestampSpan = new Span();
            Run timestampRun = new Run(FormatTimestampDelta(increment))
            {
                FontSize = Math.Max(8, LogBox.FontSize - 2),
                Foreground = CreateBrush(Color.FromRgb(0x8a, 0x8a, 0x8a))
            };
            timestampSpan.Inlines.Add(timestampRun);
            paragraph.Inlines.Add(timestampSpan);
            paragraph.Inlines.Add(new Run(" "));
            _timestampHoverRanges.Add(new TimestampHoverRange(timestampSpan.ContentStart, timestampSpan.ContentEnd, FormatTimestampToolTip(timestamp, increment)));
        }

        private static DateTime GetDisplayTimestamp(CaptureLineIndex.CaptureLine line)
        {
            return line.Timestamp == DateTime.MinValue ? DateTime.Now : line.Timestamp;
        }

        private static string FormatTimestampDelta(TimeSpan increment)
        {
            long milliseconds = Math.Max(0, (long)Math.Round(increment.TotalMilliseconds));
            if (milliseconds > 99999)
                return "+++++";

            return milliseconds.ToString(CultureInfo.InvariantCulture).PadLeft(5);
        }

        private static string FormatTimestampToolTip(DateTime timestamp, TimeSpan increment)
        {
            long milliseconds = Math.Max(0, (long)Math.Round(increment.TotalMilliseconds));
            return string.Format(
                CultureInfo.InvariantCulture,
                "Timestamp: {0:yyyy-MM-dd HH:mm:ss.fff}\nIncrement: +{1} ms",
                timestamp,
                milliseconds);
        }

        private void InitializeLogDocument()
        {
            _textLineStarts.Clear();
            _hexLineStarts.Clear();
            _mirrorHighlights.Clear();
            _searchHighlights.Clear();

            // Clear timestamp hover ranges whose TextPointers belong to any prior document.
            _timestampHoverRanges.Clear();
            CloseTimestampToolTip();

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
            FontFamily fontFamily = _state.FontFamilyChoice.FontFamily;
            LogBox.FontFamily = fontFamily;
            HexLogBox.FontFamily = fontFamily;
            if (LogBox.Document != null)
                LogBox.Document.FontFamily = fontFamily;
            if (HexLogBox.Document != null)
                HexLogBox.Document.FontFamily = fontFamily;

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
            if (TextColumn == null || HexPane == null || HexGridSplitter == null || SplitterColumn == null || HexColumn == null)
                return;

            bool showHex = _state.HexBytes;
            HexPane.Visibility = showHex ? Visibility.Visible : Visibility.Collapsed;
            HexGridSplitter.Visibility = showHex ? Visibility.Visible : Visibility.Collapsed;
            SplitterColumn.Width = showHex ? new GridLength(4) : new GridLength(0);
            if (showHex)
                ApplySavedHexSplitRatio();
            else
                HexColumn.Width = new GridLength(0);
            if (!showHex)
            {
                ClearHexHover();
                ClearMirrorHighlights();
                ClearSelection(HexLogBox);
            }
        }

        private void ApplySavedHexSplitRatio()
        {
            if (TextColumn == null || HexColumn == null || !_state.HexBytes)
                return;

            double ratio = Math.Max(0.15, Math.Min(0.85, _state.HexSplitRatio));
            TextColumn.Width = new GridLength(ratio, GridUnitType.Star);
            HexColumn.Width = new GridLength(1 - ratio, GridUnitType.Star);
        }

        private void SaveHexSplitRatio()
        {
            if (TextColumn == null || HexColumn == null || !_state.HexBytes)
                return;

            double totalWidth = TextColumn.ActualWidth + HexColumn.ActualWidth;
            if (totalWidth <= 0)
                return;

            _state.HexSplitRatio = TextColumn.ActualWidth / totalWidth;
        }

        public void Dispose()
        {
            _isDisposed = true;
            StopDeviceChangeWatcher();
            _portRefreshDebounceTimer.Stop();
            _selectionDebounceTimer.Stop();
            SaveSettings();
            _reader.Dispose();
        }

        private void StartDeviceChangeWatcher()
        {
            try
            {
                _deviceChangeWatcher = new ManagementEventWatcher(new WqlEventQuery("SELECT * FROM Win32_DeviceChangeEvent"));
                _deviceChangeWatcher.EventArrived += DeviceChangeWatcher_EventArrived;
                _deviceChangeWatcher.Start();
            }
            catch
            {
                StopDeviceChangeWatcher();
            }
        }

        private void StopDeviceChangeWatcher()
        {
            if (_deviceChangeWatcher == null)
                return;

            try
            {
                _deviceChangeWatcher.EventArrived -= DeviceChangeWatcher_EventArrived;
                _deviceChangeWatcher.Stop();
            }
            catch
            {
                // Ignore watcher teardown failures.
            }
            finally
            {
                _deviceChangeWatcher.Dispose();
                _deviceChangeWatcher = null;
            }
        }

        private void DeviceChangeWatcher_EventArrived(object sender, EventArrivedEventArgs e)
        {
            if (_isDisposed || Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
                return;

#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                QueuePortRefresh();
            });
        }

        private void QueuePortRefresh()
        {
            if (_isDisposed)
                return;

            _portRefreshDebounceTimer.Stop();
            _portRefreshDebounceTimer.Start();
        }

        private void PortRefreshDebounceTimer_Tick(object? sender, EventArgs e)
        {
            _portRefreshDebounceTimer.Stop();
            if (_isDisposed)
                return;

            RefreshPorts();
        }

        private void ApplySavedPort()
        {
            ApplyStateToQuickControls();
        }

        private void ApplyStateToQuickControls()
        {
            if (!string.IsNullOrWhiteSpace(_state.PortName) && PortComboBox.Items.Contains(_state.PortName))
                PortComboBox.SelectedItem = _state.PortName;

            EncodingComboBox.SelectedItem = _state.EncodingChoice;
            if (TimestampsButton.IsChecked != _state.Timestamps)
                TimestampsButton.IsChecked = _state.Timestamps;
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
            SolidColorBrush brush = CreateBrush(Color.FromRgb(0x3f, 0x3f, 0x46));

            foreach (TextRange range in ranges)
            {
                if (range.IsEmpty)
                    continue;

                object previousBackground = range.GetPropertyValue(TextElement.BackgroundProperty);
                range.ApplyPropertyValue(TextElement.BackgroundProperty, brush);
                _mirrorHighlights.Add(new HoverHighlightRange(range, previousBackground));
            }
        }

        private void ClearMirrorHighlights()
        {
            if (_mirrorHighlights.Count == 0)
                return;

            foreach (HoverHighlightRange highlight in _mirrorHighlights)
            {
                if (highlight.Range.IsEmpty)
                    continue;

                object previousBackground = highlight.PreviousBackground == DependencyProperty.UnsetValue
                    ? null!
                    : highlight.PreviousBackground;
                highlight.Range.ApplyPropertyValue(TextElement.BackgroundProperty, previousBackground);
            }

            _mirrorHighlights.Clear();
        }

        private void UpdateTimestampHover(Point point)
        {
            if (!_state.Timestamps || _timestampHoverRanges.Count == 0)
            {
                ClearTimestampHover();
                return;
            }

            TextPointer? pointer = LogBox.GetPositionFromPoint(point, snapToText: true);
            if (pointer == null)
            {
                ClearTimestampHover();
                return;
            }

            foreach (TimestampHoverRange range in _timestampHoverRanges)
            {
                if (!range.Contains(pointer))
                    continue;

                ShowTimestampToolTip(range.ToolTip);
                return;
            }

            ClearTimestampHover();
        }

        private void ClearTimestampHover()
        {
            CloseTimestampToolTip();
        }

        private void ShowTimestampToolTip(string text)
        {
            if (_timestampToolTip != null && string.Equals(_timestampToolTip.Content as string, text, StringComparison.Ordinal))
                return;

            CloseTimestampToolTip();

            ToolTip toolTip = new ToolTip
            {
                Content = text,
                Placement = PlacementMode.Mouse,
                PlacementTarget = LogBox,
                StaysOpen = true,
                IsOpen = true
            };

            _timestampToolTip = toolTip;
        }

        private void CloseTimestampToolTip()
        {
            if (_timestampToolTip == null)
                return;

            _timestampToolTip.IsOpen = false;
            _timestampToolTip = null;
        }

        private void UpdateHexHover(Point point)
        {
            if (!_state.HexMouseOverToolTips)
            {
                ClearHexHover();
                return;
            }

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
            if (int.TryParse(_state.BaudRate, out int baudRate) && baudRate > 0)
                return baudRate;

            return 115200;
        }

        private Encoding GetSelectedEncoding()
        {
            return _state.EncodingChoice.Encoding;
        }

        private void UpdateSelectedEncodingSnapshot()
        {
            _selectedEncoding = GetSelectedEncoding();
        }

        private int GetSelectedDataBits()
        {
            return _state.DataBits;
        }

        private int GetSelectedTabSize()
        {
            return GetAllowedTabSize(_state.TabSize);
        }

        private static int GetAllowedTabSize(int value)
        {
            return TabExpansion.GetAllowedTabSize(value);
        }

        private StopBits GetSelectedStopBits()
        {
            return _state.StopBitsChoice.StopBits;
        }

        private Parity GetSelectedParity()
        {
            return _state.Parity;
        }

        private Handshake GetSelectedHandshake()
        {
            return _state.FlowControlChoice.Handshake;
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
            if (double.TryParse(_state.FontSize, out fontSize) && fontSize > 0)
                return true;

            fontSize = 16;
            return false;
        }

        private void SaveSettings()
        {
            _state.PortName = PortComboBox.SelectedItem as string ?? _state.PortName;
            _state.AutoScroll = _state.AutoScroll;
            _state.HexBytes = _state.HexBytes;
            SaveHexSplitRatio();
            _state.Save();
        }

        private void SearchHexCheckBox_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrWhiteSpace(SearchTextBox.Text))
                PerformSearch(NormalizeSearchQuery(SearchTextBox.Text, SearchHexCheckBox.IsChecked == true), SearchHexCheckBox.IsChecked == true, true, restart: true);
        }

        private void SearchPreviousButton_Click(object sender, RoutedEventArgs e)
        {
            PerformSearch(NormalizeSearchQuery(SearchTextBox.Text, SearchHexCheckBox.IsChecked == true), SearchHexCheckBox.IsChecked == true, false, restart: false);
        }

        private void SearchNextButton_Click(object sender, RoutedEventArgs e)
        {
            PerformSearch(NormalizeSearchQuery(SearchTextBox.Text, SearchHexCheckBox.IsChecked == true), SearchHexCheckBox.IsChecked == true, true, restart: false);
        }

        private void ClearSearchSelection()
        {
            ClearSearchHighlights();
            ClearMirrorHighlights();
            _lastSearchQuery = string.Empty;
            _currentSearchLineIndex = -1;
            _currentSearchStartOffset = -1;
            _currentSearchEndOffset = -1;
        }

        private bool HasSearchContextChanged(string query, bool searchHex)
        {
            return !string.Equals(_lastSearchQuery, query, StringComparison.Ordinal) || _lastSearchWasHex != searchHex;
        }

        private void PerformSearch(string query, bool searchHex, bool searchForward, bool restart)
        {
            if (string.IsNullOrWhiteSpace(query) || _lineIndex.Count == 0)
                return;

            RichTextBox box = searchHex ? HexLogBox : LogBox;
            if (!TryFindSearchMatch(query, searchHex, searchForward, restart, out int lineIndex, out int matchStartOffset, out int matchEndOffset))
            {
                ClearSearchSelection();
                _lastSearchQuery = query;
                _lastSearchWasHex = searchHex;
                return;
            }

            CaptureLineIndex.CaptureLine line = _lineIndex.GetLine(lineIndex);
            TextPointer? lineStart = GetLineStartPointer(box, lineIndex);
            if (lineStart == null)
                return;

            TextPointer start = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(box, line, matchStartOffset));
            TextPointer end = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(box, line, matchEndOffset));
            RichTextBox target = searchHex ? LogBox : HexLogBox;
            try
            {
                _suppressSelectionEvents = true;
                ClearSearchHighlights();
                ClearMirrorHighlights();
                ClearSelection(LogBox);
                ClearSelection(HexLogBox);
                ApplySearchHighlight(new TextRange(start, end));
            }
            finally
            {
                _suppressSelectionEvents = false;
            }

            ScrollToSearchMatch(box, lineIndex, start);
            _lastSearchQuery = query;
            _lastSearchWasHex = searchHex;
            _currentSearchLineIndex = lineIndex;
            _currentSearchStartOffset = matchStartOffset;
            _currentSearchEndOffset = matchEndOffset;
            ApplySearchMirrorHighlight(line, searchHex, matchStartOffset, matchEndOffset, target, lineIndex);
        }

        private void ApplySearchMirrorHighlight(CaptureLineIndex.CaptureLine line, bool searchHex, int matchStartOffset, int matchEndOffset, RichTextBox target, int lineIndex)
        {
            if (!_state.HexBytes && !searchHex)
                return;

            if (!TryMapOffsets(line, searchHex, matchStartOffset, matchEndOffset, includeLeadingBytes: false, includeTrailingBytes: false, out int targetStartOffset, out int targetEndOffset))
                return;

            TextPointer? lineStart = GetLineStartPointer(target, lineIndex);
            if (lineStart == null)
                return;

            int targetLineLength = searchHex ? line.TextLength : line.HexLength;
            int safeStart = Math.Max(0, Math.Min(targetStartOffset, targetLineLength));
            int safeEnd = Math.Max(safeStart, Math.Min(targetEndOffset, targetLineLength));
            TextPointer start = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(target, line, safeStart));
            TextPointer end = TextPointerNavigation.GetAtTextOffset(lineStart, GetPointerOffset(target, line, safeEnd));
            ApplyMirrorHighlights(target, new[] { new TextRange(start, end) });
        }

        private bool TryFindSearchMatch(string query, bool searchHex, bool searchForward, bool restart, out int lineIndex, out int matchStartOffset, out int matchEndOffset)
        {
            lineIndex = -1;
            matchStartOffset = -1;
            matchEndOffset = -1;

            int startLine = restart || _currentSearchLineIndex < 0 || _lastSearchWasHex != searchHex
                ? (searchForward ? 0 : _lineIndex.Count - 1)
                : Math.Max(0, Math.Min(_currentSearchLineIndex, _lineIndex.Count - 1));
            int firstLine = startLine;

            for (int visited = 0; visited < _lineIndex.Count; visited++)
            {
                CaptureLineIndex.CaptureLine line = _lineIndex.GetLine(startLine);
                string modelText = searchHex ? line.Hex : line.Text;
                string searchable = searchHex ? modelText : GetSearchableTextLine(modelText);
                if (searchable.Length == 0)
                {
                    startLine = searchForward
                        ? (startLine + 1) % _lineIndex.Count
                        : (startLine - 1 + _lineIndex.Count) % _lineIndex.Count;
                    continue;
                }

                int startOffset;
                if (restart || startLine != firstLine || _currentSearchLineIndex < 0 || _lastSearchWasHex != searchHex)
                {
                    startOffset = searchForward ? 0 : Math.Max(0, searchable.Length - 1);
                }
                else
                {
                    startOffset = searchForward
                        ? Math.Max(0, Math.Min(_currentSearchEndOffset, searchable.Length))
                        : Math.Max(0, Math.Min(_currentSearchStartOffset - 1, Math.Max(0, searchable.Length - 1)));
                }

                int found = searchForward
                    ? searchable.IndexOf(query, startOffset, StringComparison.Ordinal)
                    : searchable.LastIndexOf(query, startOffset, StringComparison.Ordinal);
                if (found >= 0)
                {
                    lineIndex = startLine;
                    matchStartOffset = searchHex ? found : TabExpansion.ExpandedOffsetToModelOffset(modelText, found, GetSelectedTabSize());
                    matchEndOffset = searchHex ? found + query.Length : TabExpansion.ExpandedOffsetToModelOffset(modelText, found + query.Length, GetSelectedTabSize());
                    return true;
                }

                startLine = searchForward
                    ? (startLine + 1) % _lineIndex.Count
                    : (startLine - 1 + _lineIndex.Count) % _lineIndex.Count;
            }

            return false;
        }

        private void ScrollToSearchMatch(RichTextBox box, int lineIndex, TextPointer matchStart)
        {
            ScrollViewer? scrollViewer = GetScrollViewer(box);
            if (scrollViewer == null)
                return;

            double lineScrollUnit = GetLineScrollUnit(scrollViewer, _lineIndex.Count, box.FontSize);
            double visibleRows = scrollViewer.ViewportHeight > 0 && lineScrollUnit > 0
                ? Math.Max(1, Math.Floor(scrollViewer.ViewportHeight / lineScrollUnit))
                : 1;
            double firstVisibleLine = Math.Max(0, Math.Min(Math.Max(0, lineIndex - 2), Math.Max(0, _lineIndex.Count - visibleRows)));
            scrollViewer.ScrollToVerticalOffset(firstVisibleLine * lineScrollUnit);
        }

        private static double GetLineScrollUnit(ScrollViewer scrollViewer, int lineCount, double fallbackLineHeight)
        {
            if (lineCount > 0 && scrollViewer.ExtentHeight > 0)
                return Math.Max(1, scrollViewer.ExtentHeight / lineCount);

            return Math.Max(1, fallbackLineHeight);
        }

        private static string NormalizeSearchQuery(string query, bool searchHex)
        {
            if (string.IsNullOrEmpty(query))
                return string.Empty;

            string normalized = searchHex ? query : StripAnsiSequences(query);
            normalized = normalized.Replace("\r", string.Empty).Replace("\n", string.Empty);
            return normalized;
        }

        private string GetSearchableTextLine(string text)
        {
            int column = 0;
            return TabExpansion.Expand(text, GetSelectedTabSize(), ref column);
        }

        private static string StripAnsiSequences(string text)
        {
            if (string.IsNullOrEmpty(text))
                return string.Empty;

            var builder = new StringBuilder(text.Length);
            for (int index = 0; index < text.Length; index++)
            {
                char current = text[index];
                if (current != '\u001b')
                {
                    builder.Append(current);
                    continue;
                }

                if (index + 1 < text.Length && text[index + 1] == '[')
                {
                    index += 2;
                    while (index < text.Length)
                    {
                        char token = text[index];
                        if (token >= '@' && token <= '~')
                            break;
                        index++;
                    }
                    continue;
                }

                if (index + 1 < text.Length)
                    index++;
            }

            return builder.ToString();
        }

        private void ApplySearchHighlight(TextRange range)
        {
            if (range.IsEmpty)
                return;

            object previousBackground = range.GetPropertyValue(TextElement.BackgroundProperty);
            range.ApplyPropertyValue(TextElement.BackgroundProperty, CreateBrush(Color.FromRgb(0x26, 0x4f, 0x78)));
            _searchHighlights.Add(new HoverHighlightRange(range, previousBackground));
        }

        private void ClearSearchHighlights()
        {
            if (_searchHighlights.Count == 0)
                return;

            foreach (HoverHighlightRange highlight in _searchHighlights)
            {
                if (highlight.Range.IsEmpty)
                    continue;

                object previousBackground = highlight.PreviousBackground == DependencyProperty.UnsetValue
                    ? null!
                    : highlight.PreviousBackground;
                highlight.Range.ApplyPropertyValue(TextElement.BackgroundProperty, previousBackground);
            }

            _searchHighlights.Clear();
        }

        private sealed class MonitorSettingsState
        {
            private readonly UartMonitorUserSettings _settings;
            private static readonly string[] PreferredFontFamilies = { "Ubuntu Mono", "Lucida Console", "Courier New" };

            private MonitorSettingsState(UartMonitorUserSettings settings)
            {
                _settings = settings;
            }

            public string? PortName { get; set; }
            public string BaudRate { get; set; } = "115200";
            public EncodingChoice EncodingChoice { get; set; } = null!;
            public FontFamilyChoice FontFamilyChoice { get; set; } = null!;
            public string FontSize { get; set; } = "16";
            public int TabSize { get; set; } = 8;
            public int DataBits { get; set; } = 8;
            public StopBitsChoice StopBitsChoice { get; set; } = null!;
            public Parity Parity { get; set; } = Parity.None;
            public FlowControlChoice FlowControlChoice { get; set; } = null!;
            public bool AutoScroll { get; set; } = true;
            public bool HexBytes { get; set; } = true;
            public double HexSplitRatio { get; set; } = 0.6;
            public bool HexMouseOverToolTips { get; set; } = true;
            public bool Timestamps { get; set; }
            public bool PanelSync { get; set; }

            public static MonitorSettingsState Load(
                IReadOnlyList<EncodingChoice> encodingOptions,
                IReadOnlyList<FontFamilyChoice> fontFamilyOptions,
                IReadOnlyList<StopBitsChoice> stopBitsOptions,
                IReadOnlyList<FlowControlChoice> flowControlOptions)
            {
                UartMonitorUserSettings settings = UartMonitorUserSettings.Load();
                return new MonitorSettingsState(settings)
                {
                    PortName = settings.PortName,
                    BaudRate = settings.BaudRate,
                    EncodingChoice = encodingOptions.FirstOrDefault(choice => string.Equals(choice.DisplayName, settings.EncodingDisplayName, StringComparison.OrdinalIgnoreCase))
                        ?? encodingOptions.FirstOrDefault(choice => choice.Encoding.CodePage == 28591)
                        ?? encodingOptions.First(),
                    FontFamilyChoice = fontFamilyOptions.FirstOrDefault(choice => choice.FontFamily.Source.Equals(settings.FontFamily, StringComparison.OrdinalIgnoreCase))
                        ?? GetPreferredFontFamilyChoice(fontFamilyOptions)
                        ?? fontFamilyOptions.FirstOrDefault(choice => choice.IsMonospace)
                        ?? fontFamilyOptions.First(),
                    FontSize = settings.FontSize,
                    TabSize = GetAllowedTabSize(settings.TabSize),
                    DataBits = settings.DataBits,
                    StopBitsChoice = stopBitsOptions.FirstOrDefault(choice => choice.DisplayName == settings.StopBits) ?? stopBitsOptions.First(),
                    Parity = Enum.TryParse(settings.Parity, out Parity parity) ? parity : Parity.None,
                    FlowControlChoice = flowControlOptions.FirstOrDefault(choice => choice.DisplayName == settings.FlowControl)
                        ?? flowControlOptions.First(choice => choice.Handshake == Handshake.XOnXOff),
                    AutoScroll = settings.AutoScroll,
                    HexBytes = settings.HexBytes,
                    HexSplitRatio = settings.HexSplitRatio,
                    HexMouseOverToolTips = settings.HexMouseOverToolTips,
                    Timestamps = settings.Timestamps,
                    PanelSync = settings.PanelSync
                };
            }

            public void Save()
            {
                _settings.PortName = PortName;
                _settings.BaudRate = BaudRate;
                _settings.EncodingDisplayName = EncodingChoice.DisplayName;
                _settings.FontFamily = FontFamilyChoice.FontFamily.Source;
                _settings.FontSize = FontSize;
                _settings.TabSize = TabSize;
                _settings.DataBits = DataBits;
                _settings.StopBits = StopBitsChoice.DisplayName;
                _settings.Parity = Parity.ToString();
                _settings.FlowControl = FlowControlChoice.DisplayName;
                _settings.AutoScroll = AutoScroll;
                _settings.HexBytes = HexBytes;
                _settings.HexSplitRatio = HexSplitRatio;
                _settings.HexMouseOverToolTips = HexMouseOverToolTips;
                _settings.Timestamps = Timestamps;
                _settings.PanelSync = PanelSync;
                _settings.Save();
            }

            private static FontFamilyChoice? GetPreferredFontFamilyChoice(IReadOnlyList<FontFamilyChoice> fontFamilyOptions)
            {
                foreach (string fontFamily in PreferredFontFamilies)
                {
                    FontFamilyChoice? match = fontFamilyOptions.FirstOrDefault(choice => choice.FontFamily.Source.Equals(fontFamily, StringComparison.OrdinalIgnoreCase));
                    if (match != null)
                        return match;
                }

                return null;
            }
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

        private sealed class TimestampHoverRange
        {
            public TimestampHoverRange(TextPointer start, TextPointer end, string toolTip)
            {
                Start = start;
                End = end;
                ToolTip = toolTip;
            }

            public TextPointer Start { get; }
            public TextPointer End { get; }
            public string ToolTip { get; }

            public bool Contains(TextPointer pointer)
            {
                return pointer.CompareTo(Start) >= 0 && pointer.CompareTo(End) <= 0;
            }
        }

        private void Pane_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            if (!_state.PanelSync || !_state.HexBytes)
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
