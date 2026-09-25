#include "Horo/Foundation/Assertions.h"

#include "AssertionFormatting.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <cstdlib>

namespace Horo::AssertionPolicy {
    /** @copydoc FailFast */
    [[noreturn]] void FailFast(const FailureKind kind, const std::string_view expression, const std::string_view message,
                               const std::source_location location) noexcept {
        const auto formatted =
            Detail::FormatFailure(kind, expression, message, location.file_name(), location.line(), location.function_name());
        Log::Logger::WriteEmergency("foundation.assertion", Log::Level::Critical, formatted.data());
        std::abort();
    }
}  // namespace Horo::AssertionPolicy
