/** @brief Asset import UI commands and setting-state translation. */
#include "AssetImportFileDialog.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "editor/menu/EditorMenuPlatform.h"

#include <charconv>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Horo::Editor {
    namespace {
        /** @brief Resolves a choice default to the index stored by the import operation. */
        [[nodiscard]] Assets::ImportSettingValue DisplayDefault(const Assets::ImportSettingDescriptor &setting) {
            if (setting.kind != Assets::ImportSettingKind::Choice)
                return setting.defaultValue;
            if (const auto *index = std::get_if<std::size_t>(&setting.defaultValue))
                return *index;
            for (std::size_t index = 0; index < setting.choices.size(); ++index)
                if (setting.choices[index].value == setting.defaultValue)
                    return index;
            return std::size_t{0};
        }
    }  // namespace

    /** @copydoc AssetImportModal::ImporterFor */
    const Assets::AssetImporterContribution *AssetImportModal::ImporterFor(const std::size_t index) const noexcept {
        if (!m_catalog || index >= m_snapshot.items.size())
            return nullptr;
        return m_catalog->FindById(m_snapshot.items[index].importerContributionId);
    }

    /** @copydoc AssetImportModal::SettingValue */
    Assets::ImportSettingValue AssetImportModal::SettingValue(const std::size_t index,
                                                              const Assets::ImportSettingDescriptor &setting) const {
        if (index >= m_snapshot.items.size())
            return DisplayDefault(setting);
        const auto found = m_snapshot.items[index].settings.find("settings." + setting.id);
        if (found == m_snapshot.items[index].settings.end())
            return DisplayDefault(setting);
        const std::string &raw = found->second;
        switch (setting.kind) {
            case Assets::ImportSettingKind::Boolean:
                return raw == "true";
            case Assets::ImportSettingKind::Integer: {
                std::int64_t value{};
                const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                return error == std::errc{} && end == raw.data() + raw.size() ? Assets::ImportSettingValue{value} : DisplayDefault(setting);
            }
            case Assets::ImportSettingKind::Float: {
                try {
                    std::size_t parsed{};
                    const double value = std::stod(raw, &parsed);
                    return parsed == raw.size() ? Assets::ImportSettingValue{value} : DisplayDefault(setting);
                } catch (...) {
                    return DisplayDefault(setting);
                }
            }
            case Assets::ImportSettingKind::Text:
                return raw;
            case Assets::ImportSettingKind::Choice: {
                std::size_t value{};
                const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                return error == std::errc{} && end == raw.data() + raw.size() && value < setting.choices.size()
                           ? Assets::ImportSettingValue{value}
                           : DisplayDefault(setting);
            }
        }
        return DisplayDefault(setting);
    }

    /** @copydoc AssetImportModal::SetSettingValue */
    void AssetImportModal::SetSettingValue(const std::size_t index, const Assets::ImportSettingDescriptor &setting,
                                           const Assets::ImportSettingValue &value) {
        if (m_readOnlyPresentation || index >= m_snapshot.items.size() || m_snapshot.items[index].result)
            return;
        std::string encoded;
        switch (setting.kind) {
            case Assets::ImportSettingKind::Boolean:
                if (const auto *typed = std::get_if<bool>(&value))
                    encoded = *typed ? "true" : "false";
                break;
            case Assets::ImportSettingKind::Integer:
                if (const auto *typed = std::get_if<std::int64_t>(&value))
                    encoded = std::to_string(*typed);
                break;
            case Assets::ImportSettingKind::Float:
                if (const auto *typed = std::get_if<double>(&value))
                    encoded = std::to_string(*typed);
                break;
            case Assets::ImportSettingKind::Text:
                if (const auto *typed = std::get_if<std::string>(&value))
                    encoded = *typed;
                break;
            case Assets::ImportSettingKind::Choice:
                if (const auto *typed = std::get_if<std::size_t>(&value))
                    encoded = std::to_string(*typed);
                break;
        }
        if (!encoded.empty() || setting.kind == Assets::ImportSettingKind::Text)
            m_snapshot.items[index].settings["settings." + setting.id] = std::move(encoded);
    }

    /** @copydoc AssetImportModal::AddSourceFiles */
    void AssetImportModal::AddSourceFiles(const std::vector<std::filesystem::path> &paths) {
        if (paths.empty() || m_readOnlyPresentation)
            return;
        CancellationToken cancellation;
        if (m_projectRoot.empty())
            static_cast<void>(BeginImport(paths, cancellation));
        else
            static_cast<void>(BeginImport(paths, m_projectRoot, cancellation));
    }

    /** @copydoc AssetImportModal::BrowseSourceFiles */
    void AssetImportModal::BrowseSourceFiles() {
        if (!m_readOnlyPresentation)
            AddSourceFiles(ChooseAssetImportFiles());
    }

    /** @copydoc AssetImportModal::BrowseDestination */
    void AssetImportModal::BrowseDestination() {
        if (m_readOnlyPresentation || m_projectRoot.empty())
            return;
        const auto selected = ChooseAssetImportFolder();
        if (!selected)
            return;
        SetDefaultDestination(*selected);
        std::error_code error;
        if (!std::filesystem::equivalent(*selected, m_projectRoot / m_defaultDestinationFolder, error) || error)
            return;
        for (auto &item : m_snapshot.items)
            if (!item.result)
                item.destinationFolder = m_defaultDestinationFolder;
    }

    /** @copydoc AssetImportModal::RevealSelectedSource */
    void AssetImportModal::RevealSelectedSource() const {
        if (m_readOnlyPresentation || m_snapshot.selectedItemIndex >= m_snapshot.items.size())
            return;
        const auto &path = m_snapshot.items[m_snapshot.selectedItemIndex].absoluteSourcePath;
        if (path.is_absolute())
            static_cast<void>(RevealInNativeFileManager(path));
    }

    /** @copydoc AssetImportModal::StartIncludedImport */
    void AssetImportModal::StartIncludedImport() {
        if (m_readOnlyPresentation)
            return;
        CancellationToken cancellation;
        static_cast<void>(ImportIncludedItems(cancellation));
    }

    /** @copydoc AssetImportModal::OptionsFor */
    AssetImportModal::ItemOptions AssetImportModal::OptionsFor(const std::size_t index) const {
        if (index >= m_snapshot.items.size())
            return {};
        const auto &item = m_snapshot.items[index];
        return {.assetName = item.displayName,
                .folderStrategy = item.subfolderByType,
                .assetIdStrategy = item.assetIdStrategy,
                .createMetaSidecar = item.createMetaSidecar,
                .overwriteWithoutPrompt = item.overwriteWithoutPrompt};
    }

    /** @copydoc AssetImportModal::SetOptionsFor */
    void AssetImportModal::SetOptionsFor(const std::size_t index, ItemOptions options) {
        if (m_readOnlyPresentation || index >= m_snapshot.items.size() || m_snapshot.items[index].result)
            return;
        auto &item = m_snapshot.items[index];
        item.displayName = std::move(options.assetName);
        item.subfolderByType = options.folderStrategy;
        item.assetIdStrategy = options.assetIdStrategy;
        item.createMetaSidecar = options.createMetaSidecar;
        item.overwriteWithoutPrompt = options.overwriteWithoutPrompt;
    }

}  // namespace Horo::Editor
