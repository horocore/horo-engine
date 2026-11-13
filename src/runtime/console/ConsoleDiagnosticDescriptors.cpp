#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleConfiguration.h"
#include "Horo/Debug/ConsoleProduct.h"

#include <array>
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
    }  // namespace

    /** @copydoc DiagnosticDescriptors */
    std::span<const DebugCommandDescriptor> DiagnosticDescriptors() noexcept {
        static const std::array descriptors{DebugCommandDescriptor{LogLevelCommandId, "log.level",
                                                                   "Change a category's session log threshold",
                                                                   "log.level <category> <level>", LogArguments,
                                                                   CommandPermission::Developer, CommandThreadPolicy::OwnerThreadNextFrame,
                                                                   CommandAvailability::NonShipping},
                                            ConsoleMetricDescriptor(),
                                            DebugCommandDescriptor{ProfileCaptureCommandId, "sys.profile_capture",
                                                                   "Control a bounded profiler capture",
                                                                   "sys.profile_capture <start|stop> [--seconds value]", CaptureArguments,
                                                                   CommandPermission::Developer, CommandThreadPolicy::WorkerJob,
                                                                   CommandAvailability::NonShipping}};
        return descriptors;
    }

    /** @copydoc AppendDiagnosticVariables */
    void AppendDiagnosticVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
        descriptors.push_back(ConsoleHistoryVariableDescriptor());
#if !defined(HORO_PROFILE_SERVER)
        descriptors.push_back(ConsoleLogOverlayVariableDescriptor());
        descriptors.push_back(ConsoleJobsOverlayVariableDescriptor());
        descriptors.push_back(ConsoleNetworkOverlayVariableDescriptor());

        /** @brief Stable catalog identity for the remaining unbound session controls. */
        struct VariableName final {
            std::uint64_t id;
            std::string_view name;
        };

        constexpr VariableName remaining[]{{0xDB620004, "overlay.frame.visible"},  {0xDB620005, "overlay.cpu.visible"},
                                           {0xDB620006, "overlay.memory.visible"}, {0xDB620008, "overlay.renderer.visible"},
                                           {0xDB620009, "overlay.assets.visible"}, {0xDB62000A, "overlay.input.visible"}};
        for (const auto &variable : remaining)
            descriptors.push_back({variable.id, std::string(variable.name), false});
#endif
        descriptors.push_back({0xDB62000C, "asset.streaming.debug", false});
    }
}  // namespace Horo::Debug::Detail
