/**
 * @copydoc AssetImporter.h
 */

#include "../AssetErrors.h"
#include "Horo/Assets/AssetImporter.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
        [[nodiscard]] bool ValidateSemanticVersionCore(const std::string_view core) {
            std::size_t componentStart = 0;
            for (int component = 0; component < 3; ++component) {
                const std::size_t end = component == 2 ? core.size() : core.find('.', componentStart);
                if (end == std::string_view::npos || end == componentStart)
                    return false;
                if (const std::string_view digits = core.substr(componentStart, end - componentStart);
                    (digits.size() > 1 && digits.front() == '0') || !std::ranges::all_of(digits, [](const unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
                    return false;
                }
                componentStart = end + 1;
            }
            return componentStart == core.size() + 1;
        }

        [[nodiscard]] bool ValidateSemanticVersionPrerelease(const std::string_view prerelease) {
            if (prerelease.empty())
                return false;
            std::size_t identifierStart = 0;
            while (identifierStart <= prerelease.size()) {
                const std::size_t end = prerelease.find('.', identifierStart);
                const std::string_view identifier =
                    prerelease.substr(identifierStart,
                                      end == std::string_view::npos ? prerelease.size() - identifierStart : end - identifierStart);
                if (identifier.empty() || !std::ranges::all_of(identifier, [](const unsigned char character) {
                    return std::isalnum(character) != 0 || character == '-';
                })) {
                    return false;
                }
                if (const bool numeric = std::ranges::all_of(identifier,
                                                             [](const unsigned char character) {
                    return std::isdigit(character) != 0;
                });
                    numeric && identifier.size() > 1 && identifier.front() == '0') {
                    return false;
                }
                if (end == std::string_view::npos)
                    return true;
                identifierStart = end + 1;
            }
            return false;
        }

        [[nodiscard]] bool IsCanonicalSemanticVersion(const std::string_view value) {
            if (value.empty() || value.size() > 64 || value.find('+') != std::string_view::npos)
                return false;
            const std::size_t dash = value.find('-');
            if (const std::string_view core = value.substr(0, dash); !ValidateSemanticVersionCore(core))
                return false;

            if (dash == std::string_view::npos)
                return true;
            return ValidateSemanticVersionPrerelease(value.substr(dash + 1));
        }

    }  // namespace

    // ---------------------------------------------------------------------------
    // AssetImporterContribution
    // ---------------------------------------------------------------------------

    bool AssetImporterContribution::HandlesExtension(std::string_view extension) const noexcept {
        return std::ranges::find(fileExtensions, extension) != fileExtensions.end();
    }

    // ---------------------------------------------------------------------------
    // AssetImporterCatalogSnapshot
    // ---------------------------------------------------------------------------

    const IAssetImporter *AssetImporterCatalogSnapshot::FindByExtension(std::string_view extension) const noexcept {
        for (const auto &entry : entries_) {
            if (entry.HandlesExtension(extension))
                return entry.strategy.get();
        }
        return nullptr;
    }

    const AssetImporterContribution *AssetImporterCatalogSnapshot::FindById(std::string_view contributionId) const noexcept {
        for (const auto &entry : entries_) {
            if (entry.contributionId == contributionId)
                return &entry;
        }
        return nullptr;
    }

    const AssetImporterContribution *AssetImporterCatalogSnapshot::FindContributionByExtension(std::string_view extension) const noexcept {
        for (const auto &entry : entries_) {
            if (entry.HandlesExtension(extension))
                return &entry;
        }
        return nullptr;
    }

    /** @copydoc AssetImporterCatalogSnapshot::FindPreviewContribution */
    const AssetImporterContribution *AssetImporterCatalogSnapshot::FindPreviewContribution(const AssetTypeId &assetType) const noexcept {
        for (const auto &entry : entries_) {
            if (std::ranges::find(entry.assetTypes, assetType) != entry.assetTypes.end())
                return &entry;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // AssetImporterCatalog
    // ---------------------------------------------------------------------------

    struct AssetImporterCatalog::State {
        std::vector<AssetImporterContribution> entries;
        std::shared_ptr<const AssetImporterCatalogSnapshot> published;
        bool sealed{false};
    };

    AssetImporterCatalog::AssetImporterCatalog() : state_(std::make_unique<State>()) {}

    AssetImporterCatalog::~AssetImporterCatalog() = default;

    Result<void> AssetImporterCatalog::Register(AssetImporterContribution entry) {
        std::vector<AssetImporterContribution> batch;
        batch.push_back(std::move(entry));
        return RegisterBatch(std::move(batch));
    }

    /** @copydoc AssetImporterCatalog::ValidateBatch */
    Result<void> AssetImporterCatalog::ValidateBatch(const std::span<const AssetImporterContribution> entries) const {
        if (state_->sealed)
            return Result<void>::Failure(Error{CookErrors::CatalogSealed.code});

        std::vector<std::string_view> incomingIds;
        incomingIds.reserve(entries.size());
        for (const auto &entry : entries) {
            if (entry.contributionId.empty() || entry.packageId.empty() || entry.moduleId.empty() ||
                !IsCanonicalSemanticVersion(entry.moduleVersion) || !IsCanonicalSemanticVersion(entry.version) ||
                entry.strategy == nullptr) {
                return Result<void>::Failure(
                    MakeError(CookErrors::MalformedArtifact,
                              "Importer contributions require stable package/module identities and canonical module/importer versions."));
            }

            if (std::ranges::any_of(state_->entries,
                                    [&entry](const AssetImporterContribution &existing) {
                return existing.contributionId == entry.contributionId;
            }) ||
                std::ranges::find(incomingIds, entry.contributionId) != incomingIds.end()) {
                return Result<void>::Failure(Error{CookErrors::DuplicateCooker.code});
            }
            incomingIds.push_back(entry.contributionId);
        }
        return Result<void>::Success();
    }

    /** @copydoc AssetImporterCatalog::RegisterBatch */
    Result<void> AssetImporterCatalog::RegisterBatch(std::vector<AssetImporterContribution> entries) {
        if (auto validation = ValidateBatch(entries); validation.HasError())
            return validation;

        state_->entries.reserve(state_->entries.size() + entries.size());
        std::ranges::move(entries, std::back_inserter(state_->entries));
        return Result<void>::Success();
    }

    Result<std::shared_ptr<const AssetImporterCatalogSnapshot>> AssetImporterCatalog::Publish() {
        if (state_->sealed)
            return Result<std::shared_ptr<const AssetImporterCatalogSnapshot>>::Failure(Error{CookErrors::CatalogSealed.code});

        // Sort entries by first extension, then contribution ID
        std::ranges::sort(state_->entries, [](const AssetImporterContribution &a, const AssetImporterContribution &b) {
            const auto aExt = a.fileExtensions.empty() ? "" : a.fileExtensions.front();
            if (const auto bExt = b.fileExtensions.empty() ? "" : b.fileExtensions.front(); aExt != bExt)
                return aExt < bExt;
            return a.contributionId < b.contributionId;
        });

        auto snapshot = std::make_shared<AssetImporterCatalogSnapshot>(state_->entries);
        state_->published = snapshot;
        state_->sealed = true;
        return Result<std::shared_ptr<const AssetImporterCatalogSnapshot>>::Success(snapshot);
    }

    std::shared_ptr<const AssetImporterCatalogSnapshot> AssetImporterCatalog::Snapshot() const noexcept {
        return state_->published;
    }

    bool AssetImporterCatalog::IsSealed() const noexcept {
        return state_->sealed;
    }

    void AssetImporterCatalog::Reset() {
        if (state_->sealed)
            return;
        state_->entries.clear();
    }

    /** @copydoc AssetImporterCatalog::WithdrawPackage */
    bool AssetImporterCatalog::WithdrawPackage(const std::string_view packageId) noexcept {
        try {
            std::vector<AssetImporterContribution> remaining;
            for (const auto &entry : state_->entries) {
                if (entry.packageId != packageId)
                    remaining.push_back(entry);
            }
            std::shared_ptr<const AssetImporterCatalogSnapshot> replacement;
            if (state_->sealed)
                replacement = std::make_shared<AssetImporterCatalogSnapshot>(remaining);
            state_->entries = std::move(remaining);
            state_->published = std::move(replacement);
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    }
}  // namespace Horo::Assets
