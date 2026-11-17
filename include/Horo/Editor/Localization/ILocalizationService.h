#pragma once

#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief Headless interface for resolving localized strings. */
    class ILocalizationService {
    public:
        virtual ~ILocalizationService() = default;

        /**
         * @brief Returns a localized string reference for the given key.
         *
         * If the key is not found, the service returns a formatted missing key string
         * rather than crashing, ensuring safe usage in immediate-mode GUIs.
         */
        [[nodiscard]] virtual const std::string &Get(std::string_view namespaceId, std::string_view localKey) const = 0;

        /** @brief Resolves a key relative to a GUI surface's scope.
         * @param namespaceId Catalog namespace.
         * @param scope Dot-separated catalog-key prefix.
         * @param key Relative message key.
         * @return Translated value or a missing-key diagnostic for the complete key.
         * @details The default adapter delegates to Get; catalog implementations can override to avoid key concatenation.
         */
        [[nodiscard]] virtual const std::string &GetScoped(std::string_view namespaceId, std::string_view scope,
                                                           std::string_view key) const {
            std::string qualified{scope};
            if (!scope.empty())
                qualified += '.';
            qualified += key;
            return Get(namespaceId, qualified);
        }
    };
}  // namespace Horo::Editor
