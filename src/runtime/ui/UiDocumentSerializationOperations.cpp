#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"

#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        namespace Internal = SerializationInternal;

        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] Result<void> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<void>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] Result<void> AppendCanvases(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                  const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto canvas = Internal::DecodeCanvas(value, limits);
                if (canvas.HasError())
                    return Result<void>::Failure(canvas.ErrorValue());
                if (auto added = builder.AddCanvas(std::move(canvas).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendElements(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                  const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto element = Internal::DecodeElement(value, limits);
                if (element.HasError())
                    return Result<void>::Failure(element.ErrorValue());
                if (auto added = builder.AddElement(std::move(element).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendLocalizedTexts(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                        const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto text = Internal::DecodeLocalizedText(value, limits);
                if (text.HasError())
                    return Result<void>::Failure(text.ErrorValue());
                if (auto added = builder.AddLocalizedText(std::move(text).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendLocalizedAssets(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                         const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto reference = Internal::DecodeLocalizedAsset(value, limits);
                if (reference.HasError())
                    return Result<void>::Failure(reference.ErrorValue());
                if (auto added = builder.AddLocalizedAsset(std::move(reference).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendDependencies(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                      const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto dependency = Internal::DecodeDependency(value, limits);
                if (dependency.HasError())
                    return Result<void>::Failure(dependency.ErrorValue());
                if (auto added = builder.RequireAsset(std::move(dependency).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendRoutes(UiDocumentBuilder &builder, const Internal::Json &encoded,
                                                const UiDocumentSerializationLimits &limits) {
            for (const auto &value : encoded) {
                auto route = Internal::DecodeRoute(value, limits);
                if (route.HasError())
                    return Result<void>::Failure(route.ErrorValue());
                if (auto added = builder.AddRoute(std::move(route).Value()); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyCanvases(UiDocumentBuilder &builder, const std::span<const UiCanvasDescriptor> canvases) {
            for (const auto &canvas : canvases)
                if (auto result = builder.AddCanvas(canvas); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyElements(UiDocumentBuilder &builder, const std::span<const UiDocumentElement> elements) {
            for (const auto &element : elements)
                if (auto result = builder.AddElement(element); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyLocalizedTexts(UiDocumentBuilder &builder, const std::span<const UiLocalizedText> texts) {
            for (const auto &text : texts)
                if (auto result = builder.AddLocalizedText(text); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyLocalizedAssets(UiDocumentBuilder &builder,
                                                       const std::span<const UiLocalizedAssetReference> assets) {
            for (const auto &asset : assets)
                if (auto result = builder.AddLocalizedAsset(asset); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyDependencies(UiDocumentBuilder &builder, const std::span<const UiAssetDependency> dependencies) {
            for (const auto &dependency : dependencies)
                if (auto result = builder.RequireAsset(dependency); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CopyRoutes(UiDocumentBuilder &builder, const std::span<const UiRouteMetadata> routes) {
            for (const auto &route : routes)
                if (auto result = builder.AddRoute(route); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCloneLimits(const UiDocument &document, const UiDocumentSerializationLimits &limits) {
            if (document.Elements().size() > limits.maximumElements || document.Routes().size() > limits.maximumRoutes)
                return Failed(UiErrors::DocumentPayloadTooLarge);
            for (const auto &element : document.Elements()) {
                if (element.properties.size() > limits.maximumPropertiesPerElement ||
                    element.references.size() > limits.maximumReferencesPerElement)
                    return Failed(UiErrors::DocumentPayloadTooLarge);
                for (const auto &property : element.properties) {
                    if (property.key.size() > limits.maximumTextBytes)
                        return Failed(UiErrors::DocumentPayloadTooLarge);
                    if (const auto *text = std::get_if<std::string>(&property.value);
                        text != nullptr && text->size() > limits.maximumTextBytes)
                        return Failed(UiErrors::DocumentPayloadTooLarge);
                }
            }
            return Result<void>::Success();
        }

        struct DocumentSections final {
            UiDocumentSchemaVersion version;
            UiDocumentId id;
            UiDocumentRevision revision;
            const Internal::Json *canvases;
            const Internal::Json *elements;
            const Internal::Json *localizedTexts;
            const Internal::Json *localizedAssets;
            const Internal::Json *dependencies;
            const Internal::Json *routes;
        };

        [[nodiscard]] Result<UiDocumentSchemaVersion> DecodeSchemaVersion(const Internal::Json &value) {
            if (!Horo::Foundation::HasAllowedFields(value, {"major", "minor"}))
                return Failed<UiDocumentSchemaVersion>(UiErrors::DocumentSchemaUnsupported);
            auto major = Internal::ReadUnsigned<std::uint16_t>(value.at("major"));
            auto minor = Internal::ReadUnsigned<std::uint16_t>(value.at("minor"));
            if (major.HasError())
                return Result<UiDocumentSchemaVersion>::Failure(major.ErrorValue());
            if (minor.HasError())
                return Result<UiDocumentSchemaVersion>::Failure(minor.ErrorValue());
            const UiDocumentSchemaVersion version{major.Value(), minor.Value()};
            return Internal::IsSupportedVersion(version) ? Result<UiDocumentSchemaVersion>::Success(version)
                                                         : Failed<UiDocumentSchemaVersion>(UiErrors::DocumentSchemaUnsupported);
        }

        [[nodiscard]] Result<UiDocumentRevision> DecodeDocumentRevision(const Internal::Json &value) {
            auto revision = Internal::ReadUnsigned<std::uint64_t>(value);
            if (revision.HasError() || revision.Value() == 0)
                return Failed<UiDocumentRevision>(UiErrors::DocumentSerializationInvalid);
            return UiDocumentRevision::Create(revision.Value());
        }

        [[nodiscard]] bool HasValidDocumentArrays(const Internal::Json &canvases, const Internal::Json &elements,
                                                  const Internal::Json &localizedTexts, const Internal::Json &localizedAssets,
                                                  const Internal::Json &dependencies, const Internal::Json &routes,
                                                  const UiDocumentSerializationLimits &limits) {
            return canvases.is_array() && !canvases.empty() && canvases.size() <= MaximumUiDocumentCanvases && elements.is_array() &&
                   elements.size() <= limits.maximumElements && localizedTexts.is_array() &&
                   localizedTexts.size() <= MaximumUiDocumentLocalizedTexts && localizedAssets.is_array() &&
                   localizedAssets.size() <= MaximumUiDocumentLocalizedAssets && dependencies.is_array() &&
                   dependencies.size() <= MaximumUiDocumentDependencies && routes.is_array() && routes.size() <= limits.maximumRoutes;
        }

        [[nodiscard]] Result<DocumentSections> DecodeDocumentSections(const Internal::Json &root,
                                                                      const UiDocumentSerializationLimits &limits) {
            if (!Horo::Foundation::HasAllowedFields(root,
                                                    {"schemaVersion", "documentId", "revision", "canvases", "elements", "dependencies",
                                                     "routes"},
                                                    {"localizedTexts", "localizedAssets"}))
                return Failed<DocumentSections>(UiErrors::DocumentSerializationInvalid, "Runtime UI document fields are not canonical.");
            auto version = DecodeSchemaVersion(root.at("schemaVersion"));
            if (version.HasError())
                return Result<DocumentSections>::Failure(version.ErrorValue());
            auto id = Internal::DecodeUiId<UiDocumentId>(root.at("documentId"));
            if (id.HasError())
                return Result<DocumentSections>::Failure(id.ErrorValue());
            auto typedRevision = DecodeDocumentRevision(root.at("revision"));
            if (typedRevision.HasError())
                return Result<DocumentSections>::Failure(typedRevision.ErrorValue());
            const auto &canvases = root.at("canvases");
            const auto &elements = root.at("elements");
            static const Internal::Json emptyArray = Internal::Json::array();
            const auto &localizedTexts = root.contains("localizedTexts") ? root.at("localizedTexts") : emptyArray;
            const auto &localizedAssets = root.contains("localizedAssets") ? root.at("localizedAssets") : emptyArray;
            const auto &dependencies = root.at("dependencies");
            const auto &routes = root.at("routes");
            if (!HasValidDocumentArrays(canvases, elements, localizedTexts, localizedAssets, dependencies, routes, limits))
                return Failed<DocumentSections>(UiErrors::DocumentPayloadTooLarge);
            return Result<DocumentSections>::Success({version.Value(), id.Value(), typedRevision.Value(), &canvases, &elements,
                                                      &localizedTexts, &localizedAssets, &dependencies, &routes});
        }

        [[nodiscard]] Result<UiDocument> CloneDocument(const UiDocument &source, const UiDocumentSchemaVersion schemaVersion,
                                                       const UiDocumentSerializationLimits &limits) {
            UiDocumentBuilder builder{source.Id(), source.Revision(), schemaVersion};
            if (auto result = CopyCanvases(builder, source.Canvases()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = CopyElements(builder, source.Elements()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = CopyLocalizedTexts(builder, source.LocalizedTexts()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = CopyLocalizedAssets(builder, source.LocalizedAssets()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = CopyDependencies(builder, source.Dependencies()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = CopyRoutes(builder, source.Routes()); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            auto cloned = std::move(builder).Build();
            if (cloned.HasError())
                return cloned;
            if (auto valid = ValidateCloneLimits(cloned.Value(), limits); valid.HasError())
                return Result<UiDocument>::Failure(valid.ErrorValue());
            return cloned;
        }

        [[nodiscard]] Result<UiDocument> DecodeDocument(const Internal::Json &root, const UiDocumentSerializationLimits &limits) {
            auto sections = DecodeDocumentSections(root, limits);
            if (sections.HasError())
                return Result<UiDocument>::Failure(sections.ErrorValue());
            const auto &content = sections.Value();
            UiDocumentBuilder builder{content.id, content.revision, content.version};
            if (auto result = AppendCanvases(builder, *content.canvases, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = AppendElements(builder, *content.elements, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = AppendLocalizedTexts(builder, *content.localizedTexts, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = AppendLocalizedAssets(builder, *content.localizedAssets, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = AppendDependencies(builder, *content.dependencies, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            if (auto result = AppendRoutes(builder, *content.routes, limits); result.HasError())
                return Result<UiDocument>::Failure(result.ErrorValue());
            return std::move(builder).Build();
        }

        [[nodiscard]] bool IsMigrationStepValid(const UiDocumentMigrationStep &step, const std::span<const UiDocumentMigrationStep> steps,
                                                const std::size_t index) {
            if (!Internal::IsSupportedVersion(step.from) || !Internal::IsSupportedVersion(step.to) || step.to <= step.from ||
                step.upgrade == nullptr)
                return false;
            return std::ranges::none_of(steps.first(index), [&](const UiDocumentMigrationStep &previous) {
                return previous.from == step.from;
            });
        }

        [[nodiscard]] Result<void> ValidateSteps(const std::span<const UiDocumentMigrationStep> steps) {
            for (std::size_t index = 0; index < steps.size(); ++index)
                if (!IsMigrationStepValid(steps[index], steps, index))
                    return Failed(UiErrors::DocumentMigrationInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] const UiDocumentMigrationStep *FindMigrationStep(const UiDocumentSchemaVersion version,
                                                                       const std::span<const UiDocumentMigrationStep> steps) {
            const auto found = std::ranges::find(steps, version, &UiDocumentMigrationStep::from);
            return found == steps.end() ? nullptr : &*found;
        }

        [[nodiscard]] Result<UiDocument> ApplyMigration(const UiDocument &current, const UiDocumentMigrationStep &step,
                                                        const UiDocumentSerializationLimits &limits) {
            auto migrated = step.upgrade(current);
            if (migrated.HasError())
                return migrated;
            if (migrated.Value().SchemaVersion() != step.to)
                return Failed<UiDocument>(UiErrors::DocumentMigrationInvalid);
            if (auto valid = ValidateCloneLimits(migrated.Value(), limits); valid.HasError())
                return Result<UiDocument>::Failure(valid.ErrorValue());
            return Result<UiDocument>::Success(std::move(migrated).Value());
        }

        [[nodiscard]] Result<UiDocument> MigrateToVersion(UiDocument current, const UiDocumentSchemaVersion targetVersion,
                                                          const std::span<const UiDocumentMigrationStep> steps,
                                                          const UiDocumentSerializationLimits &limits) {
            std::size_t applied{};
            while (current.SchemaVersion() < targetVersion) {
                const auto *selected = FindMigrationStep(current.SchemaVersion(), steps);
                if (selected == nullptr)
                    return Failed<UiDocument>(UiErrors::DocumentMigrationMissing);
                if (++applied > steps.size())
                    return Failed<UiDocument>(UiErrors::DocumentMigrationInvalid);
                auto migrated = ApplyMigration(current, *selected, limits);
                if (migrated.HasError())
                    return migrated;
                current = std::move(migrated).Value();
            }
            return current.SchemaVersion() == targetVersion ? Result<UiDocument>::Success(std::move(current))
                                                            : Failed<UiDocument>(UiErrors::DocumentMigrationMissing);
        }

        [[nodiscard]] Internal::OrderedJson EncodeDocument(const UiDocument &document) {
            Internal::OrderedJson root{{"schemaVersion", Internal::OrderedJson{{"major", document.SchemaVersion().major},
                                                                               {"minor", document.SchemaVersion().minor}}},
                                       {"documentId", Internal::EncodeUiId(document.Id())},
                                       {"revision", document.Revision().Value()},
                                       {"canvases", Internal::OrderedJson::array()},
                                       {"elements", Internal::OrderedJson::array()},
                                       {"localizedTexts", Internal::OrderedJson::array()},
                                       {"localizedAssets", Internal::OrderedJson::array()},
                                       {"dependencies", Internal::OrderedJson::array()},
                                       {"routes", Internal::OrderedJson::array()}};
            for (const auto &canvas : document.Canvases())
                root.at("canvases").push_back(Internal::EncodeCanvas(canvas));
            for (const auto &element : document.Elements())
                root.at("elements").push_back(Internal::EncodeElement(element));
            for (const auto &text : document.LocalizedTexts())
                root.at("localizedTexts").push_back(Internal::EncodeLocalizedText(text));
            for (const auto &asset : document.LocalizedAssets())
                root.at("localizedAssets").push_back(Internal::EncodeLocalizedAsset(asset));
            for (const auto &dependency : document.Dependencies())
                root.at("dependencies").push_back(Internal::EncodeDependency(dependency));
            for (const auto &route : document.Routes())
                root.at("routes").push_back(Internal::EncodeRoute(route));
            return root;
        }
    }  // namespace

    /** @copydoc SerializeUiDocument */
    Result<std::string> SerializeUiDocument(const UiDocument &document, const UiDocumentSerializationLimits &limits) {
        if (!limits.IsValid())
            return Failed<std::string>(UiErrors::DocumentSerializationInvalid);
        auto validated = CloneDocument(document, document.SchemaVersion(), limits);
        if (validated.HasError())
            return Result<std::string>::Failure(validated.ErrorValue());
        std::string encoded = EncodeDocument(validated.Value()).dump();
        if (encoded.size() > limits.maximumSourceBytes)
            return Failed<std::string>(UiErrors::DocumentPayloadTooLarge);
        return Result<std::string>::Success(std::move(encoded));
    }

    /** @copydoc DeserializeUiDocument */
    Result<UiDocument> DeserializeUiDocument(const std::string_view source, const UiDocumentSerializationLimits &limits) {
        auto root = Internal::ParseJson(source, limits);
        if (root.HasError())
            return Result<UiDocument>::Failure(root.ErrorValue());
        try {
            return DecodeDocument(root.Value(), limits);
        } catch (const std::bad_alloc &) {
            return Failed<UiDocument>(UiErrors::DocumentPayloadTooLarge);
        } catch (const Internal::Json::exception &) {
            return Failed<UiDocument>(UiErrors::DocumentSerializationInvalid);
        }
    }

    /** @copydoc MigrateUiDocument */
    Result<UiDocument> MigrateUiDocument(const UiDocument &source, const UiDocumentSchemaVersion targetVersion,
                                         const std::span<const UiDocumentMigrationStep> steps,
                                         const UiDocumentSerializationLimits &limits) {
        if (!limits.IsValid() || !Internal::IsSupportedVersion(source.SchemaVersion()) || !Internal::IsSupportedVersion(targetVersion))
            return Failed<UiDocument>(UiErrors::DocumentSchemaUnsupported);
        auto validated = CloneDocument(source, source.SchemaVersion(), limits);
        if (validated.HasError())
            return validated;
        if (targetVersion == source.SchemaVersion())
            return validated;
        if (targetVersion < source.SchemaVersion())
            return Failed<UiDocument>(UiErrors::DocumentMigrationMissing);
        if (auto valid = ValidateSteps(steps); valid.HasError())
            return Result<UiDocument>::Failure(valid.ErrorValue());

        return MigrateToVersion(std::move(validated).Value(), targetVersion, steps, limits);
    }
}  // namespace Horo::Runtime::Ui
