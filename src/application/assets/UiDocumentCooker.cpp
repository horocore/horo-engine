#include "UiDocumentCooker.h"

#include "Horo/Assets/AssetCookService.h"
#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiCanvasDocumentSerialization.h"

#include <algorithm>
#include <utility>

namespace Horo::Application::PrefabCookDetail {
    namespace {
        using Runtime::Ui::CookedUiDocument;
        using Runtime::Ui::UiDocument;
        namespace UiErrors = Runtime::Ui::UiErrors;

        /** @brief Decodes the same typed authoring envelope used by the Editor, including legacy canvases. */
        Result<UiDocument> ReadDocument(const Assets::CookSourceView &source) {
            if (source.bytes.empty())
                return Result<UiDocument>::Failure(MakeError(UiErrors::DocumentInvalid));
            return Editor::UiCanvasDocumentSerialization::Parse({reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()});
        }

        /** @brief Resolves typed references inside one immutable inventory without consulting filesystem or live registries. */
        Result<std::vector<Assets::AssetId>> ResolveDependencies(const UiDocument &document,
                                                                 const Assets::AssetRegistrySnapshot &registry) {
            using Outcome = Result<std::vector<Assets::AssetId>>;
            std::vector<Assets::AssetId> dependencies;
            dependencies.reserve(document.Dependencies().size());
            for (const auto &dependency : document.Dependencies()) {
                const auto *record = registry.Find(dependency.asset);
                if (!record) {
                    if (dependency.required)
                        return Outcome::Failure(MakeError(UiErrors::AssetMissing, dependency.asset.ToString()));
                    continue;
                }
                if (record->type != dependency.expectedType)
                    return Outcome::Failure(MakeError(UiErrors::AssetTypeMismatch, dependency.asset.ToString()));
                dependencies.push_back(dependency.asset);
            }
            return Outcome::Success(std::move(dependencies));
        }

        /** @brief Publishes deterministic source-free document bytes through the ordinary asset pipeline. */
        class UiDocumentCooker final : public Assets::ICookerStrategy {
        public:
            explicit UiDocumentCooker(Assets::AssetRegistrySnapshot registry) : registry_(std::move(registry)) {}

            [[nodiscard]] Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "1.0.0", .settingsSchemaVersion = 1};
            }

            [[nodiscard]] Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(
                const Assets::CookSourceView &source, const CancellationToken &cancellation) const override {
                using Outcome = Result<std::optional<std::vector<Assets::AssetId>>>;
                if (cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(Assets::CookErrors::Cancelled));
                const auto document = ReadDocument(source);
                if (document.HasError())
                    return Outcome::Failure(document.ErrorValue());
                auto dependencies = ResolveDependencies(document.Value(), registry_);
                if (dependencies.HasError())
                    return Outcome::Failure(dependencies.ErrorValue());
                return Outcome::Success(std::move(dependencies).Value());
            }

            [[nodiscard]] Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                              const CancellationToken &cancellation) const override {
                using Outcome = Result<Assets::CookOutputSink>;
                if (cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(Assets::CookErrors::Cancelled));
                const auto document = ReadDocument(source);
                if (document.HasError())
                    return Outcome::Failure(document.ErrorValue());
                auto dependencies = ResolveDependencies(document.Value(), registry_);
                if (dependencies.HasError())
                    return Outcome::Failure(dependencies.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Outcome::Failure(MakeError(Assets::CookErrors::Cancelled));
                const auto cooked = CookedUiDocument::Cook(document.Value());
                if (cooked.HasError())
                    return Outcome::Failure(cooked.ErrorValue());
                const auto payload = cooked.Value().Payload();
                return Outcome::Success({{payload.begin(), payload.end()}, std::move(dependencies).Value(), {}});
            }

            [[nodiscard]] Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                                             std::span<const std::uint8_t> payload) const override {
                const auto document = ReadDocument(source);
                if (document.HasError())
                    return Result<void>::Failure(document.ErrorValue());
                const auto cooked = CookedUiDocument::Cook(document.Value());
                if (cooked.HasError())
                    return Result<void>::Failure(cooked.ErrorValue());
                if (!std::ranges::equal(cooked.Value().Payload(), payload))
                    return Result<void>::Failure(MakeError(UiErrors::CookedPayloadMalformed));
                return Result<void>::Success();
            }

        private:
            const Assets::AssetRegistrySnapshot registry_;
        };
    }  // namespace

    /** @copydoc MakeUiDocumentCooker */
    Assets::CookerContribution MakeUiDocumentCooker(Assets::AssetRegistrySnapshot registry, const AssetCookTargetId &target) {
        return {"horo.builtin.runtime-ui-document",
                Assets::AssetTypeId::Parse(Runtime::Ui::UiDocumentAssetTypeName).Value(),
                {target},
                std::make_shared<const UiDocumentCooker>(std::move(registry))};
    }
}  // namespace Horo::Application::PrefabCookDetail
