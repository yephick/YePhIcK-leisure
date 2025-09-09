#pragma once
#include "MainWindow.g.h"

namespace winrt::runlock::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void BrowseArchive_Click(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void Archive_DragOver(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::Windows::Foundation::IAsyncAction Archive_Drop(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);

        void Rules_DragOver(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::Windows::Foundation::IAsyncAction Rules_Drop(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);

        void GeneratePasswords_Click(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void StartPause_Click(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

        void SaveProject_Click(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void LoadProject_Click(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    };
}

namespace winrt::runlock::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
