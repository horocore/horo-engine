#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

#include <iterator>

#if !defined(HORO_DEBUG_COMMANDS_ENABLED) || defined(HORO_PROFILE_SHIPPING)
#error "Diagnostic console translation units must be omitted from Shipping builds."
#endif

namespace Horo::Debug::Detail {
    namespace {
        constexpr std::string_view Levels[]{"trace", "debug", "info", "warn", "error", "critical", "off"};
        constexpr std::string_view CaptureActions[]{"start", "stop"};
        constexpr DebugArgumentDescriptor
            LogArguments[]{{"category", "Exact log category", DebugArgumentType::String},
                           {"level", "Session log severity", DebugArgumentType::Enum, true, {}, false, false, Levels}};
        constexpr DebugArgumentDescriptor
            CaptureArguments[]{{"action", "Bounded capture control", DebugArgumentType::Enum, true, {}, false, false, CaptureActions},
                               {"seconds", "Bounded capture duration", DebugArgumentType::Int32, false, "20", false, true, {}, 1.0, 120.0}};
        constexpr DebugCommandDescriptor Descriptors[]{
            {LogLevelCommandId, "log.level", "Change a category's session log threshold", "log.level <category> <level>", LogArguments,
             CommandPermission::Developer, CommandThreadPolicy::OwnerThreadNextFrame, CommandAvailability::NonShipping},
            {MetricsCommandId,
             "sys.metrics",
             "List authoritative metric channels",
             "sys.metrics",
             {},
             CommandPermission::Developer,
             CommandThreadPolicy::OwnerThreadNextFrame,
             CommandAvailability::NonShipping},
            {ProfileCaptureCommandId, "sys.profile_capture", "Control a bounded profiler capture",
             "sys.profile_capture <start|stop> [--seconds value]", CaptureArguments, CommandPermission::Developer,
             CommandThreadPolicy::WorkerJob, CommandAvailability::NonShipping},
        };
    }  // namespace

    /** @copydoc DiagnosticDescriptors */
    std::span<const DebugCommandDescriptor> DiagnosticDescriptors() noexcept {
        return Descriptors;
    }

    /** @copydoc AppendDiagnosticVariables */
    void AppendDiagnosticVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
        ConsoleVariableDescriptor history{ConsoleHistoryLimitVariableId, "console.history.limit", std::int64_t{500}, 1.0, 10000.0};
        history.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
        descriptors.push_back(std::move(history));
#if !defined(HORO_PROFILE_SERVER)
        constexpr std::string_view names[]{"overlay.logs.visible",   "overlay.frame.visible", "overlay.cpu.visible",
                                           "overlay.memory.visible", "overlay.jobs.visible",  "overlay.renderer.visible",
                                           "overlay.assets.visible", "overlay.input.visible", "overlay.network.visible"};
        for (std::size_t i = 0; i < std::size(names); ++i)
            descriptors.push_back({0xDB620003 + i, std::string(names[i]), false});
#endif
        descriptors.push_back({0xDB62000C, "asset.streaming.debug", false});
    }
}  // namespace Horo::Debug::Detail
