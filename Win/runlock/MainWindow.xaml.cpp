#include "pch.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::runlock::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        AllowDrop(true);
    }
}
