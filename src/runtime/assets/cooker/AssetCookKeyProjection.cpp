#include "../AssetErrors.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"

#include <algorithm>
#include <new>

namespace Horo::Assets {
    namespace {
        /** @brief Length-delimits canonical fields; paths and publication counters are deliberately absent. */
        void Append(std::string &bytes, const std::string_view field) {
            bytes += std::to_string(field.size());
            bytes += ':';
            bytes += field;
        }
    }  // namespace

    /** @copydoc AssetCookKeyProjection::Create */
    Result<std::shared_ptr<const AssetCookKeyProjection>> AssetCookKeyProjection::Create(const AssetCookInputSnapshot &inputs,
                                                                                         std::vector<AssetCookKeySelection> selections,
                                                                                         const std::size_t maximumAssets) {
        using Output = Result<std::shared_ptr<const AssetCookKeyProjection>>;
        if (maximumAssets == 0 || maximumAssets > 16U * 1024U || selections.size() > maximumAssets)
            return Output::Failure(MakeError(CookErrors::TooLarge));
        try {
            std::ranges::sort(selections, {}, &AssetCookKeySelection::asset);
            std::vector<Sha256Digest> digests;
            digests.reserve(selections.size());
            std::size_t references{};
            std::optional<AssetId> previous;
            for (auto &selection : selections) {
                if (!selection.asset.IsValid() || previous == selection.asset || selection.inputs.empty() ||
                    selection.inputs.size() > maximumAssets || selection.inputs.size() > 1'048'576U - references)
                    return Output::Failure(MakeError(CookErrors::MalformedArtifact));
                references += selection.inputs.size();
                previous = selection.asset;
                std::ranges::sort(selection.inputs);
                if (std::ranges::adjacent_find(selection.inputs) != selection.inputs.end() ||
                    !std::ranges::binary_search(selection.inputs, selection.asset))
                    return Output::Failure(MakeError(CookErrors::MalformedArtifact));
                std::string bytes{"horo.asset.producer-inputs.v1"};
                Append(bytes, selection.asset.ToString());
                Append(bytes, FormatSha256(selection.producerSettings));
                for (const auto id : selection.inputs) {
                    const auto *source = inputs.Find(id);
                    if (!id.IsValid() || !source)
                        return Output::Failure(MakeError(CookErrors::MalformedArtifact));
                    Append(bytes, id.ToString());
                    Append(bytes, source->record.type.Value());
                    Append(bytes, FormatSha256(source->sourceDigest));
                    Append(bytes, FormatSha256(source->metadataDigest));
                }
                digests.push_back(ComputeSha256(std::as_bytes(std::span{bytes})));
            }
            return Output::Success(std::shared_ptr<const AssetCookKeyProjection>{
                new AssetCookKeyProjection(inputs, std::move(selections), std::move(digests))});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(CookErrors::TooLarge));
        }
    }

    /** @copydoc AssetCookKeyProjection::AssetCookKeyProjection */
    AssetCookKeyProjection::AssetCookKeyProjection(const AssetCookInputSnapshot &capture, std::vector<AssetCookKeySelection> selections,
                                                   std::vector<Sha256Digest> digests)
        : capture_(capture), selections_(std::move(selections)), digests_(std::move(digests)) {}

    /** @copydoc AssetCookKeyProjection::Matches */
    bool AssetCookKeyProjection::Matches(const AssetCookInputSnapshot &inputs) const {
        return capture_.state_ == inputs.state_;
    }

    /** @copydoc AssetCookKeyProjection::Find */
    const AssetCookKeySelection *AssetCookKeyProjection::Find(const AssetId asset) const noexcept {
        const auto found = std::ranges::lower_bound(selections_, asset, {}, &AssetCookKeySelection::asset);
        return found != selections_.end() && found->asset == asset ? std::to_address(found) : nullptr;
    }

    /** @copydoc AssetCookKeyProjection::Digest */
    Sha256Digest AssetCookKeyProjection::Digest(const AssetId asset) const noexcept {
        const auto *selection = Find(asset);
        return selection ? digests_[static_cast<std::size_t>(selection - selections_.data())] : Sha256Digest{};
    }
}  // namespace Horo::Assets
