#include "Horo/Debug/ConsoleConfiguration.h"

namespace Horo::Debug {
    /** @copydoc ConsoleHistoryVariableDescriptor */
    ConsoleVariableDescriptor ConsoleHistoryVariableDescriptor() {
        ConsoleVariableDescriptor history{ConsoleHistoryLimitVariableId, "console.history.limit", std::int64_t{500}, 1.0, 10000.0};
        history.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
        history.configurationKey = SettingKey(std::string(ConsoleHistorySettingName));
        return history;
    }
}  // namespace Horo::Debug
