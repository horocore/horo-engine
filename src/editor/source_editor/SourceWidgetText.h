#pragma once

/** @file SourceWidgetText.h
 * @brief Private conversion between exact authored bytes and the widget's LF presentation mirror.
 */
#include "Horo/Editor/SourceDocumentService.h"

namespace Horo::Editor::SourceWidgetText {
    /** @brief Scalar-aligned unchanged prefix/suffix lengths in two LF mirrors. */
    struct Difference final {
        std::size_t prefix{};
        std::size_t suffix{};
    };

    /** @brief Finds the minimal replacement interval without splitting a UTF-8 scalar.
     * @param before Original mirror. @param after Edited mirror. @return Unchanged boundary lengths.
     */
    [[nodiscard]] Difference Compare(std::string_view before, std::string_view after);
    /** @brief Clamps another view's old position to a valid boundary after a new publication.
     * @param source New source lease. @param offset Previous authored offset. @return Valid authored boundary.
     */
    [[nodiscard]] std::size_t ClampOffset(const SourceDocumentSnapshot &source, std::size_t offset);

    /** @brief Checks a complete document text boundary, including UTF-8, BOM and CRLF constraints.
     * @param source Authoritative publication. @param offset Exact authored byte offset. @return True for a valid presentation boundary.
     */
    [[nodiscard]] bool Boundary(const SourceDocumentSnapshot &source, std::size_t offset);
    /** @brief Normalizes only a presentation copy to LF. @param text UTF-8 bytes without a leading BOM.
     * @return LF mirror retaining all other characters.
     */
    [[nodiscard]] std::string DisplayText(std::string_view text);
    /** @brief Converts a display byte offset to authored bytes without an offset table.
     * @param text Exact document text. @param bom Whether a leading BOM is present. @param displayOffset LF mirror offset.
     * @return Authored offset, bounded by document size.
     */
    [[nodiscard]] std::size_t RawOffset(std::string_view text, bool bom, std::size_t displayOffset);
    /** @brief Converts a display offset against an authoritative publication.
     * @param source Exact publication. @param displayOffset LF mirror offset. @return Authored byte offset.
     */
    [[nodiscard]] std::size_t RawOffset(const SourceDocumentSnapshot &source, std::size_t displayOffset);
    /** @brief Converts a validated authored boundary to a display offset.
     * @param source Authoritative publication. @param rawOffset Valid authored boundary. @return LF mirror offset.
     */
    [[nodiscard]] std::size_t DisplayOffset(const SourceDocumentSnapshot &source, std::size_t rawOffset);
    /** @brief Restores the first authored newline family only in newly inserted bytes.
     * @param source Existing publication defining newline policy. @param display LF inserted text.
     * @return New authored insertion, preserving all non-newline bytes.
     */
    [[nodiscard]] std::string AuthoredText(const SourceDocumentSnapshot &source, std::string_view display);
    /** @brief Applies document text admission to bounded clipboard/IME content.
     * @param text UTF-8 proposed insertion. @return False for malformed, binary or oversized input.
     */
    [[nodiscard]] bool AdmittedText(std::string_view text);
}  // namespace Horo::Editor::SourceWidgetText
