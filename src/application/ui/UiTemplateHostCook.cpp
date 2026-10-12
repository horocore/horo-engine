#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostState.h"

#include <algorithm>
#include <map>

namespace Horo::Application {
    namespace {
        using namespace Runtime::Ui;

        struct ManifestCapacityExceeded final {};

        /** @brief Checks remaining ordinary-plus-rich dependency capacity before adding an owned candidate. */
        void AddRichResource(std::vector<UiAssetDependency> &resources, const UiAssetDependency &dependency, const std::size_t maximum) {
            if (resources.size() >= maximum)
                throw ManifestCapacityExceeded{};
            resources.push_back(dependency);
        }

        /** @brief Enumerates concrete rich style assets without interpreting property IDs or paths. */
        void StyleResources(const UiStyleValue &value, std::vector<UiAssetDependency> &resources, const std::size_t maximum) {
            std::visit([&]<typename T>(const T &item) {
                if constexpr (std::is_same_v<T, UiStyleTypography>)
                    AddRichResource(resources, {item.family, {}, true}, maximum);
                else if constexpr (std::is_same_v<T, UiStyleImage>)
                    AddRichResource(resources, {item.asset, {}, true}, maximum);
            }, value);
        }

        /** @brief Derives rich resource requirements from explicit frozen Localization/provider registrations. */
        Result<std::vector<UiAssetDependency>> RichResources(const UiTemplates::UiTemplateFlattenedDocument &document,
                                                             const UiTemplateResourceBindings &registrations, const std::size_t maximum) {
            std::vector<UiAssetDependency> result;
            for (const auto &style : document.styles) {
                if (style.assignment.value.referencesToken)
                    AddRichResource(result, {Assets::AssetId::FromBytes(style.assignment.value.token.asset.Bytes()), {}, true}, maximum);
                else
                    StyleResources(style.assignment.value.literal, result, maximum);
            }
            for (const auto &text : document.localizedTexts) {
                const UiTemplateMessageResource *selected = nullptr;
                for (const auto &resource : registrations.messages)
                    if (resource.owner.namespaceId == text.text.Key().namespaceId) {
                        if (selected || !resource.owner.IsValid())
                            return Result<std::vector<UiAssetDependency>>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
                        selected = &resource;
                    }
                if (!selected)
                    return Result<std::vector<UiAssetDependency>>::Failure(MakeError(UiTemplates::UiErrors::TemplateMissing));
                AddRichResource(result, selected->resource, maximum);
            }
            for (const auto &binding : document.bindings) {
                const UiTemplateBindingResource *selected = nullptr;
                for (const auto &resource : registrations.bindings)
                    if (resource.provider == binding.source.providerType) {
                        if (selected)
                            return Result<std::vector<UiAssetDependency>>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
                        selected = &resource;
                    }
                if (selected)
                    AddRichResource(result, selected->resource, maximum);
            }
            return Result<std::vector<UiAssetDependency>>::Success(std::move(result));
        }

    }  // namespace

    /** @brief Admits cumulative framed identity bytes before copying package/resource strings into candidate storage. */
    class UiTemplateHost::VerifiedManifest final {
    public:
        explicit VerifiedManifest(const std::size_t maximum) : maximum_(maximum), retained_(Domain.size()) {
            if (retained_ > maximum_)
                throw ManifestCapacityExceeded{};
        }

        void Add(const std::string_view value) {
            const auto prefix = std::to_string(value.size());
            const auto framing = prefix.size() + 1U;
            if (framing > maximum_ - retained_ || value.size() > maximum_ - retained_ - framing)
                throw ManifestCapacityExceeded{};
            identities_.emplace_back(value);
            retained_ += framing + value.size();
        }

        Sha256Digest Finish() {
            std::ranges::sort(identities_);
            identities_.erase(std::unique(identities_.begin(), identities_.end()), identities_.end());
            std::string bytes{Domain};
            bytes.reserve(retained_);
            for (const auto &identity : identities_) {
                bytes += std::to_string(identity.size());
                bytes += ':';
                bytes += identity;
            }
            return ComputeSha256(std::as_bytes(std::span{bytes}));
        }

