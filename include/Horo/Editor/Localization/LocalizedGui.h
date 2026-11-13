#pragma once

/** @file LocalizedGui.h
 * @brief Required localization scope shared by modal, panel and screen surfaces.
 */

#include "Horo/Editor/Localization/ILocalizationService.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief Compile-time validated, value-owned catalog prefix, safe across UI module replacement. */
    class GuiLocalizationScope {
    public:
        static constexpr std::size_t MaxLength = 128; /**< Maximum prefix length in bytes, excluding the terminator. */

        /** @brief Validates a literal surface prefix during compilation.
         * @param scope Nonempty literal prefix without boundary dots, at most MaxLength bytes.
         * @throws std::invalid_argument During constant evaluation if the prefix is invalid.
         */
        consteval GuiLocalizationScope(const char *scope) {
            const std::string_view text{scope};
            if (text.empty() || text.size() > MaxLength || text.front() == '.' || text.back() == '.')
                throw std::invalid_argument("GUI localization scope must fit MaxLength and have no boundary dots");
            size_ = text.size();
            for (std::size_t index = 0; index < size_; ++index)
                value_[index] = text[index];
        }

        /** @brief Returns the validated prefix. @return View borrowed for this scope value's lifetime. */
        [[nodiscard]] constexpr std::string_view Value() const noexcept {
            return {value_.data(), size_};
        }

        /** @brief Resolves a relative key against the active locale.
         * @param localization Borrowed localization authority.
         * @param key Relative message key.
         * @return Translated value or the complete missing-key diagnostic.
         */
        [[nodiscard]] const std::string &Text(const ILocalizationService &localization, std::string_view key) const {
            return localization.GetScoped("editor", Value(), key);
        }

    private:
        std::array<char, MaxLength + 1> value_{};
        std::size_t size_{};
    };

    /** @brief Stores one mandatory message scope without owning services or translated values. */
    class LocalizedGui {
    public:
        /** @brief Returns the surface's immutable catalog-key prefix. @return Scope without a trailing dot. */
        [[nodiscard]] std::string_view LocalizationScope() const noexcept {
            return scope_.Value();
        }

    protected:
        /** @brief Requires a validated static scope at surface construction. @param scope Surface prefix. */
        explicit constexpr LocalizedGui(GuiLocalizationScope scope) noexcept : scope_(scope) {}

        ~LocalizedGui() = default;

        /** @brief Resolves a relative key against the caller's current localization snapshot.
         * @param localization Borrowed localization authority for this draw.
         * @param key Key relative to this surface's scope.
         * @return Active translated value, or the service's missing-key diagnostic.
         */
        [[nodiscard]] const std::string &LocalizedText(const ILocalizationService &localization, std::string_view key) const {
            return scope_.Text(localization, key);
        }

    private:
        const GuiLocalizationScope scope_;
    };
}  // namespace Horo::Editor
