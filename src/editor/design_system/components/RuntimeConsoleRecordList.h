#pragma once

/** @file RuntimeConsoleRecordList.h @brief Non-installed virtual terminal record primitive over the original bounded history. */
#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Editor/EditorTheme.h"

#include <array>

namespace Horo::Editor::Ui {
    /** @brief Bounded formatted identity display; owns no Scene/entity/component storage. */
    struct RuntimeInspectionRecordRow final {
        std::array<char, 64> title{}, detail{};
        std::uint8_t titleBytes{}, detailBytes{};
    };

    /** @brief Current-frame identity projection over at most 256 copied rows; native paint remains bounded to 128. */
    struct RuntimeInspectionRecordListProps final {
        std::span<const RuntimeInspectionRecordRow> rows;
        std::uint64_t revision{};
        std::string_view empty, detailLabel;
        std::optional<std::size_t> selected;
        float height{};
    };

    /** @brief Frame-borrowed source and localized status copy; stores no parallel command/result history. */
    struct RuntimeConsoleRecordListProps final {
        const char *id{"##RuntimeConsoleRecords"};
        std::span<const Debug::HistoryEntry> rows;
        std::uint64_t revision{}; /**< Source mutation revision; also invalidates native row activation identities. */
        std::string_view empty;
        std::string_view pending;
        std::string_view unavailable;
        std::optional<std::size_t> selected;
        float height{}; /**< Requested screen-pixel height, clamped to 128 theme-resolved rows. */
        bool enabled{true};
        bool followTail{true}; /**< Keeps an already bottom-pinned view at the tail; deliberate upward scrolling is preserved. */
    };

    /** @brief Keyboard/pointer selection and actual bounded paint count for the shared component gallery. */
    struct RuntimeConsoleRecordListResult final {
        std::optional<std::size_t> activated;
        std::size_t painted{};
    };

    /** @brief Actual process store projection selected by the host; no producer or store is instantiated by this identity. */
    enum class RuntimeStoreRecordKind {
        Logs,
        Jobs
    };

    /** @brief Frame-borrowed bounded store view using the same virtual-list geometry and native activation protection. */
    struct RuntimeStoreRecordListProps final {
        const Debug::ConsoleStoreOverlays &source;
        RuntimeStoreRecordKind kind{};
        std::span<const std::string_view> labels; /**< Seven severity or operation-state labels, indexed by the actual source enum. */
        const char *id{"##RuntimeStoreRecords"};
        std::string_view empty;
        std::optional<std::size_t> selected;
        float height{};
        bool enabled{true};
        bool followTail{true};
    };

    /** @brief Draws at most 128 visible fixed-height rows using body/monospace typography and native keyboard navigation.
     * @param props Source borrowed only during this call; row activation is an ordinal valid only for the current history revision.
     * @param fonts Current atlas roles. @return Selection intent and actual paint count; never mutates the source.
     * Text is clipped without reducing its size; complete selected output is presented separately by the owning surface.
     * Native hover/selected/disabled/navigation states and custom text both use the current theme and disabled alpha.
     */
    [[nodiscard]] RuntimeConsoleRecordListResult RuntimeConsoleRecordList(const RuntimeConsoleRecordListProps &props,
                                                                          const Theme::Fonts &fonts);
    /** @brief Paints only borrowed visible log/job projections with actual severity/state, repeat count and optional progress.
     * @param props Existing projection and localized status labels; no query/refresh occurs during paint.
     * @param fonts Current typography. @return Activation intent and actual paint count, bounded to 128 rows.
     */
    [[nodiscard]] RuntimeConsoleRecordListResult RuntimeStoreRecordList(const RuntimeStoreRecordListProps &props,
                                                                        const Theme::Fonts &fonts);
    /** @brief Paints bounded copied identity rows with the same native selection, clipping and typography as other debug lists.
     * @param props Borrowed current display projection; querying/formatting is completed before painting.
     * @param fonts Current typography. @return Native selection intent and actual painted count, at most 128.
     */
    [[nodiscard]] RuntimeConsoleRecordListResult RuntimeInspectionRecordList(const RuntimeInspectionRecordListProps &props,
                                                                             const Theme::Fonts &fonts);
}  // namespace Horo::Editor::Ui