    private:
        static constexpr std::string_view Domain{"horo.ui-template.verified-manifest.v1"};
        std::size_t maximum_;
        std::size_t retained_;
        std::vector<std::string> identities_;
    };

    namespace {
        /** @brief Checks complete captured template bytes and exact type/revision, not a caller digest alone. */
        bool ExactTemplate(const Assets::AssetCookPinnedSource &captured, const UiTemplates::UiTemplateReference &reference,
                           const Assets::AssetTypeId &type, const std::string &source, const Sha256Digest &digest) {
            return captured.sourceDigest == digest && digest == reference.revision && captured.record.type == type &&
                   captured.bytes.size() == source.size() && std::equal(captured.bytes.begin(), captured.bytes.end(), source.begin());
        }

        /** @brief Uses the existing verified package graph for each declared template requirement. */
        bool AllowsPackages(const UiTemplates::UiTemplateDocument &source,
                            const std::shared_ptr<const Packages::PackageRestoreGraph> &graph) {
            for (const auto &requirement : source.dependency.packages)
                if (!graph || std::ranges::none_of(graph->packages, [&](const auto &package) {
                    return package.lock.package == requirement.package && requirement.versions.Allows(package.lock.version);
                }))
                    return false;
            return true;
        }

        /** @brief Admits caller cardinalities before walking captured authority or allocating the manifest. */
        bool ValidCookRequest(const UiTemplates::UiTemplateDocumentSnapshot &snapshot, std::span<const UiTemplateInputLease> inputs,
                                const Assets::AssetTypeId &type, const UiTemplateResourceBindings &resources) {
            const auto maximum = snapshot.Limits().maximumDependencies;
            return !type.Value().empty() && !inputs.empty() && inputs.size() <= maximum &&
                   resources.messages.size() <= maximum && resources.bindings.size() <= maximum;
        }
    }  // namespace

    /** @copydoc UiTemplateHost::SelectResource */
    Result<const Assets::AssetCookPinnedSource *> UiTemplateHost::SelectResource(std::span<const UiTemplateInputLease> inputs,
                                                                                Assets::AssetId asset) {
        const Assets::AssetCookPinnedSource *selected = nullptr;
        for (const auto &input : inputs)
            if (const auto *candidate = input.state_->sources.Find(asset)) {
                if (selected &&
                    (selected->sourceDigest != candidate->sourceDigest || selected->metadataDigest != candidate->metadataDigest))
                    return Result<const Assets::AssetCookPinnedSource *>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
                selected = candidate;
            }
        return Result<const Assets::AssetCookPinnedSource *>::Success(selected);
    }

