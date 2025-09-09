#pragma once

#include "MainWindow.g.h"

namespace winrt::runlock::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void BrowseArchive_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void Archive_DragOver(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void Archive_Drop(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void PasswordRules_DragOver(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void PasswordRules_Drop(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void GeneratePasswords_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void CancelGenerate_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void UnlockButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SaveProject_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void LoadProject_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void Window_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        enum class UnlockState { Stopped, Running, Paused };
        UnlockState m_unlockState{ UnlockState::Stopped };
    };
}

namespace winrt::runlock::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
