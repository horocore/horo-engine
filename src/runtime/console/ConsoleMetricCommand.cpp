#include "ConsoleMetricCommandInternal.h"
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <charconv>

namespace Horo::Debug::Internal {
    namespace {
        /** @brief Maps availability without exposing descriptor text or record payloads. */
        std::string_view AvailabilityName(Telemetry::MetricAvailabilityState state) noexcept {
            using State = Telemetry::MetricAvailabilityState;
            switch (state) {
                case State::Available:
                    return "available";
                case State::TemporarilyUnavailable:
                    return "temporarily_unavailable";
                case State::UnsupportedByPlatform:
                    return "unsupported_by_platform";
                case State::UnsupportedByBackend:
                    return "unsupported_by_backend";
                case State::DisabledByProfile:
                    return "disabled_by_profile";
                case State::PermissionDenied:
                    return "permission_denied";
                case State::SamplerFailed:
                    return "sampler_failed";
            }
            return "unknown";
        }

        /** @brief Reads the fixed-capacity process snapshot once per explicit owner-thread request. */
        class MetricCommand final : public ICommandHandler {
        public:
            explicit MetricCommand(std::size_t limit) : limit_(limit), owner_(std::this_thread::get_id()) {}

            /** @copydoc ICommandHandler::Execute */
            Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>,
                                               const CancellationToken &cancellation, const JobExecutionContext *) override {
                if (std::this_thread::get_id() != owner_)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::WrongThread));
                if (cancellation.IsCancellationRequested())
                    return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                const auto snapshot = Telemetry::Runtime::GetDiagnosticSnapshot();
                if (snapshot.availabilityCount > snapshot.availability.size())
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::LimitExceeded));
                std::string text;
                text.reserve(limit_);
                const auto append = [&](std::string_view value) {
                    text.append(value.substr(0, limit_ - text.size()));
                };
                const auto number = [&](std::uint64_t value) {
                    std::array<char, 32> buffer{};
                    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                    if (error == std::errc{})
                        append({buffer.data(), static_cast<std::size_t>(end - buffer.data())});
                };
                append("telemetry generation=");
                number(snapshot.runtimeGeneration);
                append(snapshot.runtimeEnabled ? " enabled=true revision=" : " enabled=false revision=");
                number(snapshot.availabilityRevision);
                append("\n");
                for (std::size_t i = 0; i < snapshot.availabilityCount && text.size() < limit_; ++i) {
                    const auto &row = snapshot.availability[i];
                    append("instrument=");
                    number(row.instrumentId);
                    append(" state=");
                    append(AvailabilityName(row.state));
                    append("\n");
                }
                return Result<DebugCommandOutput>::Success({std::move(text)});
            }

        private:
            std::size_t limit_;
            std::thread::id owner_;
        };
    }  // namespace

    /** @copydoc BindMetricCommand */
    Result<std::vector<CommandBinding>> BindMetricCommand(std::shared_ptr<const CommandRegistry> registry) {
        if (!registry || !registry->Profile() || registry->Profile() == ProductProfile::Shipping)
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto descriptor = registry->Resolve(MetricsCommandId, {15, true, true});
        if (descriptor.HasError())
            return Result<std::vector<CommandBinding>>::Success({});
        const auto &value = descriptor.Value();
        const auto expected = ConsoleMetricDescriptor();
        if (value.name != expected.name || value.permissions != expected.permissions || value.threadPolicy != expected.threadPolicy ||
            value.availability != expected.availability || value.flags != expected.flags || !value.arguments.empty())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<std::vector<CommandBinding>>::Success(
            {{MetricsCommandId, std::make_shared<MetricCommand>(registry->Limits().outputBytes)}});
    }
}  // namespace Horo::Debug::Internal
