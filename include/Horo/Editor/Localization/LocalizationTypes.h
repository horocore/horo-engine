#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief Normalized BCP 47 locale identity used by editor localization. */
    struct LocaleTag {
        std::string value;

        [[nodiscard]] static std::optional<LocaleTag> Parse(std::string_view tag);
        [[nodiscard]] bool operator==(const LocaleTag &) const = default;
    };

    /** @brief Stable namespace and semantic local key identifying one message. */
    struct MessageKey {
        std::string namespaceId;
        std::string localKey;

        [[nodiscard]] std::string Canonical() const;
        [[nodiscard]] bool operator==(const MessageKey &) const = default;
    };

    /** @brief Borrowed segmented key used for allocation-free scoped catalog lookup. */
    struct MessageKeyView {
        std::string_view namespaceId; /**< Borrowed catalog namespace. */
        std::string_view scope;       /**< Optional prefix; empty means the key is already canonical. */
        std::string_view key;         /**< Relative suffix, or the canonical local key when scope is empty. */
    };

    /** @brief Transparent hash for owned and segmented message keys. */
    struct MessageKeyHash {
        using is_transparent = void;
        /** @brief Hashes an owned canonical key. @param key Catalog identity. @return Stable in-process lookup hash. */
        [[nodiscard]] std::size_t operator()(const MessageKey &key) const noexcept;
        /** @brief Hashes the equivalent segmented identity. @param key Borrowed identity. @return The canonical key's lookup hash. */
        [[nodiscard]] std::size_t operator()(MessageKeyView key) const noexcept;
    };

    /** @brief Compares segmented scope/key views with the canonical owned catalog key. */
    struct MessageKeyEqual {
        using is_transparent = void;
        /** @brief Compares owned identities. @param left First key. @param right Second key. @return True for equal keys. */
        [[nodiscard]] bool operator()(const MessageKey &left, const MessageKey &right) const noexcept;
        /** @brief Compares canonical and segmented identities at dot boundaries.
         * @param left Owned catalog key. @param right Borrowed segmented key. @return True for the same namespace and complete key.
         */
        [[nodiscard]] bool operator()(const MessageKey &left, MessageKeyView right) const noexcept;
        /** @brief Compares segmented and canonical identities.
         * @param left Borrowed segmented key. @param right Owned catalog key. @return True for the same namespace and complete key.
         */
        [[nodiscard]] bool operator()(MessageKeyView left, const MessageKey &right) const noexcept;
    };

    /** @brief Value-owned deferred message reference resolved against the active snapshot. */
}  // namespace Horo::Editor
