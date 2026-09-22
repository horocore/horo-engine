#pragma once

/**
 * @file UiDocumentSerialization.h
 * @brief Bounded canonical source serialization, validation, and explicit migration for Runtime UI documents.
 */

#include "Horo/Runtime/Ui/UiDocument.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Hard parser ceilings for one Runtime UI source document. */
    struct UiDocumentSerializationLimits final {
        static constexpr std::size_t MaximumSourceBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t MaximumJsonDepth = 64;

        std::size_t maximumSourceBytes{MaximumSourceBytes};
        std::size_t maximumJsonDepth{MaximumJsonDepth};
        std::size_t maximumElements{MaximumUiDocumentElements};
        std::size_t maximumPropertiesPerElement{MaximumUiDocumentProperties};
        std::size_t maximumReferencesPerElement{MaximumUiDocumentReferences};
        std::size_t maximumRoutes{MaximumUiDocumentRoutes};
        std::size_t maximumTextBytes{MaximumUiDocumentTextBytes};

        /** @brief Checks caller limits are positive and never weaken compiled safety ceilings. @return True when usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return HasValidParserBounds() && HasValidDocumentBounds() && HasValidTextBounds();
        }

        [[nodiscard]] constexpr auto operator<=>(const UiDocumentSerializationLimits &) const noexcept = default;

    private:
        [[nodiscard]] constexpr bool HasValidParserBounds() const noexcept {
            return maximumSourceBytes > 0 && maximumSourceBytes <= MaximumSourceBytes && maximumJsonDepth > 0 &&
                   maximumJsonDepth <= MaximumJsonDepth;
        }

        [[nodiscard]] constexpr bool HasValidDocumentBounds() const noexcept {
            return maximumElements > 0 && maximumElements <= MaximumUiDocumentElements && maximumRoutes > 0 &&
                   maximumRoutes <= MaximumUiDocumentRoutes && maximumPropertiesPerElement > 0 &&
                   maximumPropertiesPerElement <= MaximumUiDocumentProperties && maximumReferencesPerElement > 0 &&
                   maximumReferencesPerElement <= MaximumUiDocumentReferences;
        }

        [[nodiscard]] constexpr bool HasValidTextBounds() const noexcept {
            return maximumTextBytes > 0 && maximumTextBytes <= MaximumUiDocumentTextBytes;
        }
    };

    /** @brief Explicit one-edge transformation between two durable UI source schemas. */
    using UiDocumentMigrationFunction = Result<UiDocument> (*)(const UiDocument &source);

    /** @brief Exact migration graph edge; version ordering never implies a migration. */
    struct UiDocumentMigrationStep final {
        UiDocumentSchemaVersion from;
        UiDocumentSchemaVersion to;
        UiDocumentMigrationFunction upgrade{};
    };

    /**
     * @brief Serializes one complete immutable document into canonical UTF-8 JSON.
     * @param document Validated authored Runtime UI document.
     * @param limits Explicit output and semantic bounds.
     * @return Deterministic owned source bytes or a typed validation/capacity failure.
     */
    [[nodiscard]] Result<std::string> SerializeUiDocument(const UiDocument &document, const UiDocumentSerializationLimits &limits = {});

    /**
     * @brief Parses one bounded canonical UI source document without mutating runtime state.
     * @param source Untrusted UTF-8 JSON source bytes.
     * @param limits Explicit parser and semantic bounds.
     * @return Detached validated document or a typed malformed/version/identity failure.
     */
    [[nodiscard]] Result<UiDocument> DeserializeUiDocument(std::string_view source, const UiDocumentSerializationLimits &limits = {});

    /**
     * @brief Applies a supplied exact migration chain to an immutable parsed document.
     * @param source Parsed source document; the input is never modified.
     * @param targetVersion Exact destination schema.
     * @param steps Explicit non-ambiguous version edges.
     * @param limits Bounds re-applied to every migrated candidate.
     * @return Detached migrated document or a typed missing/invalid migration failure.
     */
    [[nodiscard]] Result<UiDocument> MigrateUiDocument(const UiDocument &source, UiDocumentSchemaVersion targetVersion,
                                                       std::span<const UiDocumentMigrationStep> steps,
                                                       const UiDocumentSerializationLimits &limits = {});
}  // namespace Horo::Runtime::Ui
