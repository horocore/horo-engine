#include "ExternalAssetImporter.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <new>
#include <stdexcept>
#include <thread>

namespace Horo::Extensions {
    ExtensionModuleLifetime::~ExtensionModuleLifetime() {
        (void)UnloadNow();
        if (teardownFailed) {
            if (const auto owner = retirement.lock())
                owner->RequireRestart();
        }
        if (teardownFailed && (code->library || !code->dependencies.empty())) {
            LOG_WARN("extensions.importer", "Restart required: failed teardown retains native dependency closure for %s.",
                     moduleId.c_str());
            (void)code.release();
        }
    }

    /** @copydoc ExtensionModuleLifetime::UnloadNow */
    bool ExtensionModuleLifetime::UnloadNow() noexcept {
        if (!loaded)
            return !teardownFailed;
        loaded = false;
        if (std::this_thread::get_id() != ownerThread) {
            teardownFailed = true;
            LOG_WARN("extensions.importer", "Restart required: native teardown attempted off the module owner lane.");
            return false;
        }
        if (unload != nullptr && moduleApi.moduleContext != nullptr) {
            try {
                unload(&moduleApi);
            } catch (const std::runtime_error &exception) {
                LOG_WARN("extensions.importer", "Runtime error during module unload: %s", exception.what());
                teardownFailed = true;
                return false;
            } catch (const std::logic_error &exception) {
                LOG_WARN("extensions.importer", "Logic error during module unload: %s", exception.what());
                teardownFailed = true;
                return false;
            } catch (const std::bad_alloc &exception) {
                LOG_WARN("extensions.importer", "Bad alloc during module unload: %s", exception.what());
                teardownFailed = true;
                return false;
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "Exception during module unload: %s", exception.what());
                teardownFailed = true;
                return false;
            } catch (...) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "Unknown exception during module unload.");
                teardownFailed = true;
                return false;
            }
        }
        return true;
    }

}  // namespace Horo::Extensions
