#include "UpdateHttpResponse.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumHeaderBytes = 64U * 1024U;
        constexpr std::size_t MaximumLineBytes = 8U * 1024U;

        /** @brief Parses one unsigned decimal value without whitespace or trailing data. */
        [[nodiscard]] bool ParseDecimal(const std::string_view text, std::uint64_t &value) {
            if (text.empty())
                return false;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        /** @brief Compares ASCII header names and identity encoding without locale dependence. */
        [[nodiscard]] bool EqualAsciiIgnoreCase(const std::string_view left, const std::string_view right) {
            if (left.size() != right.size())
                return false;
            for (std::size_t index = 0U; index < left.size(); ++index) {
                const auto lower = [](const unsigned char character) {
                    return character >= 'A' && character <= 'Z' ? static_cast<unsigned char>(character - 'A' + 'a') : character;
                };
                if (lower(static_cast<unsigned char>(left[index])) != lower(static_cast<unsigned char>(right[index])))
                    return false;
            }
            return true;
        }

        /** @brief Removes HTTP optional whitespace around a field value. */
        [[nodiscard]] std::string_view Trim(const std::string_view value) {
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string_view::npos)
                return {};
            const auto last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1U);
        }

        /** @brief Parses an exact bounded byte-range declaration; wildcard forms are not resumable. */
        [[nodiscard]] bool ParseContentRange(const std::string_view value, std::uint64_t &start, std::uint64_t &end, std::uint64_t &total) {
            if (!value.starts_with("bytes "))
                return false;
            const auto dash = value.find('-', 6U);
            const auto slash = value.find('/', dash == std::string_view::npos ? 6U : dash + 1U);
            return dash != std::string_view::npos && slash != std::string_view::npos && ParseDecimal(value.substr(6U, dash - 6U), start) &&
                   ParseDecimal(value.substr(dash + 1U, slash - dash - 1U), end) && ParseDecimal(value.substr(slash + 1U), total) &&
                   start <= end && end < total;
        }

        /** @brief Parses a final or interim HTTP status line. */
        [[nodiscard]] bool ParseStatus(const std::string_view line, unsigned &status) {
            if (!line.starts_with("HTTP/"))
                return false;
            const auto space = line.find(' ');
            if (space == std::string_view::npos || space + 4U > line.size())
                return false;
            std::uint64_t parsed{};
            if (!ParseDecimal(line.substr(space + 1U, 3U), parsed) || parsed < 100U || parsed > 599U ||
                (space + 4U < line.size() && line[space + 4U] != ' '))
                return false;
            status = static_cast<unsigned>(parsed);
            return true;
        }

        /** @brief Constructs one typed invalid-response failure. */
        [[nodiscard]] Result<std::optional<UpdateTransferResponse>> Invalid() {
            return Result<std::optional<UpdateTransferResponse>>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        }
    }  // namespace

    UpdateHttpResponseHeaders::UpdateHttpResponseHeaders(std::string requestedUrl) : requestedUrl_(std::move(requestedUrl)) {}

    void UpdateHttpResponseHeaders::ResetFields() {
        strongEtag_.clear();
        rangeStart_.reset();
        rangeEnd_.reset();
        rangeTotal_.reset();
        contentLength_ = 0U;
        lengthSeen_ = false;
        rangeSeen_ = false;
        etagSeen_ = false;
        encodingSeen_ = false;
    }

    bool UpdateHttpResponseHeaders::ParseField(const std::string_view line) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U)
            return false;
        const auto name = line.substr(0U, colon);
        const auto value = Trim(line.substr(colon + 1U));
        if (EqualAsciiIgnoreCase(name, "content-length")) {
            lengthSeen_ = !lengthSeen_ && ParseDecimal(value, contentLength_);
            return lengthSeen_;
        }
        if (EqualAsciiIgnoreCase(name, "content-range")) {
            if (rangeSeen_)
                return false;
            std::uint64_t start{};
            std::uint64_t end{};
            std::uint64_t total{};
            if (!ParseContentRange(value, start, end, total))
                return false;
            rangeStart_ = start;
            rangeEnd_ = end;
            rangeTotal_ = total;
            rangeSeen_ = true;
            return true;
        }
        if (EqualAsciiIgnoreCase(name, "etag")) {
            if (etagSeen_)
                return false;
            strongEtag_ = value;
            etagSeen_ = true;
            return true;
        }
        if (EqualAsciiIgnoreCase(name, "content-encoding")) {
            if (encodingSeen_ || !EqualAsciiIgnoreCase(value, "identity"))
                return false;
            encodingSeen_ = true;
            return true;
        }
        return !EqualAsciiIgnoreCase(name, "transfer-encoding");
    }

    Result<std::optional<UpdateTransferResponse>> UpdateHttpResponseHeaders::Feed(std::string_view line,
                                                                                  const std::string_view effectiveUrl) {
        if (finalSeen_ || line.size() > MaximumLineBytes || line.size() > MaximumHeaderBytes - headerBytes_)
            return Invalid();
        headerBytes_ += line.size();
        if (line.ends_with("\r\n"))
            line.remove_suffix(2U);
        else if (line.ends_with('\n'))
            line.remove_suffix(1U);
        else
            return Invalid();
        if (line.starts_with("HTTP/")) {
            if (statusSeen_ || !ParseStatus(line, status_))
                return Invalid();
            ResetFields();
            statusSeen_ = true;
            return Result<std::optional<UpdateTransferResponse>>::Success(std::nullopt);
        }
        if (!statusSeen_)
            return Invalid();
        if (!line.empty())
            return ParseField(line) ? Result<std::optional<UpdateTransferResponse>>::Success(std::nullopt) : Invalid();
        if (status_ < 200U) {
            statusSeen_ = false;
            ResetFields();
            return Result<std::optional<UpdateTransferResponse>>::Success(std::nullopt);
        }
        if (!lengthSeen_ || effectiveUrl.empty())
            return Invalid();
        finalSeen_ = true;
        return Result<std::optional<UpdateTransferResponse>>::Success(UpdateTransferResponse{.status = status_,
                                                                                             .requestedUrl = requestedUrl_,
                                                                                             .effectiveUrl = std::string{effectiveUrl},
                                                                                             .strongEtag = strongEtag_,
                                                                                             .contentLength = contentLength_,
                                                                                             .rangeStart = rangeStart_,
                                                                                             .rangeEnd = rangeEnd_,
                                                                                             .rangeTotal = rangeTotal_});
    }
}  // namespace Horo::Release
