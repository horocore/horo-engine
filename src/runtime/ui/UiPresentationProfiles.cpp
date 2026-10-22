#include "Horo/Runtime/Ui/UiPresentationProfiles.h"

#include "Horo/Runtime/Ui/UiHotReload.h"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint8_t KnownCapabilities = 15;

        /** @brief Authoring ingress verifies the actual isolated subtree, not just caller-provided identity fields. */
        bool MatchesAuthoringSource(const UiDocument &document, const UiElementTree &tree, const UiCanvasDescriptor &canvas) {
            std::size_t count{};
            const auto elements = document.Elements();
            for (const auto &element : elements) {
                const UiDocumentElement *ancestor = &element;
                std::size_t depth{};
                while (ancestor->parent.IsValid() && depth++ < MaximumUiTreeDepth) {
                    const auto parent = std::ranges::find(elements, ancestor->parent, &UiDocumentElement::id);
                    if (parent == elements.end())
                        return false;
                    ancestor = &*parent;
                }
                if (ancestor->id != canvas.rootElement)
                    continue;
                ++count;
                const auto handle = tree.Find(element.id);
                if (handle.HasError())
                    return false;
                const auto record = tree.Get(handle.Value());
                if (record.HasError())
                    return false;
                if (!element.parent.IsValid()) {
                    if (record.Value().parent.IsValid())
                        return false;
                } else {
                    const auto parent = tree.Find(element.parent);
                    if (parent.HasError() || record.Value().parent != parent.Value())
                        return false;
                }
            }
            return tree.Size() == (elements.empty() ? 1 : count);
        }

        /** @brief Checks the finite capability vocabulary without assuming a modality from another fact. */
        [[nodiscard]] bool ValidCapabilities(const UiPresentationCapabilities value) noexcept {
            return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(~KnownCapabilities)) == 0;
        }

        /** @brief Checks independent closed host classifications. */
        [[nodiscard]] bool ValidClassification(const UiPresentationFormFactor form, const UiPresentationViewRole role) noexcept {
            return form <= UiPresentationFormFactor::Television && role <= UiPresentationViewRole::Split;
        }

        /** @brief Compares admitted rational products with an explicit representability check. */
        [[nodiscard]] std::optional<std::strong_ordering> CompareRatio(const std::uint32_t leftNumerator,
                                                                       const std::uint32_t leftDenominator,
                                                                       const std::uint32_t rightNumerator,
                                                                       const std::uint32_t rightDenominator) noexcept {
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (leftDenominator == 0 || rightDenominator == 0 || leftNumerator > maximum / rightDenominator ||
                rightNumerator > maximum / leftDenominator)
                return std::nullopt;
            return static_cast<std::uint64_t>(leftNumerator) * rightDenominator <=>
                   static_cast<std::uint64_t>(rightNumerator) * leftDenominator;
        }

        /** @brief Matches a validated half-open extent interval. */
        [[nodiscard]] bool MatchesExtent(const UiPresentationExtentRange &range, const std::int32_t value) noexcept {
            return (!range.minimum || value >= *range.minimum) && (!range.maximum || value < *range.maximum);
        }

        /** @brief Matches a validated selector using only exact canonical logical facts. */
        [[nodiscard]] bool Matches(const UiPresentationProfileSelector &selector, const UiPresentationProfileEvidence &evidence) noexcept {
            if (!MatchesExtent(selector.width, evidence.content.width) || !MatchesExtent(selector.height, evidence.content.height) ||
                (selector.formFactor && *selector.formFactor != evidence.formFactor) ||
                (selector.viewRole && *selector.viewRole != evidence.viewRole) ||
                (static_cast<std::uint8_t>(evidence.available) & static_cast<std::uint8_t>(selector.required)) !=
                    static_cast<std::uint8_t>(selector.required))
                return false;
            const auto width = static_cast<std::uint32_t>(evidence.content.width);
            const auto height = static_cast<std::uint32_t>(evidence.content.height);
            if (selector.aspect.minimum) {
                const auto &bound = *selector.aspect.minimum;
                const auto compared = CompareRatio(width, height, bound.numerator, bound.denominator);
                if (!compared || *compared < 0)
                    return false;
            }
            if (selector.aspect.maximum) {
                const auto &bound = *selector.aspect.maximum;
                const auto compared = CompareRatio(width, height, bound.numerator, bound.denominator);
                if (!compared || *compared >= 0)
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc UiPresentationExtentRange::IsValid */
    bool UiPresentationExtentRange::IsValid() const noexcept {
        return (!minimum || *minimum > 0) && (!maximum || *maximum > 0) && (!minimum || !maximum || *minimum < *maximum);
    }

    /** @copydoc UiPresentationAspectRatio::IsValid */
    bool UiPresentationAspectRatio::IsValid() const noexcept {
        return numerator != 0 && denominator != 0 && std::gcd(numerator, denominator) == 1;
    }

    /** @copydoc UiPresentationAspectRange::IsValid */
    bool UiPresentationAspectRange::IsValid() const noexcept {
        if ((minimum && !minimum->IsValid()) || (maximum && !maximum->IsValid()))
            return false;
        if (!minimum || !maximum)
            return true;
        const auto compared = CompareRatio(minimum->numerator, minimum->denominator, maximum->numerator, maximum->denominator);
        return compared && *compared < 0;
    }

    /** @copydoc UiPresentationProfileSelector::IsValid */
    bool UiPresentationProfileSelector::IsValid() const noexcept {
        return width.IsValid() && height.IsValid() && aspect.IsValid() &&
               ValidClassification(formFactor.value_or(UiPresentationFormFactor::Unknown),
                                   viewRole.value_or(UiPresentationViewRole::Single)) &&
               ValidCapabilities(required);
    }

    /** @copydoc UiPresentationProfileEvidence::IsValid */
    bool UiPresentationProfileEvidence::IsValid() const noexcept {
        return canvas.IsValid() && instance.IsValid() && runtimeCanvas.IsValid() && tree.IsValid() && attachment.IsValid() &&
               revision.IsValid() && content.width > 0 && content.height > 0 && ValidClassification(formFactor, viewRole) &&
               ValidCapabilities(available);
    }

    /** @copydoc UiPresentationProfileTable::UiPresentationProfileTable */
    UiPresentationProfileTable::UiPresentationProfileTable(const UiElementTree &tree, const UiCanvasId canvas,
                                                           const UiPresentationProfileRevision revision,
                                                           std::vector<UiPresentationProfile> rows) noexcept
        : document_(tree.SourceDocument()), documentRevision_(tree.SourceDocumentRevision()), canvas_(canvas), instance_(tree.Instance()),
          runtimeCanvas_(tree.Canvas()), root_(tree.Root().Value().handle), issuer_(tree.IssuerPin()), revision_(revision),
          rows_(std::move(rows)) {}

    /** @copydoc ValidateUiPresentationProfiles */
    Result<void> ValidateUiPresentationProfiles(const std::span<const UiPresentationProfile> rows) {
        if (rows.size() > MaximumUiPresentationProfiles)
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        std::size_t totalOverrides = 0;
        for (const auto &row : rows) {
            if (row.overrides.size() > MaximumUiPresentationOverrides - totalOverrides)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            totalOverrides += row.overrides.size();
        }
        const auto bytes = rows.size() * sizeof(UiPresentationProfile) + totalOverrides * sizeof(UiPresentationLayoutOverride);
        if (bytes > MaximumUiPresentationTableBytes)
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto &row = rows[index];
            if (!ValidCapabilities(row.selector.required))
                return Result<void>::Failure(MakeError(UiErrors::PresentationProfileUnsupported));
            if (!row.id.IsValid() || !row.selector.IsValid())
                return Result<void>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
            for (std::size_t previous = 0; previous < index; ++previous)
                if (rows[previous].id == row.id || rows[previous].priority == row.priority)
                    return Result<void>::Failure(MakeError(UiErrors::DocumentDuplicateIdentity));
            for (const auto &override : row.overrides)
                if (!override.element.IsValid() || !override.layout.IsValid())
                    return Result<void>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPresentationProfileTable::Create */
    Result<UiPresentationProfileTable> UiPresentationProfileTable::Create(const UiReloadGeneration &generation, const UiCanvasId canvas,
                                                                          const UiPresentationProfileRevision revision,
                                                                          const std::span<const UiPresentationProfile> rows) {
        const auto canvases = generation.Canvases();
        const auto admitted = std::ranges::find(canvases, canvas, &UiReloadCanvas::id);
        if (admitted == canvases.end())
            return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        return CreateValidated(admitted->tree, canvas, revision, rows);
    }

    /** @copydoc UiPresentationProfileTable::CreateForAuthoring */
    Result<UiPresentationProfileTable> UiPresentationProfileTable::CreateForAuthoring(const UiDocument &document, const UiElementTree &tree,
                                                                                      const UiCanvasId canvas) {
        const auto canvases = document.Canvases();
        const auto found = std::ranges::find(canvases, canvas, &UiCanvasDescriptor::id);
        const auto root = tree.Root();
        if (found == canvases.end() || tree.SourceDocument() != document.Id() || tree.SourceDocumentRevision() != document.Revision() ||
            root.HasError() || root.Value().id != found->rootElement || !MatchesAuthoringSource(document, tree, *found))
            return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        const auto profiles = document.PresentationProfiles();
        const auto table = std::ranges::find(profiles, canvas, &UiDocumentPresentationProfiles::canvas);
        return table == profiles.end() ? CreateValidated(tree, canvas, UiPresentationProfileRevision::Create(1).Value(), {})
                                       : CreateValidated(tree, canvas, table->revision, table->rows);
    }

    /** @copydoc UiPresentationProfileTable::CreateValidated */
    Result<UiPresentationProfileTable> UiPresentationProfileTable::CreateValidated(const UiElementTree &tree, const UiCanvasId canvas,
                                                                                   const UiPresentationProfileRevision revision,
                                                                                   const std::span<const UiPresentationProfile> rows) {
        if (!canvas.IsValid() || !revision.IsValid())
            return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        if (const auto valid = ValidateUiPresentationProfiles(rows); valid.HasError())
            return Result<UiPresentationProfileTable>::Failure(valid.ErrorValue());
        if (tree.State() != UiElementTreeState::Active)
            return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        for (const auto &row : rows)
            for (const auto &item : row.overrides)
                if (tree.Find(item.element).HasError())
                    return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        try {
            std::vector<UiPresentationProfile> canonical(rows.begin(), rows.end());
            std::ranges::sort(canonical, {}, &UiPresentationProfile::priority);
            for (auto &row : canonical) {
                std::ranges::sort(row.overrides, {}, &UiPresentationLayoutOverride::element);
                if (std::ranges::adjacent_find(row.overrides, {}, &UiPresentationLayoutOverride::element) != row.overrides.end())
                    return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::DocumentDuplicateIdentity));
            }
            return Result<UiPresentationProfileTable>::Success(UiPresentationProfileTable{tree, canvas, revision, std::move(canonical)});
        } catch (const std::bad_alloc &) {
            return Result<UiPresentationProfileTable>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiPresentationProfileTable::MatchesTree */
    bool UiPresentationProfileTable::MatchesTree(const UiElementTree &tree, const UiPresentationProfileEvidence &evidence) const noexcept {
        if (tree.State() != UiElementTreeState::Active)
            return false;
        const auto root = tree.Root();
        return root.HasValue() && root.Value().handle == root_ && tree.IssuerPin() == issuer_ && tree.Instance() == instance_ &&
               tree.Canvas() == runtimeCanvas_ && tree.SourceDocument() == document_ &&
               tree.SourceDocumentRevision() == documentRevision_ && evidence.canvas == canvas_ && evidence.instance == instance_ &&
               evidence.runtimeCanvas == runtimeCanvas_ && evidence.tree == tree.Revision();
    }

    /** @copydoc UiPresentationProfileTable::Rows */
    std::span<const UiPresentationProfile> UiPresentationProfileTable::Rows() const noexcept {
        return rows_;
    }

    /** @copydoc UiPresentationProfileTable::Select */
    Result<UiPresentationProfileSelection> UiPresentationProfileTable::Select(const UiElementTree &tree,
                                                                              const UiPresentationProfileEvidence &evidence) const {
        if (!ValidCapabilities(evidence.available))
            return Result<UiPresentationProfileSelection>::Failure(MakeError(UiErrors::PresentationProfileUnsupported));
        if (!evidence.IsValid())
            return Result<UiPresentationProfileSelection>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        if (tree.Instance() != instance_ || tree.Canvas() != runtimeCanvas_ || tree.SourceDocument() != document_ ||
            evidence.canvas != canvas_ || evidence.instance != instance_ || evidence.runtimeCanvas != runtimeCanvas_ ||
            tree.IssuerPin() != issuer_)
            return Result<UiPresentationProfileSelection>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        if (!MatchesTree(tree, evidence))
            return Result<UiPresentationProfileSelection>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        UiPresentationProfileSelection selected{document_, documentRevision_, canvas_, revision_, evidence, {}, {}};
        for (const auto &row : rows_)
            if (Matches(row.selector, evidence)) {
                for (const auto &item : row.overrides)
                    if (tree.Find(item.element).HasError())
                        return Result<UiPresentationProfileSelection>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
                selected.profile = row.id;
                selected.priority = row.priority;
                break;
            }
        return Result<UiPresentationProfileSelection>::Success(selected);
    }

    /** @copydoc UiPresentationProfileTable::ApplyLayouts */
    Result<void> UiPresentationProfileTable::ApplyLayouts(const UiElementTree &tree, const UiPresentationProfileSelection &selection,
                                                          const std::span<UiLayoutElementDescriptor> descriptors) const {
        if (selection.document != document_ || selection.documentRevision != documentRevision_ || selection.canvas != canvas_ ||
            selection.tableRevision != revision_)
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        const auto expected = Select(tree, selection.evidence);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        if (selection != expected.Value())
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        if (descriptors.size() != tree.Size())
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const auto &descriptor = descriptors[index];
            if (!descriptor.style.IsValid() || !descriptor.intrinsic.IsValid() ||
                (index != 0 && descriptors[index - 1].element >= descriptor.element))
                return Result<void>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
            if (tree.Get(descriptor.element).HasError())
                return Result<void>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        }
        if (selection.profile) {
            const auto row = std::ranges::find(rows_, *selection.profile, &UiPresentationProfile::id);
            // Full membership, sorted uniqueness, and selected targets were validated before any base is changed.
            for (const auto &item : row->overrides) {
                const auto handle = tree.Find(item.element).Value();
                const auto descriptor = std::ranges::lower_bound(descriptors, handle, {}, &UiLayoutElementDescriptor::element);
                descriptor->style = item.layout;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPresentationProfileTable::ResolveLayout */
    Result<UiLayoutStyle> UiPresentationProfileTable::ResolveLayout(const UiElementTree &tree,
                                                                    const UiPresentationProfileSelection &selection,
                                                                    const UiElementId element, const UiLayoutStyle &base) const {
        if (!element.IsValid() || !base.IsValid())
            return Result<UiLayoutStyle>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        if (selection.document != document_ || selection.documentRevision != documentRevision_ || selection.canvas != canvas_ ||
            selection.tableRevision != revision_ || !MatchesTree(tree, selection.evidence) || tree.Find(element).HasError())
            return Result<UiLayoutStyle>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        const auto expected = Select(tree, selection.evidence);
        if (expected.HasError())
            return Result<UiLayoutStyle>::Failure(expected.ErrorValue());
        if (selection != expected.Value())
            return Result<UiLayoutStyle>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        if (selection.profile) {
            const auto row = std::ranges::find(rows_, *selection.profile, &UiPresentationProfile::id);
            const auto found = std::ranges::lower_bound(row->overrides, element, {}, &UiPresentationLayoutOverride::element);
            if (found != row->overrides.end() && found->element == element)
                return Result<UiLayoutStyle>::Success(found->layout);
        }
        return Result<UiLayoutStyle>::Success(base);
    }
}  // namespace Horo::Runtime::Ui
