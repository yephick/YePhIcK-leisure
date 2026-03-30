module;

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>

export module llvc.Dialogs;

import std;
import llvc.Utils;

export namespace llvc{

using namespace ::std;
using namespace ::winrt;

using XamlRoot = winrt::Microsoft::UI::Xaml::XamlRoot;
using DialogResult = winrt::Windows::Foundation::IAsyncOperation<bool>;
using DialogAction = winrt::Windows::Foundation::IAsyncAction;

DialogAction showInfoDialogAsync(const XamlRoot& xamlRoot, const hstring& title, const hstring& message);
DialogAction showQuickManualDialogAsync(const XamlRoot& xamlRoot);
DialogAction showAboutDialogAsync(const XamlRoot& xamlRoot, const hstring& version, const hstring& description);
winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult> showUnsavedProjectPromptAsync(const XamlRoot& xamlRoot);
DialogResult showOptionsDialogAsync(const XamlRoot& xamlRoot, AppSettingsState& settings);
DialogResult showDeleteAfterExportPromptAsync(const XamlRoot& xamlRoot, bool canDeleteSource, bool canDeleteProject);
DialogResult showOverwriteSourcePromptAsync(const XamlRoot& xamlRoot);

}

namespace llvc{

using namespace std;
using namespace winrt;
namespace Controls = winrt::Microsoft::UI::Xaml::Controls;

DialogAction showInfoDialogAsync(const XamlRoot& xamlRoot, const hstring& title, const hstring& message){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(xamlRoot);
    dialog.Title(box_value(title));
    dialog.Content(box_value(message));
    dialog.CloseButtonText(L"OK");
    co_await dialog.ShowAsync();
}

DialogAction showQuickManualDialogAsync(const XamlRoot& xamlRoot){
    co_await showInfoDialogAsync(
        xamlRoot,
        L"Quick manual",
        L"Functions:\n"
        L"* Load video: Open .mp4/.mov/.avi/.webm/.wmv source footage for timeline editing.\n"
        L"* Cut markers: Right-Click on the timeline/tick bar to toggle a marker at the desired frame. Markers split the video into scenes.\n"
        L"* Cut scene toggling: Ctrl+Left-Click a scene block to mark/unmark that whole scene for cutting; dark overlays indicate sections that will be removed.\n"
        L"* Boundary RAP nudging: Ctrl+< shrinks the current scene by nudging both scene edges inward to RAPs, Ctrl+> expands by nudging both edges outward to RAPs.\n"
        L"* Preview start/pause/stop skipping cut scenes.\n"
        L"* Timeline navigation: Left/Right steps a frame, Up/Down jumps between markers, PageUp/PageDown seeks backward/forward 10 seconds, and 0-9 jumps to 0%-90% of the timeline.\n"
        L"* Preview window: Tools -> Preview in separate window opens a movable second window; use F11 to toggle full-screen.\n"
        L"* Audio controls: Keep/remove audio and configure cross-fade for segment transitions.\n"
        L"* Project files: Save and reopen .llvc projects with timeline state.\n"
        L"* Export: Render a lossless cut based on your selected ranges (auto-adjusting to proper cut points if necessary). Use F7 as a shortcut.\n\n"
        L"Usage workflow:\n"
        L"1) File -> Load video (or drag and drop a supported file).\n"
        L"2) Right-click to place red boundary markers around scenes you may want to remove.\n"
        L"3) Reevaluate cut markers to turn true RAP markers green while preserving off-RAP red markers.\n"
        L"4) Ctrl+Left-Click scene blocks to toggle which scenes are cut (dark = cut, clear = kept).\n"
        L"5) Optionally adjust Keep audio and Audio cross-fade settings, then preview playback.\n"
        L"6) Use File -> Save project, then File -> Export video (or press F7) to generate the final cut.");
}

DialogAction showAboutDialogAsync(const XamlRoot& xamlRoot, const hstring& version, const hstring& description){
    const wstring aboutText{
        wstring{description.c_str()}
        + L"\n\nVersion "
        + wstring{version.c_str()}
        + L"\n\n"
        + L"\xA9 02'2026 YePhIcK"};
    co_await showInfoDialogAsync(xamlRoot, L"About ClipRazor: Lossless Video Cutter", hstring{aboutText});
}

winrt::Windows::Foundation::IAsyncOperation<Controls::ContentDialogResult> showUnsavedProjectPromptAsync(const XamlRoot& xamlRoot){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(xamlRoot);
    dialog.Title(box_value(L"Unsaved changes"));
    dialog.Content(box_value(L"Current project has unsaved changes. Save before continuing?"));
    dialog.PrimaryButtonText(L"Save");
    dialog.SecondaryButtonText(L"Don't save");
    dialog.CloseButtonText(L"Cancel");
    co_return co_await dialog.ShowAsync();
}

DialogResult showOptionsDialogAsync(const XamlRoot& xamlRoot, AppSettingsState& settings){
    Controls::StackPanel panel{};
    panel.Spacing(10);

    Controls::TextBlock videosLabel{};
    videosLabel.Text(L"Recent videos to keep (1-20)");
    Controls::NumberBox videosCount{};
    videosCount.Minimum(1);
    videosCount.Maximum(20);
    videosCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    videosCount.Value(settings.maxRecentVideos);

    Controls::TextBlock projectsLabel{};
    projectsLabel.Text(L"Recent projects to keep (1-20)");
    Controls::NumberBox projectsCount{};
    projectsCount.Minimum(1);
    projectsCount.Maximum(20);
    projectsCount.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
    projectsCount.Value(settings.maxRecentProjects);

    Controls::CheckBox deleteAfterExportCheckBox{};
    deleteAfterExportCheckBox.Content(box_value(L"Delete source video and project file after successful export"));
    deleteAfterExportCheckBox.IsChecked(settings.deleteSourceAndProjectAfterExport);

    Controls::CheckBox autoReevaluateCutMarkersOnPlacementCheckBox{};
    autoReevaluateCutMarkersOnPlacementCheckBox.Content(box_value(L"Auto-reevaluate cut markers when placing them"));
    autoReevaluateCutMarkersOnPlacementCheckBox.IsChecked(settings.autoReevaluateCutMarkersOnPlacement);

    Controls::CheckBox generateExportTimeReportCheckBox{};
    generateExportTimeReportCheckBox.Content(box_value(L"Copy export time report to clipboard after export"));
    generateExportTimeReportCheckBox.IsChecked(settings.generateExportTimeReport);

    panel.Children().Append(videosLabel);
    panel.Children().Append(videosCount);
    panel.Children().Append(projectsLabel);
    panel.Children().Append(projectsCount);
    panel.Children().Append(deleteAfterExportCheckBox);
    panel.Children().Append(autoReevaluateCutMarkersOnPlacementCheckBox);
    panel.Children().Append(generateExportTimeReportCheckBox);

    Controls::ContentDialog dialog{};
    dialog.XamlRoot(xamlRoot);
    dialog.Title(box_value(L"Options"));
    dialog.Content(panel);
    dialog.PrimaryButtonText(L"Save");
    dialog.CloseButtonText(L"Cancel");

    if((co_await dialog.ShowAsync()) != Controls::ContentDialogResult::Primary){
        co_return false;
    }

    settings.maxRecentVideos = static_cast<uint32_t>(clamp(static_cast<int>(lround(videosCount.Value())), 1, 20));
    settings.maxRecentProjects = static_cast<uint32_t>(clamp(static_cast<int>(lround(projectsCount.Value())), 1, 20));
    settings.deleteSourceAndProjectAfterExport = deleteAfterExportCheckBox.IsChecked().GetBoolean();
    settings.autoReevaluateCutMarkersOnPlacement = autoReevaluateCutMarkersOnPlacementCheckBox.IsChecked().GetBoolean();
    settings.generateExportTimeReport = generateExportTimeReportCheckBox.IsChecked().GetBoolean();
    co_return true;
}

DialogResult showDeleteAfterExportPromptAsync(const XamlRoot& xamlRoot, bool canDeleteSource, bool canDeleteProject){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(xamlRoot);
    dialog.Title(box_value(
        canDeleteSource && canDeleteProject
            ? L"Delete source video and project file?"
            : (canDeleteSource ? L"Delete source video?" : L"Delete project file?")));
    dialog.Content(box_value(
        canDeleteSource && canDeleteProject
            ? L"The export completed successfully.\n\nDelete the original source video and the project file now?"
            : (canDeleteSource
                ? L"The export completed successfully.\n\nDelete the original source video now?"
                : L"The export completed successfully.\n\nDelete the project file now?")));
    dialog.PrimaryButtonText(
        canDeleteSource && canDeleteProject
            ? L"Delete both"
            : (canDeleteSource ? L"Delete source" : L"Delete project"));
    dialog.CloseButtonText(L"Keep files");
    dialog.DefaultButton(Controls::ContentDialogButton::Close);
    co_return (co_await dialog.ShowAsync()) == Controls::ContentDialogResult::Primary;
}

DialogResult showOverwriteSourcePromptAsync(const XamlRoot& xamlRoot){
    Controls::ContentDialog dialog{};
    dialog.XamlRoot(xamlRoot);
    dialog.Title(box_value(L"Overwrite source video?"));
    dialog.Content(box_value(
        L"The selected export target is the same as the source video.\n\n"
        L"llvc will export to a temporary file first, then replace the source file only if export succeeds.\n\n"
        L"Continue?"));
    dialog.PrimaryButtonText(L"Overwrite source");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(Controls::ContentDialogButton::Close);
    co_return (co_await dialog.ShowAsync()) == Controls::ContentDialogResult::Primary;
}

}
