#include "Horo/Foundation/Assertions.h"

#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace Horo::AssertionPolicy {
    /** @copydoc FailFast */
    [[noreturn]] void FailFast(const FailureKind kind, const std::string_view expression, const std::string_view message,
                               const std::source_location location) noexcept {
        constexpr std::size_t kBufferSize = 1024;
        constexpr std::size_t kMaximumFileBytes = 192;
        constexpr std::size_t kMaximumFunctionBytes = 160;
        constexpr std::size_t kMaximumExpressionBytes = 256;
        constexpr std::size_t kMaximumMessageBytes = 256;
        std::array<char, kBufferSize> formatted{};
        const char *const kindName = kind == FailureKind::Invariant ? "Invariant violation" : "Assertion failed";
        const char *const detail = message.empty() ? "" : message.data();
        const std::string_view file{location.file_name()};
        const std::string_view function{location.function_name()};

        static_cast<void>(std::snprintf(formatted.data(), formatted.size(), "%s at %.*s:%u in %.*s: %.*s%s%.*s", kindName,
                                        static_cast<int>(std::min(file.size(), kMaximumFileBytes)), file.data(),
                                        static_cast<unsigned int>(location.line()),
                                        static_cast<int>(std::min(function.size(), kMaximumFunctionBytes)), function.data(),
                                        static_cast<int>(std::min(expression.size(), kMaximumExpressionBytes)),
                                        expression.data() == nullptr ? "" : expression.data(), message.empty() ? "" : ": ",
                                        static_cast<int>(std::min(message.size(), kMaximumMessageBytes)), detail));
        Log::Logger::WriteEmergency("foundation.assertion", Log::Level::Critical, formatted.data());
        std::abort();
    }
}  // namespace Horo::AssertionPolicy
