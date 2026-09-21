#include "Horo/Runtime/Ui/UiDiagnostics.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::string_view UiErrorDomain = "horo.runtime_ui";

        /** @brief Owns the one canonical descriptor table used by implementation and contract tests. */
        const auto &DiagnosticDescriptors() noexcept {
            static const std::array descriptors{
                &UiErrors::IdentityInvalid,
                &UiErrors::OwnershipGenerationInvalid,
                &UiErrors::HandleMalformed,
                &UiErrors::HandleOwnerMismatch,
                &UiErrors::HandleStale,
                &UiErrors::RevisionInvalid,
                &UiErrors::RevisionStale,
                &UiErrors::GenerationExhausted,
                &UiErrors::DocumentInvalid,
                &UiErrors::DocumentDuplicateIdentity,
                &UiErrors::DependencyInvalid,
                &UiErrors::LocaleInvalid,
                &UiErrors::LocaleFallbackChainInvalid,
                &UiErrors::LocalizedKeyInvalid,
                &UiErrors::LocalizedMessageInvalid,
                &UiErrors::LocalizedArgumentInvalid,
                &UiErrors::LocalizedArgumentConflict,
                &UiErrors::LocalizedArgumentCapacityExceeded,
                &UiErrors::LocalizedAssetReferenceInvalid,
                &UiErrors::LocalizedAssetVariantConflict,
                &UiErrors::LocalizedAssetUnavailable,
                &UiErrors::CapacityExceeded,
                &UiErrors::PayloadInvalid,
                &UiErrors::CanvasReferenceInvalid,
                &UiErrors::CanvasSpaceInvalid,
                &UiErrors::CanvasSpaceModeMismatch,
                &UiErrors::CanvasSpaceOverflow,
                &UiErrors::InstanceStateInvalid,
                &UiErrors::ElementTreeInvalid,
                &UiErrors::ElementTreeIdentityConflict,
                &UiErrors::StructuralCommandInvalid,
                &UiErrors::StructuralCommandConflict,
                &UiErrors::ElementTreeLifecycleUnavailable,
                &UiErrors::LayoutInvalid,
                &UiErrors::LayoutSourceStale,
                &UiErrors::LayoutNonConvergent,
                &UiErrors::LayoutSnapshotStorageExhausted,
                &UiErrors::LayoutLifecycleUnavailable,
                &UiErrors::HitTestInvalid,
                &UiErrors::HitTestSourceStale,
                &UiErrors::HitTestNotPresented,
                &UiErrors::HitTestSnapshotStorageExhausted,
                &UiErrors::HitTestLifecycleUnavailable,
                &UiErrors::RenderSnapshotInvalid,
                &UiErrors::RenderCommandInvalid,
                &UiErrors::RenderResourceReferenceInvalid,
                &UiErrors::RenderSnapshotStorageExhausted,
                &UiErrors::RenderSnapshotLifecycleUnavailable,
                &UiErrors::RenderCompositionCapacityExceeded,
                &UiErrors::RenderCompositionInvalid,
                &UiErrors::RenderPresentationInvalid,
                &UiErrors::RenderPresentationStale,
                &UiErrors::EventDispatchInvalid,
                &UiErrors::EventDispatchSourceStale,
                &UiErrors::EventDispatchModalBoundaryViolation,
                &UiErrors::EventDispatchCapacityExceeded,
                &UiErrors::EventDispatchRouteInvalidated,
                &UiErrors::EventDispatchReentrant,
                &UiErrors::EventDispatchHandlerFailed,
                &UiErrors::EventDispatchLifecycleUnavailable,
                &UiErrors::ActionInvalid,
                &UiErrors::ActionPayloadInvalid,
                &UiErrors::ActionPayloadCapacityExceeded,
                &UiErrors::ActionCommandInvalid,
                &UiErrors::ActionQueueCapacityExceeded,
                &UiErrors::ActionSourceStale,
                &UiErrors::ActionResultInvalid,
                &UiErrors::ActionResultStale,
                &UiErrors::ActionHandlerFailed,
                &UiErrors::ActionLifecycleUnavailable,
                &UiErrors::NavigationInvalid,
                &UiErrors::DiagnosticInvalid,
                &UiErrors::DiagnosticUnsupported,
            };
            return descriptors;
        }

        /** @brief Checks that a correlation value contains one valid identity of the requested Runtime UI domain. */
        template <typename Identity> bool HasValidIdentity(const UiDiagnosticCorrelationValue &value) noexcept {
            const auto *identity = std::get_if<Identity>(&value);
            return identity != nullptr && identity->IsValid();
        }

        /** @brief Checks that a correlation value contains a player slot in the canonical uint8 range. */
        bool HasValidPlayer(const UiDiagnosticCorrelationValue &value) noexcept {
            const auto *player = std::get_if<std::uint64_t>(&value);
            return player != nullptr && *player <= std::numeric_limits<std::uint8_t>::max();
        }

        /** @brief Checks that a correlation value contains one non-zero scalar identity projection. */
        bool HasValidScalarIdentity(const UiDiagnosticCorrelationValue &value) noexcept {
            const auto *identity = std::get_if<std::uint64_t>(&value);
            return identity != nullptr && *identity != 0;
        }

        /** @brief Validates that one correlation key carries its exact declared value type and range. */
        bool IsValidCorrelationEntry(const UiDiagnosticCorrelationEntry &entry) noexcept {
            using enum UiDiagnosticCorrelationKey;
            switch (entry.key) {
                case Document:
                    return HasValidIdentity<UiDocumentId>(entry.value);
                case Element:
                    return HasValidIdentity<UiElementId>(entry.value);
                case Canvas:
                    return HasValidIdentity<UiCanvasId>(entry.value);
                case Player:
                    return HasValidPlayer(entry.value);
                case Viewport:
                case Operation:
                    return HasValidScalarIdentity(entry.value);
            }
            return false;
        }

        /** @brief Checks capacity, stable key order and every key-specific value invariant. */
        bool IsValidCorrelation(const std::span<const UiDiagnosticCorrelationEntry> correlation) noexcept {
            if (correlation.size() > MaximumUiDiagnosticCorrelationEntries)
                return false;
            for (std::size_t index = 0; index < correlation.size(); ++index) {
                if ((index != 0 && correlation[index].key <= correlation[index - 1].key) || !IsValidCorrelationEntry(correlation[index]))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc UiDiagnosticCategoryName */
    std::string_view UiDiagnosticCategoryName(const UiDiagnosticCategory category) noexcept {
        using enum UiDiagnosticCategory;
        switch (category) {
            case Document:
                return "runtime_ui.document";
            case Layout:
                return "runtime_ui.layout";
            case Text:
                return "runtime_ui.text";
            case Input:
                return "runtime_ui.input";
            case Focus:
                return "runtime_ui.focus";
            case Binding:
                return "runtime_ui.binding";
            case Render:
                return "runtime_ui.render";
            case Accessibility:
                return "runtime_ui.accessibility";
            case Lifecycle:
                return "runtime_ui.lifecycle";
        }
        return {};
    }

    /** @copydoc UiDiagnosticErrorDescriptors */
    std::span<const ErrorCodeDescriptor *const> UiDiagnosticErrorDescriptors() noexcept {
        return DiagnosticDescriptors();
    }

    /** @copydoc MakeUiDiagnosticRecord */
    Result<UiDiagnosticRecord> MakeUiDiagnosticRecord(const UiDiagnosticCategory category, const Error &error,
                                                      const std::span<const UiDiagnosticCorrelationEntry> correlation) {
        if (UiDiagnosticCategoryName(category).empty())
            return Result<UiDiagnosticRecord>::Failure(
                MakeError(UiErrors::DiagnosticUnsupported, "Unknown Runtime UI diagnostic category."));

        const auto code = DiagnosticCodeForDeclaredError(error, UiErrorDomain, UiDiagnosticErrorDescriptors());
        const auto severity = DiagnosticSeverityForError(error.severity);
        if (!code.has_value())
            return Result<UiDiagnosticRecord>::Failure(
                MakeError(UiErrors::DiagnosticUnsupported, "Unknown or foreign Runtime UI diagnostic source error."));
        if (!severity.has_value() || error.message.empty() || error.message.size() > MaximumUiDiagnosticMessageBytes)
            return Result<UiDiagnosticRecord>::Failure(
                MakeError(UiErrors::DiagnosticInvalid, "Runtime UI diagnostic evidence is malformed or exceeds its bounds."));
        if (!IsValidCorrelation(correlation))
            return Result<UiDiagnosticRecord>::Failure(
                MakeError(UiErrors::DiagnosticInvalid, "Runtime UI diagnostic correlation is unordered or malformed."));

        UiDiagnosticRecord record;
        record.category = category;
        record.code = *code;
        record.severity = *severity;
        record.message = error.message;
        record.correlationCount = static_cast<std::uint8_t>(correlation.size());
        std::ranges::copy(correlation, record.correlation.begin());
        return Result<UiDiagnosticRecord>::Success(std::move(record));
    }
}  // namespace Horo::Runtime::Ui
