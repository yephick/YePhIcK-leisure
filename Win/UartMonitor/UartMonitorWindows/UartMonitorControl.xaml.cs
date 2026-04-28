using Microsoft.VisualStudio.Shell;
using System;
using System.IO.Ports;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using UartMonitor.Serial;

namespace UartMonitor
{
    public partial class MyToolWindowControl : UserControl, IDisposable
    {
        private readonly SerialReader _reader = new SerialReader();

        public MyToolWindowControl()
        {
            InitializeComponent();

            BaudComboBox.ItemsSource = new[] { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000, 3000000, 4000000, 4608000 };
            BaudComboBox.SelectedItem = 115200;

            _reader.TextReceived += Reader_TextReceived;
            _reader.StatusChanged += Reader_StatusChanged;
            _reader.Error += Reader_Error;

            RefreshPorts();
        }

        private void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            RefreshPorts();
        }

        private void ConnectButton_Click(object sender, RoutedEventArgs e)
        {
            string? portName = PortComboBox.SelectedItem as string;
            if (string.IsNullOrWhiteSpace(portName))
            {
                SetStatus("No COM port selected");
                return;
            }

            int baudRate = BaudComboBox.SelectedItem is int selectedBaud ? selectedBaud : 115200;

            var options = new SerialPortOptions
            {
                PortName = portName!,
                BaudRate = baudRate,
                DataBits = 8,
                Parity = Parity.None,
                StopBits = StopBits.One,
                Handshake = Handshake.None,
                Encoding = Encoding.UTF8
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

        private void ClearButton_Click(object sender, RoutedEventArgs e)
        {
            LogBox.Document.Blocks.Clear();
        }

        private void Reader_TextReceived(object sender, string text)
        {
#pragma warning disable VSSDK007
            _ = ThreadHelper.JoinableTaskFactory.RunAsync(async () =>
#pragma warning restore VSSDK007
            {
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                LogBox.AppendText(text);

                if (AutoScrollCheckBox.IsChecked == true)
                    LogBox.ScrollToEnd();
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

        public void Dispose()
        {
            _reader.Dispose();
        }
    }
}
