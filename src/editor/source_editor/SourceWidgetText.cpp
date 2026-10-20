#include "SourceWidgetText.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>

namespace Horo::Editor::SourceWidgetText {
    namespace {
        /** @brief Finds unchanged leading bytes and retracts through a partial shared scalar. */
        std::size_t CommonPrefix(const std::string_view before, const std::string_view after) {
            std::size_t count = 0;
            while (count < before.size() && count < after.size() && before[count] == after[count])
                ++count;
            while (count > 0 && count < after.size() && (static_cast<unsigned char>(after[count]) & 0xC0U) == 0x80U)
                --count;
            return count;
        }

        /** @brief Finds unchanged trailing bytes without overlapping the prefix or splitting a scalar. */
        std::size_t CommonSuffix(const std::string_view before, const std::string_view after, const std::size_t prefix) {
            std::size_t count = 0;
            while (count < after.size() - prefix && count < before.size() - prefix &&
                   after[after.size() - count - 1] == before[before.size() - count - 1])
                ++count;
            while (count > 0 && (static_cast<unsigned char>(after[after.size() - count]) & 0xC0U) == 0x80U)
                --count;
            return count;
        }
    }  // namespace

    /** @copydoc Compare */
    Difference Compare(const std::string_view before, const std::string_view after) {
        const auto prefix = CommonPrefix(before, after);
        return {prefix, CommonSuffix(before, after, prefix)};
    }

    /** @copydoc ClampOffset */
    std::size_t ClampOffset(const SourceDocumentSnapshot &source, std::size_t offset) {
        offset = std::min(offset, source.Text().size());
        while (!Boundary(source, offset) && offset > 0)
            --offset;
        return source.Metadata().utf8Bom ? std::max(offset, std::size_t{3}) : offset;
    }

    /** @copydoc Boundary */
    bool Boundary(const SourceDocumentSnapshot &source, const std::size_t offset) {
        const auto text = source.Text();
        if (offset > text.size() || (source.Metadata().utf8Bom && offset < 3))
            return false;
        if (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
            return false;
        return offset == 0 || offset == text.size() || text[offset - 1] != '\r' || text[offset] != '\n';
    }

    /** @copydoc DisplayText */
    std::string DisplayText(std::string_view text) {
        std::string result;
        result.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') {
                result += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n')
                    ++i;
            } else {
                result += text[i];
            }
        }
        return result;
    }

    /** @copydoc RawOffset */
    std::size_t RawOffset(const std::string_view text, const bool bom, const std::size_t displayOffset) {
        std::size_t raw = bom ? 3 : 0;
        for (std::size_t i = 0; i < displayOffset && raw < text.size(); ++i, ++raw) {
            if (text[raw] == '\r' && raw + 1 < text.size() && text[raw + 1] == '\n')
                ++raw;
        }
        return raw;
    }

    /** @copydoc RawOffset */
    std::size_t RawOffset(const SourceDocumentSnapshot &source, const std::size_t displayOffset) {
        return RawOffset(source.Text(), source.Metadata().utf8Bom, displayOffset);
    }

    /** @copydoc DisplayOffset */
    std::size_t DisplayOffset(const SourceDocumentSnapshot &source, const std::size_t rawOffset) {
        const auto text = source.Text();
        std::size_t count = 0;
        for (std::size_t raw = source.Metadata().utf8Bom ? 3 : 0; raw < rawOffset; ++raw, ++count) {
            if (text[raw] == '\r' && raw + 1 < rawOffset && text[raw + 1] == '\n')
                ++raw;
        }
        return count;
    }

    /** @copydoc AuthoredText */
    std::string AuthoredText(const SourceDocumentSnapshot &source, const std::string_view display) {
        const auto text = source.Text();
        const auto newline = text.find_first_of("\r\n");
        std::string_view ending = "\n";
        if (newline != std::string_view::npos && text[newline] == '\r')
            ending = newline + 1 < text.size() && text[newline + 1] == '\n' ? "\r\n" : "\r";
        std::string result;
        for (const char ch : display) {
            if (ch == '\n')
                result += ending;
            else
                result += ch;
        }
        return result;
    }

    /** @copydoc AdmittedText */
    bool AdmittedText(const std::string_view text) {
        return text.size() <= SourceDocumentLimits{}.maximumDocumentBytes && IsValidUtf8ScalarSequence(text) &&
               std::ranges::none_of(text, [](const unsigned char ch) {
            return ch < 32 && ch != '\n' && ch != '\r' && ch != '\t' && ch != '\f';
        });
    }

}  // namespace Horo::Editor::SourceWidgetText