    /** @copydoc UiTemplateHost::VerifyTemplates */
    Result<void> UiTemplateHost::VerifyTemplates(const UiTemplates::UiTemplateDocumentSnapshot &snapshot,
                                                 const UiTemplates::UiTemplateFlattenedDocument &flat,
                                                 std::span<const UiTemplateInputLease> inputs, const Assets::AssetTypeId &type,
                                                 VerifiedManifest &identity) {
        for (const auto &reference : flat.acceptedDependencies) {
            auto source = snapshot.AcceptedTemplateSource(reference);
            auto typed = snapshot.AcceptedTemplate(reference);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            if (typed.HasError())
                return Result<void>::Failure(typed.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{source.Value()}));
            bool found = false;
            for (const auto &input : inputs) {
                const auto *captured = input.state_->sources.Find(reference.asset.asset);
                if (!captured || !ExactTemplate(*captured, reference, type, source.Value(), digest))
                    continue;
                if (!AllowsPackages(*typed.Value(), input.state_->packages))
                    return Result<void>::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
                identity.Add(reference.asset.asset.ToString() + FormatSha256(digest) + FormatSha256(captured->metadataDigest));
                found = true;
                break;
            }
            if (!found)
                return Result<void>::Failure(MakeError(UiTemplates::UiErrors::TemplateMissing));
        }
        return Result<void>::Success();
    }

    /** @copydoc UiTemplateHost::VerifyResources */
    Result<std::vector<Runtime::Ui::UiAssetDependency>> UiTemplateHost::VerifyResources(
        const UiTemplates::UiTemplateFlattenedDocument &flat, const UiTemplateResourceBindings &resources,
        std::span<const UiTemplateInputLease> inputs, std::size_t maximum, VerifiedManifest &identity) {
        using Output = Result<std::vector<Runtime::Ui::UiAssetDependency>>;
        const auto ordinary = flat.ordinary.Dependencies();
        if (ordinary.size() > maximum)
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        auto rich = RichResources(flat, resources, maximum - ordinary.size());
        if (rich.HasError())
            return Output::Failure(rich.ErrorValue());
        auto dependencies = std::move(rich).Value();
        dependencies.insert(dependencies.end(), ordinary.begin(), ordinary.end());
        for (auto &dependency : dependencies) {
            const auto selection = SelectResource(inputs, dependency.asset);
            if (selection.HasError())
                return Output::Failure(selection.ErrorValue());
            const auto *selected = selection.Value();
            if (!selected) {
                if (dependency.required)
                    return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateMissing));
                identity.Add("absent:" + dependency.asset.ToString());
                continue;
            }
            if (!dependency.expectedType.Value().empty() && dependency.expectedType != selected->record.type)
                return Output::Failure(MakeError(Runtime::Ui::UiErrors::AssetIdentityMismatch));
            dependency.expectedType = selected->record.type;
            identity.Add(dependency.asset.ToString() + selected->record.type.Value() + FormatSha256(selected->sourceDigest) +
                         FormatSha256(selected->metadataDigest));
        }
        return Output::Success(std::move(dependencies));
    }

    UiTemplateHostCook::UiTemplateHostCook(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc UiTemplateHostCook::Document */
    const UiTemplates::UiTemplateCookedDocument &UiTemplateHostCook::Document() const noexcept {
        return state_->document;
    }

    /** @copydoc UiTemplateHost::Cook */
    Result<UiTemplateHostCook> UiTemplateHost::Cook(const UiTemplates::UiTemplateDocumentSnapshot &snapshot,
                                                    const std::span<const UiTemplateInputLease> inputs,
                                                    const Assets::AssetTypeId &templateType, const UiTemplateResourceBindings &resources,
                                                    const CancellationToken &cancellation) {
        using Output = Result<UiTemplateHostCook>;
        try {
            if (!ValidCookRequest(snapshot, inputs, templateType, resources))
                return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
            auto flat = snapshot.Flatten(cancellation);
            if (flat.HasError())
                return Output::Failure(flat.ErrorValue());
            VerifiedManifest identity{snapshot.Limits().maximumPayloadBytes};
            for (const auto &input : inputs) {
                if (!input.state_)
                    return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphInvalid));
                identity.Add(input.state_->packageIdentity);
            }
            if (const auto verified = VerifyTemplates(snapshot, flat.Value(), inputs, templateType, identity); verified.HasError())
                return Output::Failure(verified.ErrorValue());
            auto dependencies = VerifyResources(flat.Value(), resources, inputs, snapshot.Limits().maximumDependencies, identity);
            if (dependencies.HasError())
                return Output::Failure(dependencies.ErrorValue());
            const auto digest = identity.Finish();
            auto cooked = snapshot.Cook({digest, dependencies.Value()}, cancellation);
            if (cooked.HasError())
                return Output::Failure(cooked.ErrorValue());
            return Output::Success(UiTemplateHostCook{std::make_shared<const UiTemplateHostCook::State>(
                UiTemplateHostCook::State{std::move(cooked).Value(), snapshot, {inputs.begin(), inputs.end()}, resources, {}, {}})});
        } catch (const ManifestCapacityExceeded &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::Application
