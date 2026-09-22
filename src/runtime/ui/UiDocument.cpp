#include "Horo/Runtime/Ui/UiDocument.h"

#include "Horo/Foundation/Utf8.h"
#include "UiAssetDependencyInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> MergeDocumentDependency(std::vector<UiAssetDependency> &dependencies, UiAssetDependency dependency) {
            switch (Internal::MergeUiAssetDependency(dependencies, std::move(dependency), MaximumUiDocumentDependencies)) {
                case Internal::UiAssetDependencyMergeResult::Inserted:
                case Internal::UiAssetDependencyMergeResult::Strengthened:
                    return Result<void>::Success();
                case Internal::UiAssetDependencyMergeResult::CapacityExceeded:
                    return Failure(UiErrors::CapacityExceeded);
                case Internal::UiAssetDependencyMergeResult::Invalid:
                case Internal::UiAssetDependencyMergeResult::Conflict:
                    return Failure(UiErrors::DependencyInvalid);
            }
            return Failure(UiErrors::DependencyInvalid);
        }

        [[nodiscard]] bool IsValidSchemaVersion(const UiDocumentSchemaVersion version) noexcept {
            return version.major != 0 && version.major == CurrentUiDocumentSchemaVersion.major &&
                   version.minor <= CurrentUiDocumentSchemaVersion.minor;
        }

        [[nodiscard]] bool IsValidBand(const UiPresentationBand band) noexcept {
            return static_cast<std::uint8_t>(band) < static_cast<std::uint8_t>(UiPresentationBand::Count);
        }

        [[nodiscard]] bool IsValidReferenceKind(const UiReferenceKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) < static_cast<std::uint8_t>(UiReferenceKind::Count);
        }

        [[nodiscard]] bool IsValidText(const std::string &value) noexcept {
            return !value.empty() && value.size() <= MaximumUiDocumentTextBytes && IsValidUtf8ScalarSequence(value);
        }

        [[nodiscard]] bool IsElementReferenceShape(const UiReference &reference) noexcept {
            return reference.element.IsValid() && !reference.canvas.IsValid() && !reference.document.IsValid() &&
                   !reference.asset.IsValid() && reference.expectedAssetType.Value().empty();
        }

        [[nodiscard]] bool IsCanvasReferenceShape(const UiReference &reference) noexcept {
            return !reference.element.IsValid() && reference.canvas.IsValid() && !reference.document.IsValid() &&
                   !reference.asset.IsValid() && reference.expectedAssetType.Value().empty();
        }

        [[nodiscard]] bool IsDocumentReferenceShape(const UiReference &reference) noexcept {
            return !reference.element.IsValid() && !reference.canvas.IsValid() && reference.document.IsValid() &&
                   !reference.asset.IsValid() && reference.expectedAssetType.Value().empty();
        }

        [[nodiscard]] bool IsAssetReferenceShape(const UiReference &reference) noexcept {
            return !reference.element.IsValid() && !reference.canvas.IsValid() && !reference.document.IsValid() &&
                   reference.asset.IsValid() && !reference.expectedAssetType.Value().empty();
        }

        [[nodiscard]] bool IsValidReferenceShape(const UiReference &reference) noexcept {
            if (!IsValidReferenceKind(reference.kind))
                return false;
            using enum UiReferenceKind;
            switch (reference.kind) {
                case Element:
                    return IsElementReferenceShape(reference);
                case Canvas:
                    return IsCanvasReferenceShape(reference);
                case Document:
                    return IsDocumentReferenceShape(reference);
                case Asset:
                    return IsAssetReferenceShape(reference);
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsValidProperty(const UiTypedProperty &property) noexcept {
            if (!IsValidText(property.key))
                return false;
            return std::visit([]<typename T>(const T &value) {
                using Value = std::decay_t<T>;
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(value);
                if constexpr (std::is_same_v<Value, std::string>)
                    return value.size() <= MaximumUiDocumentTextBytes && IsValidUtf8ScalarSequence(value);
                if constexpr (std::is_same_v<Value, UiReference>)
                    return IsValidReferenceShape(value);
                return true;
            }, property.value);
        }

        [[nodiscard]] bool ContainsElement(const std::vector<UiDocumentElement> &elements, const UiElementId id) noexcept {
            return std::ranges::find(elements, id, &UiDocumentElement::id) != elements.end();
        }

        [[nodiscard]] bool ContainsCanvas(const std::vector<UiCanvasDescriptor> &canvases, const UiCanvasId id) noexcept {
            return std::ranges::find(canvases, id, &UiCanvasDescriptor::id) != canvases.end();
        }

        [[nodiscard]] bool ContainsDependency(const std::vector<UiAssetDependency> &dependencies, const UiReference &reference) noexcept {
            return std::ranges::any_of(dependencies, [&](const UiAssetDependency &dependency) {
                return dependency.asset == reference.asset && dependency.expectedType == reference.expectedAssetType;
            });
        }

        struct UiElementIdHash final {
            [[nodiscard]] std::size_t operator()(const UiElementId &id) const noexcept {
                std::size_t hash{};
                for (const auto byte : id.Bytes())
                    hash = (hash * 131U) ^ byte;
                return hash;
            }
        };

        [[nodiscard]] bool RootsMatchCanvases(const std::vector<UiCanvasDescriptor> &canvases,
                                              const std::vector<UiDocumentElement> &elements) {
            std::size_t roots{};
            for (const auto &element : elements) {
                if (!element.parent.IsValid()) {
                    ++roots;
                    if (!std::ranges::any_of(canvases, [&](const UiCanvasDescriptor &canvas) {
                        return canvas.rootElement == element.id;
                    }))
                        return false;
                } else if (!ContainsElement(elements, element.parent)) {
                    return false;
                }
            }
            if (roots != canvases.size())
                return false;
            return true;
        }

        [[nodiscard]] bool HasAcyclicParentChains(const std::vector<UiDocumentElement> &elements) {
            std::unordered_map<UiElementId, std::size_t, UiElementIdHash> indices;
            indices.reserve(elements.size());
            for (std::size_t index = 0; index < elements.size(); ++index)
                if (!indices.try_emplace(elements[index].id, index).second)
                    return false;

            enum class VisitState : std::uint8_t {
                Unvisited,
                Visiting,
                Visited
            };
            using enum VisitState;
            std::vector states(elements.size(), Unvisited);
            std::vector<std::size_t> path;
            path.reserve(elements.size());
            for (std::size_t start = 0; start < elements.size(); ++start) {
                if (states[start] == Visited)
                    continue;
                path.clear();
                auto current = start;
                while (states[current] != Visited) {
                    if (states[current] == Visiting)
                        return false;
                    states[current] = Visiting;
                    path.push_back(current);
                    const auto parent = elements[current].parent;
                    if (!parent.IsValid())
                        break;
                    const auto found = indices.find(parent);
                    if (found == indices.end())
                        return false;
                    current = found->second;
                }
                for (const auto index : path)
                    states[index] = Visited;
            }
            return true;
        }

        [[nodiscard]] bool IsHierarchyValid(const std::vector<UiCanvasDescriptor> &canvases,
                                            const std::vector<UiDocumentElement> &elements) {
            // Canvas-only documents remain valid for compatibility with the pre-element authored model.
            return elements.empty() || (RootsMatchCanvases(canvases, elements) && HasAcyclicParentChains(elements));
        }

        [[nodiscard]] Result<void> ValidateDocumentEnvelope(const UiDocumentSchemaVersion schemaVersion, const UiDocumentId id,
                                                            const UiDocumentRevision revision,
                                                            const std::vector<UiCanvasDescriptor> &canvases,
                                                            const std::vector<UiDocumentElement> &elements,
                                                            const std::vector<UiAssetDependency> &dependencies,
                                                            const std::vector<UiRouteMetadata> &routes) {
            if (!IsValidSchemaVersion(schemaVersion) || !id.IsValid() || !revision.IsValid())
                return Failure(UiErrors::DocumentInvalid);
            if (canvases.empty() || canvases.size() > MaximumUiDocumentCanvases || elements.size() > MaximumUiDocumentElements ||
                dependencies.size() > MaximumUiDocumentDependencies || routes.size() > MaximumUiDocumentRoutes)
                return Failure(UiErrors::DocumentInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCanvases(const std::vector<UiCanvasDescriptor> &canvases) {
            for (std::size_t index = 0; index < canvases.size(); ++index) {
                if (!canvases[index].IsValid())
                    return Failure(UiErrors::DocumentInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (canvases[previous].id == canvases[index].id || canvases[previous].rootElement == canvases[index].rootElement)
                        return Failure(UiErrors::DocumentDuplicateIdentity);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDependencies(const std::vector<UiAssetDependency> &dependencies) {
            for (std::size_t index = 0; index < dependencies.size(); ++index) {
                if (!dependencies[index].asset.IsValid() || dependencies[index].expectedType.Value().empty())
                    return Failure(UiErrors::DependencyInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (dependencies[previous].asset == dependencies[index].asset &&
                        dependencies[previous].expectedType != dependencies[index].expectedType)
                        return Failure(UiErrors::DependencyInvalid);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReferenceTarget(const UiReference &reference, const std::vector<UiCanvasDescriptor> &canvases,
                                                           const std::vector<UiDocumentElement> &elements,
                                                           const std::vector<UiAssetDependency> &dependencies) {
            using enum UiReferenceKind;
            switch (reference.kind) {
                case Element:
                    return ContainsElement(elements, reference.element) ? Result<void>::Success()
                                                                        : Failure(UiErrors::DocumentReferenceInvalid);
                case Canvas:
                    return ContainsCanvas(canvases, reference.canvas) ? Result<void>::Success()
                                                                      : Failure(UiErrors::DocumentReferenceInvalid);
                case Asset:
                    return ContainsDependency(dependencies, reference) ? Result<void>::Success()
                                                                       : Failure(UiErrors::DocumentReferenceInvalid);
                case Document:
                    return Result<void>::Success();
                case Count:
                    return Failure(UiErrors::DocumentReferenceInvalid);
            }
            return Failure(UiErrors::DocumentReferenceInvalid);
        }

        [[nodiscard]] Result<void> ValidateElementProperties(const UiDocumentElement &element) {
            for (std::size_t propertyIndex = 0; propertyIndex < element.properties.size(); ++propertyIndex) {
                const auto &property = element.properties[propertyIndex];
                if (!IsValidProperty(property))
                    return Failure(UiErrors::DocumentSerializationInvalid);
                for (std::size_t previous = 0; previous < propertyIndex; ++previous)
                    if (element.properties[previous].key == property.key)
                        return Failure(UiErrors::DocumentDuplicateProperty);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateElementReferences(const UiDocumentElement &element,
                                                             const std::vector<UiCanvasDescriptor> &canvases,
                                                             const std::vector<UiDocumentElement> &elements,
                                                             const std::vector<UiAssetDependency> &dependencies) {
            for (const auto &reference : element.references) {
                if (!IsValidReferenceShape(reference))
                    return Failure(UiErrors::DocumentReferenceInvalid);
                if (auto result = ValidateReferenceTarget(reference, canvases, elements, dependencies); result.HasError())
                    return result;
            }
            for (const auto &property : element.properties)
                if (const auto *reference = std::get_if<UiReference>(&property.value)) {
                    if (auto result = ValidateReferenceTarget(*reference, canvases, elements, dependencies); result.HasError())
                        return result;
                }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateElement(const UiDocumentElement &element, const std::vector<UiCanvasDescriptor> &canvases,
                                                   const std::vector<UiDocumentElement> &elements,
                                                   const std::vector<UiAssetDependency> &dependencies) {
            if (!element.id.IsValid() || element.type.Value().empty() || element.properties.size() > MaximumUiDocumentProperties ||
                element.references.size() > MaximumUiDocumentReferences)
                return Failure(UiErrors::DocumentSerializationInvalid);
            if (auto result = ValidateElementProperties(element); result.HasError())
                return result;
            return ValidateElementReferences(element, canvases, elements, dependencies);
        }

        [[nodiscard]] Result<void> ValidateElements(const std::vector<UiCanvasDescriptor> &canvases,
                                                    const std::vector<UiDocumentElement> &elements,
                                                    const std::vector<UiAssetDependency> &dependencies) {
            for (std::size_t index = 0; index < elements.size(); ++index) {
                const auto &element = elements[index];
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (elements[previous].id == element.id)
                        return Failure(UiErrors::DocumentDuplicateIdentity);
                if (auto result = ValidateElement(element, canvases, elements, dependencies); result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRoutes(const std::vector<UiRouteMetadata> &routes) {
            for (std::size_t index = 0; index < routes.size(); ++index) {
                if (!routes[index].id.IsValid() || !IsValidBand(routes[index].band))
                    return Failure(UiErrors::DocumentRouteInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (routes[previous].id == routes[index].id)
                        return Failure(UiErrors::DocumentRouteInvalid);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDocumentContent(const UiDocumentSchemaVersion schemaVersion, const UiDocumentId id,
                                                           const UiDocumentRevision revision,
                                                           const std::vector<UiCanvasDescriptor> &canvases,
                                                           const std::vector<UiDocumentElement> &elements,
                                                           const std::vector<UiAssetDependency> &dependencies,
                                                           const std::vector<UiRouteMetadata> &routes) {
            if (auto result = ValidateDocumentEnvelope(schemaVersion, id, revision, canvases, elements, dependencies, routes);
                result.HasError())
                return result;
            if (auto result = ValidateCanvases(canvases); result.HasError())
                return result;
            if (auto result = ValidateDependencies(dependencies); result.HasError())
                return result;
            if (auto result = ValidateElements(canvases, elements, dependencies); result.HasError())
                return result;
            if (!IsHierarchyValid(canvases, elements))
                return Failure(UiErrors::DocumentHierarchyInvalid);
            return ValidateRoutes(routes);
        }
    }  // namespace

    struct UiDocument::State final {
        UiDocumentSchemaVersion schemaVersion;
        UiDocumentId id;
        UiDocumentRevision revision;
        std::vector<UiCanvasDescriptor> canvases;
        std::vector<UiDocumentElement> elements;
        std::vector<UiLocalizedText> localizedTexts;
        std::vector<UiLocalizedAssetReference> localizedAssets;
        std::vector<UiAssetDependency> dependencies;
        std::vector<UiRouteMetadata> routes;
    };

    /** @copydoc UiDocument::UiDocument */
    UiDocument::UiDocument(State state) noexcept
        : schemaVersion_(state.schemaVersion), id_(state.id), revision_(state.revision), canvases_(std::move(state.canvases)),
          elements_(std::move(state.elements)), localizedTexts_(std::move(state.localizedTexts)),
          localizedAssets_(std::move(state.localizedAssets)), dependencies_(std::move(state.dependencies)),
          routes_(std::move(state.routes)) {}

    /** @copydoc UiDocument::SchemaVersion */
    UiDocumentSchemaVersion UiDocument::SchemaVersion() const noexcept {
        return schemaVersion_;
    }

    /** @copydoc UiDocument::Id */
    UiDocumentId UiDocument::Id() const noexcept {
        return id_;
    }

    /** @copydoc UiDocument::Revision */
    UiDocumentRevision UiDocument::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc UiDocument::Canvases */
    std::span<const UiCanvasDescriptor> UiDocument::Canvases() const noexcept {
        return canvases_;
    }

    /** @copydoc UiDocument::LocalizedTexts */
    std::span<const UiLocalizedText> UiDocument::LocalizedTexts() const noexcept {
        return localizedTexts_;
    }

    /** @copydoc UiDocument::LocalizedAssets */
    std::span<const UiLocalizedAssetReference> UiDocument::LocalizedAssets() const noexcept {
        return localizedAssets_;
    }

    /** @copydoc UiDocument::Elements */
    std::span<const UiDocumentElement> UiDocument::Elements() const noexcept {
        return elements_;
    }

    /** @copydoc UiDocument::Dependencies */
    std::span<const UiAssetDependency> UiDocument::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc UiDocument::Routes */
    std::span<const UiRouteMetadata> UiDocument::Routes() const noexcept {
        return routes_;
    }

    /** @copydoc UiDocumentBuilder::UiDocumentBuilder */
    UiDocumentBuilder::UiDocumentBuilder(UiDocumentId id, UiDocumentRevision revision, const UiDocumentSchemaVersion schemaVersion) noexcept
        : schemaVersion_(schemaVersion), id_(id), revision_(revision) {}

    /** @copydoc UiDocumentBuilder::AddCanvas */
    Result<void> UiDocumentBuilder::AddCanvas(UiCanvasDescriptor canvas) {
        if (canvases_.size() == MaximumUiDocumentCanvases)
            return Failure(UiErrors::CapacityExceeded);
        canvases_.push_back(std::move(canvas));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::AddLocalizedText */
    Result<void> UiDocumentBuilder::AddLocalizedText(UiLocalizedText text) {
        if (!text.IsValid())
            return Failure(UiErrors::LocalizedMessageInvalid);
        if (localizedTexts_.size() == MaximumUiDocumentLocalizedTexts)
            return Failure(UiErrors::CapacityExceeded);
        localizedTexts_.push_back(std::move(text));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::AddLocalizedAsset */
    Result<void> UiDocumentBuilder::AddLocalizedAsset(UiLocalizedAssetReference reference) {
        if (!reference.IsValid())
            return Failure(UiErrors::LocalizedAssetReferenceInvalid);
        if (localizedAssets_.size() == MaximumUiDocumentLocalizedAssets)
            return Failure(UiErrors::CapacityExceeded);

        std::size_t newDependencyCount = 0;
        for (const UiAssetDependency &dependency : reference.Dependencies()) {
            if (!dependency.asset.IsValid() || dependency.expectedType.Value().empty())
                return Failure(UiErrors::DependencyInvalid);
            const auto position = std::ranges::lower_bound(dependencies_, dependency.asset, {}, &UiAssetDependency::asset);
            if (position != dependencies_.end() && position->asset == dependency.asset) {
                if (position->expectedType != dependency.expectedType)
                    return Failure(UiErrors::DependencyInvalid);
            } else if (dependencies_.size() + newDependencyCount >= MaximumUiDocumentDependencies) {
                return Failure(UiErrors::CapacityExceeded);
            } else {
                ++newDependencyCount;
            }
        }

        localizedAssets_.reserve(localizedAssets_.size() + 1);
        dependencies_.reserve(dependencies_.size() + newDependencyCount);
        for (const UiAssetDependency &dependency : reference.Dependencies()) {
            const auto merged = Internal::MergeUiAssetDependency(dependencies_, dependency, MaximumUiDocumentDependencies);
            if (merged == Internal::UiAssetDependencyMergeResult::CapacityExceeded)
                return Failure(UiErrors::CapacityExceeded);
            if (merged == Internal::UiAssetDependencyMergeResult::Invalid || merged == Internal::UiAssetDependencyMergeResult::Conflict)
                return Failure(UiErrors::DependencyInvalid);
        }
        localizedAssets_.push_back(std::move(reference));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::AddElement */
    Result<void> UiDocumentBuilder::AddElement(UiDocumentElement element) {
        if (elements_.size() == MaximumUiDocumentElements)
            return Failure(UiErrors::CapacityExceeded);
        elements_.push_back(std::move(element));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::RequireAsset */
    Result<void> UiDocumentBuilder::RequireAsset(UiAssetDependency dependency) {
        return MergeDocumentDependency(dependencies_, std::move(dependency));
    }

    /** @copydoc UiDocumentBuilder::AddRoute */
    Result<void> UiDocumentBuilder::AddRoute(UiRouteMetadata route) {
        if (routes_.size() == MaximumUiDocumentRoutes)
            return Failure(UiErrors::CapacityExceeded);
        routes_.push_back(std::move(route));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::Build */
    Result<UiDocument> UiDocumentBuilder::Build() && {
        for (auto &element : elements_) {
            std::ranges::sort(element.properties, {}, &UiTypedProperty::key);
            std::ranges::sort(element.references);
        }
        if (const auto validated = ValidateDocumentContent(schemaVersion_, id_, revision_, canvases_, elements_, dependencies_, routes_);
            validated.HasError())
            return Result<UiDocument>::Failure(validated.ErrorValue());
        std::ranges::sort(dependencies_, {}, &UiAssetDependency::asset);
        std::ranges::sort(routes_, {}, &UiRouteMetadata::id);
        return Result<UiDocument>::Success(UiDocument{
            UiDocument::State{schemaVersion_, id_, revision_, std::move(canvases_), std::move(elements_), std::move(localizedTexts_),
                              std::move(localizedAssets_), std::move(dependencies_), std::move(routes_)}});
    }

    /** @copydoc ValidateUiCanvasAssetReference */
    Result<void> ValidateUiCanvasAssetReference(const UiCanvasAssetReference &reference) {
        if (!reference.asset.IsValid() || !reference.document.IsValid() || !reference.canvas.IsValid() ||
            !reference.minimumRevision.IsValid())
            return Failure(UiErrors::CanvasReferenceInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::UiRuntimeInstance */
    UiRuntimeInstance::UiRuntimeInstance(UiDocumentId document, UiDocumentRevision revision, std::vector<UiAssetDependency> dependencies,
                                         std::vector<std::uint8_t> payload, RuntimeUiInstanceId instance) noexcept
        : document_(document), revision_(revision), dependencies_(std::move(dependencies)), payload_(std::move(payload)),
          instance_(instance) {}

    /** @copydoc UiRuntimeInstance::Create */
    Result<UiRuntimeInstance> UiRuntimeInstance::Create(CookedUiDocument document, RuntimeUiInstanceId instance) {
        if (!instance.IsValid())
            return Failure<UiRuntimeInstance>(UiErrors::HandleMalformed);
        if (!document.Id().IsValid() || !document.SourceRevision().IsValid() || document.Payload().empty())
            return Failure<UiRuntimeInstance>(UiErrors::PayloadInvalid);
        return Result<UiRuntimeInstance>::Success(UiRuntimeInstance{document.Id(), document.SourceRevision(),
                                                                    std::move(document.dependencies_), std::move(document.payload_),
                                                                    instance});
    }

    /** @copydoc UiRuntimeInstance::InstanceId */
    RuntimeUiInstanceId UiRuntimeInstance::InstanceId() const noexcept {
        return instance_;
    }

    /** @copydoc UiRuntimeInstance::DocumentId */
    UiDocumentId UiRuntimeInstance::DocumentId() const noexcept {
        return document_;
    }

    /** @copydoc UiRuntimeInstance::DocumentRevision */
    UiDocumentRevision UiRuntimeInstance::DocumentRevision() const noexcept {
        return revision_;
    }

    /** @copydoc UiRuntimeInstance::State */
    UiRuntimeInstanceState UiRuntimeInstance::State() const noexcept {
        return state_;
    }

    /** @copydoc UiRuntimeInstance::Dependencies */
    std::span<const UiAssetDependency> UiRuntimeInstance::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc UiRuntimeInstance::Payload */
    std::span<const std::uint8_t> UiRuntimeInstance::Payload() const noexcept {
        return payload_;
    }

    /** @copydoc UiRuntimeInstance::Activate */
    Result<void> UiRuntimeInstance::Activate() {
        if (state_ != UiRuntimeInstanceState::Prepared)
            return Failure(UiErrors::InstanceStateInvalid);
        state_ = UiRuntimeInstanceState::Active;
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::BeginRetirement */
    Result<void> UiRuntimeInstance::BeginRetirement() {
        using enum UiRuntimeInstanceState;
        if (state_ != Prepared && state_ != Active)
            return Failure(UiErrors::InstanceStateInvalid);
        state_ = Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::Shutdown */
    void UiRuntimeInstance::Shutdown() noexcept {
        state_ = UiRuntimeInstanceState::Stopped;
        std::vector<UiAssetDependency>{}.swap(dependencies_);
        std::vector<std::uint8_t>{}.swap(payload_);
    }
}  // namespace Horo::Runtime::Ui
