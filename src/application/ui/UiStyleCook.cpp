#include "Horo/Application/UiStyleWorkspace.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Immutable semantic strategy; fresh and cached outputs must describe exactly the captured source. */
        class UiStyleCooker final : public Assets::ICookerStrategy {
        public:
            UiStyleCooker(std::vector<UiStyleAssetDocument> documents, UiStyleSchema schema)
                : documents_(std::move(documents)), schema_(std::move(schema)) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "horo.ui-style.v1", .settingsDigest = schema_.fingerprint, .settingsSchemaVersion = 1};
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(UiErrors::AssetLoadCancelled));
                auto document = AdmitSource(source);
                if (document.HasError())
                    return Result<Assets::CookOutputSink>::Failure(document.ErrorValue());
                auto payload = CookUiStyleAsset(document.Value(), schema_);
                if (payload.HasError())
                    return Result<Assets::CookOutputSink>::Failure(payload.ErrorValue());
                std::vector<Assets::AssetId> dependencies;
                for (const auto &dependency : document.Value().dependencies)
                    dependencies.push_back(dependency.asset);
                return Result<Assets::CookOutputSink>::Success({std::move(payload).Value(), std::move(dependencies), {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                auto document = AdmitSource(source);
                if (document.HasError())
                    return Result<void>::Failure(document.ErrorValue());
                auto decoded = DecodeCookedUiStyleAsset(payload, source.id, schema_);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                auto expected = CookUiStyleAsset(document.Value(), schema_);
                if (expected.HasError())
                    return Result<void>::Failure(expected.ErrorValue());
                if (!std::ranges::equal(expected.Value(), payload))
                    return Result<void>::Failure(MakeError(UiErrors::StyleSourceStale));
                return Result<void>::Success();
            }

        private:
            /** @brief Check exact sidecar identity, source digest and captured complete source before semantic cooking. */
            Result<UiStyleAssetDocument> AdmitSource(const Assets::CookSourceView &source) const {
                if (source.type.Value() != UiStyleAssetTypeName || source.bytes.size() > MaximumUiStyleAssetBytes ||
                    ComputeSha256(std::as_bytes(source.bytes)) != source.sourceDigest)
                    return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleSourceStale));
                const auto found = std::ranges::find_if(documents_, [&](const auto &value) {
                    return value.style.id.Bytes() == source.id.Bytes();
                });
                if (found == documents_.end())
                    return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleReferenceInvalid));
                auto parsed = DeserializeUiStyleAsset({reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()});
                if (parsed.HasError())
                    return parsed;
                auto captured = SerializeUiStyleAsset(*found), actual = SerializeUiStyleAsset(parsed.Value());
                if (captured.HasError())
                    return Result<UiStyleAssetDocument>::Failure(captured.ErrorValue());
                if (actual.HasError())
                    return Result<UiStyleAssetDocument>::Failure(actual.ErrorValue());
                if (parsed.Value().style.id.Bytes() != source.id.Bytes() || captured.Value() != actual.Value())
                    return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleSourceStale));
                return parsed;
            }

            std::vector<UiStyleAssetDocument> documents_;
            UiStyleSchema schema_;
        };
    }  // namespace

    /** @copydoc MakeUiStyleCookerContribution */
    Result<Assets::CookerContribution> MakeUiStyleCookerContribution(std::vector<UiStyleAssetDocument> documents, UiStyleSchema schema,
                                                                     AssetCookTargetId target, const RuntimeStyleGeneration generation) {
        auto admitted = PrepareUiStyleRegistry(documents, schema, generation);
        if (admitted.HasError())
            return Result<Assets::CookerContribution>::Failure(admitted.ErrorValue());
        if (target.Value() != "headless-null")
            return Result<Assets::CookerContribution>::Failure(MakeError(UiErrors::StyleLifecycleUnavailable));
        auto type = Assets::AssetTypeId::Parse(UiStyleAssetTypeName);
        if (type.HasError())
            return Result<Assets::CookerContribution>::Failure(type.ErrorValue());
        return Result<Assets::CookerContribution>::Success(
            {"horo.builtin.ui_style.v1",
             std::move(type).Value(),
             {std::move(target)},
             std::make_shared<const UiStyleCooker>(std::move(documents), std::move(schema))});
    }

    /** @copydoc UiStyleWorkspaceThemes */
    Result<std::vector<UiStyleWorkspaceTheme>> UiStyleWorkspaceThemes(const Assets::AssetRegistrySnapshot &assets) {
        std::vector<UiStyleWorkspaceTheme> result;
        for (const auto &record : assets.Records())
            if (record.type.Value() == UiStyleAssetTypeName) {
                const auto id = RuntimeStyleAssetId::Create(record.id.Bytes());
                if (id.HasError())
                    return Result<std::vector<UiStyleWorkspaceTheme>>::Failure(id.ErrorValue());
                if (result.size() == MaximumUiStyleAssets)
                    return Result<std::vector<UiStyleWorkspaceTheme>>::Failure(MakeError(UiErrors::CapacityExceeded));
                result.emplace_back(id.Value(), record.sourcePath.String());
            }
        return Result<std::vector<UiStyleWorkspaceTheme>>::Success(std::move(result));
    }
}  // namespace Horo::Application
