// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QVector>

namespace Core {
class System;
}

/**
 * Dialog for exporting a game as a native-export artifact bundle using
 * ahead-of-time (AOT) static recompilation.
 *
 * Pipeline:
 *   1. Extract and validate the title's ExeFS/RomFS.
 *   2. Translate discovered AArch64 basic blocks into portable C projects.
 *   3. Optionally compile those projects and link their AOT blocks into a
 *      title-specialized suyu runtime.
 *   4. Package the local title data, runtime, configuration, and build manifest.
 *
 * Unsupported instructions, uncovered indirect targets, and dynamic code are
 * deliberately handled by Dynarmic at run time. An export is therefore a
 * static-first hybrid, not a claim that every possible instruction was lifted.
 */
class GameExportDialog : public QDialog {
    Q_OBJECT

public:
    struct LibraryEntry {
        QString title;
        QString path;
        quint64 program_id{};
        QPixmap icon;
    };

    explicit GameExportDialog(Core::System& system_, QWidget* parent = nullptr);
    ~GameExportDialog() override = default;

    /// Set the game ROM path and optional title-id for portable data bundling.
    void SetRomPath(const QString& path, quint64 program_id = 0);
    void SetLibraryEntries(QVector<LibraryEntry> entries);
    /// Provide the game's artwork so it can be embedded as the launcher exe icon.
    void SetGameIcon(const QPixmap& icon);

    /// Test-only: drive a full export run without needing to click through
    /// the dialog's file pickers/combo boxes, so live automation can trigger
    /// and observe the AOT pipeline (including its known hang past ~15%)
    /// directly.
    /// @param format_index optional output-format combo index to select first
    ///        (0 = Source, 1 = Build); negative leaves the current selection.
    void TriggerExportForTesting(const QString& rom_path, const QString& output_dir,
                                 int format_index = -1);

    /// Every built title launcher or per-module AOT test executable found for
    /// this game, newest-looking first. Empty when the game was never exported,
    /// or was exported but never compiled.
    ///
    /// @param game_name  Library title of the game.
    /// @param rom_path   ROM path; its base name is what the exporter actually
    ///                   names the output directory, which is often not the
    ///                   library title, so both are tried.
    static QStringList FindRecompiledExecutables(const QString& game_name,
                                                 const QString& rom_path = {});

    /// Directories that have been used as export output, most recent first.
    static QStringList RecompileOutputRoots();
    /// Remember @p dir as an export output root for future lookups.
    static void RememberOutputRoot(const QString& dir);

    enum class TargetPlatform {
        Windows,
        Linux,
        MacOS,
    };

signals:
    void ExportFinished(bool success, const QString& output_path);

private slots:
    void OnBrowseRom();
    void OnSelectFromLibrary();
    void OnBrowseOutput();
    void OnExport();

protected:
    // An export pumps the event loop for as long as the compilers take (tens of
    // minutes on a large title) with its whole state on the stack of OnExport().
    // Closing the dialog in that window - Escape, the title-bar X, or anything
    // else that reaches reject() - returns from the exec() that owns this
    // object and destroys it while OnExport() is still running, which takes the
    // process down with no crash log. Both entry points are refused while an
    // export is in flight.
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    void SetupUi();

    /// True from the moment OnExport() starts until it returns. Guards both
    /// dialog teardown and re-entry into OnExport() itself: the automation RPC
    /// can call TriggerExportForTesting() from the event loop that the export
    /// is pumping, and two exports writing the same cache directory corrupt it.
    bool export_in_progress{false};

    /// AOT export: scan AArch64 code and emit portable C projects.
    /// Returns path to the generated cache directory, or empty string on failure.
    QString RunAotPrecompile(const QString& exefs_dir, const QString& cache_dir,
                             const QString& game_name);

    /// Package the translated output into a platform-specific export bundle.
    bool PackageNativeExport(const QString& rom_path, const QString& cache_dir,
                             const QString& output_dir, const QString& game_name,
                             TargetPlatform platform);

    QLineEdit* rom_path_edit{};
    QLineEdit* output_path_edit{};
    QComboBox* platform_combo{};
    QCheckBox* include_save_data_checkbox{};
    QCheckBox* include_shader_cache_checkbox{};
    QCheckBox* include_custom_config_checkbox{};
    QCheckBox* aot_full_scan_checkbox{};
    /// When checked and a module fails to recompile, emit a stub that falls back
    /// to the Dynarmic JIT for that module instead of aborting the export.
    QCheckBox* fallback_to_jit_checkbox{};
    QCheckBox* steam_shortcut_checkbox{};
    QCheckBox* steam_replace_rom_checkbox{};
    /// Export format: index 0 = source only, index 1 = build to a native binary.
    /// "Build" is a promise, not a hint - when it is selected the export runs
    /// cmake to completion and reports a hard error if a binary cannot be
    /// produced, rather than quietly degrading to a folder of C.
    QComboBox* output_format_combo{};
    /// True when output_format_combo selects the build-to-binary format.
    bool WantsCompiledOutput() const;
    void MaybeAddToSteam(const QString& game_name, const QString& exe_path);
    QProgressBar* progress_bar{};
    QPushButton* export_button{};
    QLabel* status_label{};
    quint64 rom_program_id{};
    QVector<LibraryEntry> library_entries_;
    Core::System& system_;
    QPixmap game_icon_;
};
