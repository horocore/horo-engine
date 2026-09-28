#pragma once

#include "Horo/Release/UpdateTransfer.h"

#include <optional>
#include <string>
#include <string_view>

namespace Horo::Release {
    /** @brief Bounded parser for transport-observed final HTTP response headers. */
    class UpdateHttpResponseHeaders final {
    public:
        explicit UpdateHttpResponseHeaders(std::string requestedUrl);

        /** @brief Consumes one cURL header line and returns the final response only at its blank separator. */
        [[nodiscard]] Result<std::optional<UpdateTransferResponse>> Feed(std::string_view line, std::string_view effectiveUrl);

    private:
        void ResetFields();
        [[nodiscard]] bool ParseField(std::string_view line);

        std::string requestedUrl_;
        std::string strongEtag_;
        std::optional<std::uint64_t> rangeStart_;
        std::optional<std::uint64_t> rangeEnd_;
        std::optional<std::uint64_t> rangeTotal_;
        std::uint64_t contentLength_{};
        std::size_t headerBytes_{};
        unsigned status_{};
        bool statusSeen_{};
        bool lengthSeen_{};
        bool rangeSeen_{};
        bool etagSeen_{};
        bool encodingSeen_{};
        bool finalSeen_{};
    };
}  // namespace Horo::Release
