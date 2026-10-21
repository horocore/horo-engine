#pragma once

/** @file UiInspectorDraft.h
 * @brief Detached Runtime UI layout/style drafts; document and history remain host-owned.
 */
#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>

namespace Horo::Editor {
    /** @brief Exact host-issued draft fence; state must advance on reload and history operations. */
    struct UiInspectorContext final {
        DocumentInstanceId document;
        UiCanvasDocumentStateId state;
        Runtime::Ui::UiDocumentRevision revision;
        Runtime::Ui::UiElementId element;
        std::uint64_t selectionRevision{};
        Runtime::Ui::RuntimeStyleGeneration styles;
        Assets::AssetRegistryRevision assets;
        std::uint64_t resourcePublication{};
        /** @brief Checks that every owner fence is present. @return Whether the context is usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiInspectorContext &) const noexcept = default;
    };

    /** @brief Parse bounded retained numeric text without changing the last valid typed destination.
     * @param bytes Null-terminated input, rejected when unterminated. @param value Last valid value.
     * @return Whether the entire finite number parsed without narrowing or fallback.
     */
    template <typename T, std::size_t N> [[nodiscard]] bool ParseUiInspectorNumber(const std::array<char, N> &bytes, T &value) noexcept {
        const auto end = std::find(bytes.begin(), bytes.end(), '\0');
        const auto count = end == bytes.end() ? 0 : std::distance(bytes.begin(), end);
        const char *last = bytes.data() + count;
        T candidate{};
        const auto parsed = std::from_chars(bytes.data(), last, candidate);
        bool valid = parsed.ec == std::errc{} && parsed.ptr == last;
        if constexpr (std::floating_point<T>)
            valid = valid && std::isfinite(candidate);
        if (valid)
            value = candidate;
        return valid;
    }

    /** @brief Fixed storage for exact integers and fixed-point values, retaining invalid user input. */
    struct UiInspectorInteger final {
        std::array<char, 24> bytes{};
        bool valid{true};

        /** @brief Captures exact decimal text. @param value Logical integer, never converted to float. */
        template <std::integral T>
            requires(!std::same_as<T, bool>)
        void Capture(T value) noexcept {
            bytes.fill('\0');
            const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size() - 1, value);
            valid = result.ec == std::errc{};
        }

        /** @brief Parses the bounded buffer without changing the destination on failure.
         * @param value Last valid typed value. @return Whether complete text parsed successfully.
         */
        template <std::integral T>
            requires(!std::same_as<T, bool>)
        [[nodiscard]] bool Parse(T &value) noexcept {
            valid = ParseUiInspectorNumber(bytes, value);
            return valid;
        }
    };

    /** @brief Lossless retained decimal text for canonical double values. */
    struct UiInspectorDouble final {
        std::array<char, 64> bytes{};
        bool valid{true};

        /** @brief Capture exact round-trip decimal text. @param value Current typed value. */
        void Capture(double value) noexcept {
            bytes.fill('\0');
            const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size() - 1, value, std::chars_format::general,
                                              std::numeric_limits<double>::max_digits10);
            valid = result.ec == std::errc{} && std::isfinite(value);
        }

        /** @brief Parse on edits only, preserving the last typed value on malformed/nonfinite input.
         * @param value Typed destination. @return Whether complete finite text parsed.
         */
        [[nodiscard]] bool Parse(double &value) noexcept {
            valid = ParseUiInspectorNumber(bytes, value);
            return valid;
        }
    };

    /** @brief Detached values only; no property keys, registry, mutable document or runtime handles. */
    struct UiInspectorValues final {
        Runtime::Ui::UiLayoutStyle layout;
        std::optional<Runtime::Ui::UiStyleAssignment> style;
    };

    /** @brief One validated detached request, to be submitted through the canonical document command adapter. */
    struct UiInspectorPreparedEdit final {
        UiInspectorContext expected;
        UiInspectorValues values;
    };

    /** @brief Owner-thread draft for one selected element and an optional selected style property.
     * @details Begin captures only on fence changes. Prepare performs no mutation and does not grant
     * document edit permission or asset-resolution evidence. The host must recheck the exact context
     * and invoke its canonical command validator before publishing a transaction.
     */
    class UiInspectorDraft final {
    public:
        /** @brief Captures a new source or clears on invalid identity.
         * @param context Exact host source fences. @param values Immutable baseline values.
         * @return Whether the draft was replaced. Same-context calls retain invalid staged input.
         */
        bool Begin(UiInspectorContext context, const UiInspectorValues &values);
        /** @brief Drops all staged values and closes request admission. */
        void Clear() noexcept;
        /** @brief Restores captured values without publishing a document command. */
        void Cancel() noexcept;
        /** @brief Returns the detached editable values. @return Draft storage valid until Begin/Clear. */
        [[nodiscard]] UiInspectorValues &Values() noexcept;
        /** @brief Prepares a typed edit after layout and registered style validation.
         * @param current Current host fences; stale drafts are rejected before value work.
         * @param registry Optional active immutable registry; null performs canonical local authoring validation only.
         * @param textValid Aggregate validity of retained edit buffers; false rejects without fallback values.
         * @return Detached request or typed stale/lifecycle/layout/style failure. No document/history mutation.
         */
        [[nodiscard]] Result<UiInspectorPreparedEdit> Prepare(const UiInspectorContext &current,
                                                              const Runtime::Ui::RuntimeStyleRegistry *registry, bool textValid) const;

        /** @brief Builds one canonical presentation command, preserving unrelated assignments and fields.
         * @param current Exact current host fences. @param presentation Detached complete authored presentation.
         * @param textValid Aggregate retained text validity. @return Locally validated command or a typed failure.
         * @details Registry-dependent resolution remains an explicit runtime/host capability; this method does not invent a registry.
         */
        [[nodiscard]] Result<UiCanvasSetPresentation> PreparePresentation(const UiInspectorContext &current,
                                                                          const Runtime::Ui::UiElementPresentation &presentation,
                                                                          bool textValid) const;

    private:
        std::optional<UiInspectorContext> context_;
        UiInspectorValues baseline_;
        UiInspectorValues values_;
    };

    /** @brief Checks current selection and exact publication fences before canonical command execution.
     * @param owner Current document owner. @param expected View-captured fences.
     * @param selectionRevision Current host selection revision. @param command Prepared canonical presentation edit.
     * @param resources Current pinned host capability; required when changing resource references or their selected visual state.
     * @return Canonical execution outcome; stale, closed or mismatched selection publishes nothing.
     */
    [[nodiscard]] Result<void> ExecuteUiInspectorPresentation(UiCanvasDocument &owner, const UiInspectorContext &expected,
                                                              std::uint64_t selectionRevision, const UiCanvasSetPresentation &command,
                                                              const UiInspectorResourceSnapshot *resources = nullptr);
}  // namespace Horo::Editor
