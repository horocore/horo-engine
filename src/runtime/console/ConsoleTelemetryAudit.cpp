#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/Foundation/Utf8.h"

namespace Horo::Debug {
    namespace {
        /** @brief Emits only the explicit redacted projection, without inheriting unrelated ambient identity fields. */
        class TelemetryAudit final : public ICommandAudit {
        public:
            /** @copydoc ICommandAudit::Record */
            Result<void> Record(DebugCommandId id, std::string_view text) override {
                if (id == 0 || text.size() > 65536 || !IsValidUtf8ScalarSequence(text))
                    return Result<void>::Failure(MakeCommandError(CommandError::Usage));
                const std::array fields{Telemetry::Field{"command_id", id}, Telemetry::Field{"command", std::string(text)}};
                if (!Telemetry::Runtime::EmitEvent("runtime.console", "command.authorized", Log::Level::Info, "Console command authorized",
                                                   fields, Log::LogContextSnapshot::Isolated({})))
                    return Result<void>::Failure(MakeCommandError(CommandError::MissingCapability));
                return Result<void>::Success();
            }
        };
    }  // namespace

    /** @copydoc CreateConsoleTelemetryAudit */
    std::shared_ptr<ICommandAudit> CreateConsoleTelemetryAudit() {
        return std::make_shared<TelemetryAudit>();
    }
}  // namespace Horo::Debug
