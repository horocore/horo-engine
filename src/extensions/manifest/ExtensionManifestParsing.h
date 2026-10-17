#pragma once

#include "Horo/Extensions/ExtensionManifest.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace Horo::Extensions::ManifestParsing {
    using Json = nlohmann::json;

    /** @brief Owns a completed manifest DOM and preadmitted iterative retirement storage; no provider authority. */
    class ManifestDocument final {
    public:
        /** @brief Reserves observed raw container depth before any DOM exists; allocation failure leaves no DOM to retire. */
        explicit ManifestDocument(std::size_t observedDepth);
        ManifestDocument(const ManifestDocument &) = delete;
        ManifestDocument &operator=(const ManifestDocument &) = delete;
        /** @brief Transfers the DOM and its preadmitted scratch together; the source retains neither. */
        ManifestDocument(ManifestDocument &&other) noexcept;
        ManifestDocument &operator=(ManifestDocument &&) = delete;
        /** @brief Iteratively empties admitted completed containers using preallocated scratch.
         * @note The upstream JSON container destructor still uses its own temporary vector; checked-STL
         * allocation-failure qualification is not supported by that dependency.
         */
        ~ManifestDocument() noexcept;
        /** @brief Transfers the completed parser result into its preallocated owner; the moved-from argument becomes null.
         * @pre This owner has not previously adopted a DOM, and observed raw depth bounds every completed container.
         */
        void Adopt(Json &&document) noexcept;
        /** @brief Borrows the immutable completed DOM through synchronous schema validation. */
        [[nodiscard]] const Json &Document() const noexcept;

    private:
        Json document_;
        std::unique_ptr<Json *[]> retirement_;
        std::size_t retirementCapacity_ = 0;
    };

    [[nodiscard]] std::string ChildPath(std::string_view parent, std::string_view key);
    [[nodiscard]] std::string ElementPath(std::string_view parent, std::size_t index);
    [[nodiscard]] Error ManifestError(std::string_view path, std::string_view diagnosticCode, std::string_view reason,
                                      std::uint32_t line = 0, std::uint32_t column = 0);
    /** @brief Parses strict bounded JSON into its move-only owner, with cleanup storage reserved before parsing.
     * @return Completed immutable DOM or the unchanged typed parser diagnostic; caller budgets remain authoritative.
     */
    [[nodiscard]] Result<ManifestDocument> ParseBoundedJson(std::string_view content, const ExtensionManifestLimits &limits);
}  // namespace Horo::Extensions::ManifestParsing
