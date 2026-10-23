#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "Horo/UiTemplates/UiTemplateSource.h"
#include "UiTemplateCodec.h"
#include "UiTemplateOwnerState.h"

#include <map>
#include <new>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;
        using State = Detail::UiTemplateOwnerState;

        /** @brief Rewrites only element-reference alternatives through the complete typed identity allocation. */
        void RemapReference(UiReference &reference, const std::map<UiElementId, UiElementId> &identities) {
            if (reference.kind == UiReferenceKind::Element)
                if (const auto found = identities.find(reference.element); found != identities.end())
                    reference.element = found->second;
        }

        /** @brief Resolves a detached element identity without changing references outside the complete allocation. */
        UiElementId RemapIdentity(const UiElementId id, const std::map<UiElementId, UiElementId> &identities) {
            const auto found = identities.find(id);
            return found == identities.end() ? id : found->second;
        }

        /** @brief Remaps complete ordinary element/reference payloads through the same allocation. */
        void RemapElement(UiDocumentElement &element, const std::map<UiElementId, UiElementId> &identities) {
            element.id = RemapIdentity(element.id, identities);
            element.parent = RemapIdentity(element.parent, identities);
            for (auto &reference : element.references)
                RemapReference(reference, identities);
            for (auto &property : element.properties)
                if (auto *reference = std::get_if<UiReference>(&property.value))
                    RemapReference(*reference, identities);
        }

        /** @brief Derives the independent binding identity from its prior identity and freshly allocated element. */
        Result<UiBindingId> DetachedBindingIdentity(const UiBindingDescriptor &binding) {
            Sha256Builder hash;
            if (constexpr std::string_view domain{"horo.ui-template.detached-binding.v1"};
                !hash.Update(std::as_bytes(std::span{domain.data(), domain.size()})) ||
                !hash.Update(std::as_bytes(std::span{binding.id.Bytes()})) ||
                !hash.Update(std::as_bytes(std::span{binding.target.element.Bytes()})))
                return Result<UiBindingId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            SerializedUiId bytes{};
            const auto digest = hash.Finalize();
            std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
            auto id = UiBindingId::Create(bytes);
            if (id.HasError())
                return Result<UiBindingId>::Failure(id.ErrorValue());
            return id;
        }

        /** @brief Copies ordinary canvas and element identity without changing their order. */
        Result<void> CopyStructure(UiDocumentBuilder &builder, const UiDocument &source) {
            for (const auto &canvas : source.Canvases())
                if (const auto result = builder.AddCanvas(canvas); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            for (const auto &element : source.Elements())
                if (const auto result = builder.AddElement(element); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Copies ordinary localized declarations and explicit dependencies through their typed builder admission. */
        Result<void> CopyResources(UiDocumentBuilder &builder, const UiDocument &source) {
            for (const auto &text : source.LocalizedTexts())
                if (const auto result = builder.AddLocalizedText(text); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            for (const auto &asset : source.LocalizedAssets())
                if (const auto result = builder.AddLocalizedAsset(asset); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            for (const auto &dependency : source.Dependencies())
                if (const auto result = builder.RequireAsset(dependency); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Copies complete responsive profile tables through typed builder admission. */
        Result<void> CopyPresentationProfiles(UiDocumentBuilder &builder, const UiDocument &source) {
            for (const auto &profiles : source.PresentationProfiles())
                if (auto result = builder.AddPresentationProfiles(profiles); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        /** @brief Publishes typed asset requirements carried by ordinary expanded references into the candidate builder. */
        Result<void> RequireReference(UiDocumentBuilder &builder, const UiReference &reference) {
            if (reference.kind != UiReferenceKind::Asset)
                return Result<void>::Success();
            return builder.RequireAsset({reference.asset, reference.expectedAssetType, true});
        }

        /** @brief Preserves one expanded element and its complete explicit/typed-property asset reference evidence. */
        Result<void> AppendExpandedElement(UiDocumentBuilder &builder, const UiDocumentElement &element) {
            for (const auto &reference : element.references)
                if (const auto result = RequireReference(builder, reference); result.HasError())
                    return result;
            for (const auto &property : element.properties)
                if (const auto *reference = std::get_if<UiReference>(&property.value))
                    if (const auto result = RequireReference(builder, *reference); result.HasError())
                        return result;
            return builder.AddElement(element);
        }

        /** @brief Composes an admitted projection's ordinary hierarchy and localized message authority. */
        Result<void> AppendExpansion(UiDocumentBuilder &builder, const UiTemplateExpansion &expansion) {
            for (const auto &element : expansion.elements)
                if (const auto result = AppendExpandedElement(builder, element); result.HasError())
                    return result;
            for (const auto &text : expansion.localizedTexts)
                if (const auto result = builder.AddLocalizedText(text.text); result.HasError())
                    return result;
            return Result<void>::Success();
        }

    }  // namespace

    using namespace Runtime::Ui;
    using State = Detail::UiTemplateOwnerState;

    namespace {
        using namespace Detail::Codec;

        /** @brief Hashes owned canonical bytes without retaining source authority in a runtime artifact. */
        Sha256Digest Digest(const std::string_view bytes) {
            return ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
        }

        /** @brief Commits complete admitted schemas independently of catalog ordering or process-local pointers. */
        std::string SchemaIdentity(const State &state) {
            Json schemas = Json::array({WriteSchemas(state.authoringSchemas->Schemas())});
            for (const auto &catalog : state.catalogs)
                schemas.push_back(WriteSchemas(catalog->Schemas()));
            auto bytes = schemas.dump();
            if (bytes.size() > state.limits.maximumPayloadBytes)
                throw Failure{MakeError(UiErrors::TemplateGraphBudgetExceeded)};
            return bytes;
        }

    }  // namespace

    /** @copydoc Detail::MaterializeTemplateExpansion */
    Result<UiTemplateExpansion> Detail::MaterializeTemplateExpansion(const UiTemplateExpansion &accepted,
                                                                     const std::span<const UiTemplateDetachIdentity> allocation) {
        std::map<UiElementId, UiElementId> identities;
        for (const auto &identity : allocation)
            identities.try_emplace(identity.expanded, identity.independent);
        auto output = accepted;
        output.root = RemapIdentity(output.root, identities);
        for (auto &element : output.elements)
            RemapElement(element, identities);
        for (auto &style : output.styles)
            style.element = RemapIdentity(style.element, identities);
        for (auto &text : output.localizedTexts)
            text.element = RemapIdentity(text.element, identities);
        for (auto &binding : output.bindings) {
            binding.target.element = RemapIdentity(binding.target.element, identities);
            auto id = DetachedBindingIdentity(binding);
            if (id.HasError())
                return Result<UiTemplateExpansion>::Failure(id.ErrorValue());
            binding.id = id.Value();
        }
        output.acceptedDependencies.clear();
        output.templateOwnedElements.clear();
        return Result<UiTemplateExpansion>::Success(std::move(output));
    }

    /** @copydoc Detail::Codec::CookIdentity */
    Sha256Digest Detail::Codec::CookIdentity(const Sha256Digest &source, const Sha256Digest &schemas, const Sha256Digest &manifest,
                                             const CookedUiDocument &ordinary, const std::string_view rich) {
        const Json identity{{"algorithm", 1},
                            {"source", Hex(source.bytes)},
                            {"schemas", Hex(schemas.bytes)},
                            {"dependencies", Hex(manifest.bytes)},
                            {"ordinary", Hex(ComputeSha256(std::as_bytes(ordinary.Payload())).bytes)},
                            {"rich", Hex(Digest(rich).bytes)}};
        return Digest(identity.dump());
    }

    /** @copydoc Detail::CopyTemplateOrdinary */
    Result<UiDocument> Detail::CopyTemplateOrdinary(const UiDocument &source, const UiDocumentRevision revision,
                                                    const std::span<const UiTemplateExpansion> expansions,
                                                    const std::span<const UiAssetDependency> dependencies) {
        UiDocumentBuilder builder{source.Id(), revision, source.SchemaVersion()};
        if (const auto copied = CopyStructure(builder, source); copied.HasError())
            return Result<UiDocument>::Failure(copied.ErrorValue());
        if (const auto copied = CopyResources(builder, source); copied.HasError())
            return Result<UiDocument>::Failure(copied.ErrorValue());
        for (const auto &route : source.Routes())
            if (const auto result = builder.AddRoute(route); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
        for (const auto &expansion : expansions)
            if (const auto result = AppendExpansion(builder, expansion); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
        for (const auto &dependency : dependencies)
            if (const auto result = builder.RequireAsset(dependency); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
        if (auto copied = CopyPresentationProfiles(builder, source); copied.HasError())
            return Result<UiDocument>::Failure(std::move(copied).ErrorValue());
        return std::move(builder).Build();
    }

    UiTemplateDocumentSnapshot::UiTemplateDocumentSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc UiTemplateDocumentSnapshot::Source */
    const UiTemplateAuthoredDocument &UiTemplateDocumentSnapshot::Source() const noexcept {
        return state_->source;
    }

    /** @copydoc UiTemplateDocumentSnapshot::Expansions */
    std::span<const UiTemplateExpansion> UiTemplateDocumentSnapshot::Expansions() const noexcept {
        return state_->expansions;
    }

    /** @copydoc UiTemplateDocumentSnapshot::Limits */
    const UiTemplateLimits &UiTemplateDocumentSnapshot::Limits() const noexcept {
        return state_->limits;
    }

    /** @copydoc UiTemplateDocumentSnapshot::AcceptedTemplateSource */
    Result<std::string> UiTemplateDocumentSnapshot::AcceptedTemplateSource(const UiTemplateReference &reference) const {
        for (const auto &catalog : state_->catalogs)
            if (catalog->Resolve(reference).HasValue())
                return SerializeTemplateSource(*catalog, reference);
        return Result<std::string>::Failure(MakeError(UiErrors::TemplateMissing));
    }

    /** @copydoc UiTemplateDocumentSnapshot::AcceptedTemplate */
    Result<const UiTemplateDocument *> UiTemplateDocumentSnapshot::AcceptedTemplate(const UiTemplateReference &reference) const {
        for (const auto &catalog : state_->catalogs) {
            const auto source = catalog->Resolve(reference);
            if (source.HasValue())
                return source;
        }
        return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateMissing));
    }

    /** @copydoc UiTemplateDocumentSnapshot::Flatten */
    Result<UiTemplateFlattenedDocument> UiTemplateDocumentSnapshot::Flatten(const CancellationToken &cancellation) const {
        try {
            if (cancellation.IsCancellationRequested())
                return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            auto ordinary = Detail::CopyTemplateOrdinary(state_->source.ordinary, state_->source.ordinary.Revision(), state_->expansions);
            if (ordinary.HasError())
                return Result<UiTemplateFlattenedDocument>::Failure(ordinary.ErrorValue());
            UiTemplateFlattenedDocument output{std::move(ordinary).Value(),
                                               state_->source.styles,
                                               state_->source.localizedTexts,
                                               state_->source.bindings,
                                               {}};
            for (const auto &expansion : state_->expansions) {
                if (cancellation.IsCancellationRequested())
                    return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
                output.styles.insert(output.styles.end(), expansion.styles.begin(), expansion.styles.end());
                output.localizedTexts.insert(output.localizedTexts.end(), expansion.localizedTexts.begin(), expansion.localizedTexts.end());
                output.bindings.insert(output.bindings.end(), expansion.bindings.begin(), expansion.bindings.end());
                output.acceptedDependencies.insert(output.acceptedDependencies.end(), expansion.acceptedDependencies.begin(),
                                                   expansion.acceptedDependencies.end());
            }
            return Result<UiTemplateFlattenedDocument>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    namespace {
        /** @brief Cooks one immutable snapshot in admission order, retaining complete source/schema/dependency identity. */
        Result<UiTemplateCookedDocument> CookSnapshot(const UiTemplateDocumentSnapshot &snapshot, const State &state,
                                                      const UiTemplateCookContext &context, const CancellationToken &cancellation) {
            using Output = Result<UiTemplateCookedDocument>;
            if (std::ranges::all_of(context.verifiedDependencyManifest.bytes, [](const auto byte) {
                return byte == 0;
            }))
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            auto flattened = snapshot.Flatten(cancellation);
            if (flattened.HasError())
                return Output::Failure(flattened.ErrorValue());
            auto value = std::move(flattened).Value();
            Runtime::Ui::UiDocumentCookLimits limits;
            limits.maximumElements = state.limits.maximumElements;
            limits.maximumPayloadBytes = state.limits.maximumPayloadBytes;
            auto admitted = Detail::CopyTemplateOrdinary(value.ordinary, value.ordinary.Revision(), {}, context.verifiedRichDependencies);
            if (admitted.HasError())
                return Output::Failure(admitted.ErrorValue());
            auto ordinary = Runtime::Ui::CookedUiDocument::Cook(admitted.Value(), limits);
            if (ordinary.HasError())
                return Output::Failure(ordinary.ErrorValue());
            auto durable = SerializeTemplateDocument(snapshot);
            if (durable.HasError())
                return Output::Failure(durable.ErrorValue());
            UiTemplateAuthoredDocument rich{std::move(value.ordinary),
                                            {},
                                            std::move(value.styles),
                                            std::move(value.localizedTexts),
                                            std::move(value.bindings)};
            auto payload = WriteRich(rich).dump();
            if (ordinary.Value().Payload().size() > state.limits.maximumPayloadBytes ||
                payload.size() > state.limits.maximumPayloadBytes - ordinary.Value().Payload().size())
                return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            const auto sourceIdentity = Digest(durable.Value());
            const auto schemaIdentity = Digest(SchemaIdentity(state));
            const auto identity =
                CookIdentity(sourceIdentity, schemaIdentity, context.verifiedDependencyManifest, ordinary.Value(), payload);
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            return Output::Success({std::move(ordinary).Value(), std::move(rich.styles), std::move(rich.localizedTexts),
                                    std::move(rich.bindings), std::move(payload), identity, context.verifiedDependencyManifest,
                                    sourceIdentity, schemaIdentity});
        }
    }  // namespace

    /** @copydoc UiTemplateDocumentSnapshot::Cook */
    Result<UiTemplateCookedDocument> UiTemplateDocumentSnapshot::Cook(const UiTemplateCookContext &context,
                                                                      const CancellationToken &cancellation) const {
        return Detail::Codec::CodecResult<UiTemplateCookedDocument>([this, &context, &cancellation] {
            return CookSnapshot(*this, *state_, context, cancellation);
        });
    }
}  // namespace Horo::UiTemplates
