#pragma once

/** @file AndroidGameActivityJni.h
 * @brief Private GameActivity JNI attachment and retained application references.
 */

#include <jni.h>
#include <memory>
#include <type_traits>
#include <variant>

struct android_app;

namespace Horo::Platform::Android {
    /** @brief Borrows the current JNI environment or attaches for the lifetime of this owner. */
    class GameActivityJniAttachment final {
    public:
        /** @brief Acquires a JNI environment for the calling thread.
         * @param vm Live process Java VM, or null when unavailable.
         */
        explicit GameActivityJniAttachment(JavaVM *vm) noexcept;
        /** @brief Detaches on the creating thread only when this owner performed the attachment. */
        ~GameActivityJniAttachment();
        GameActivityJniAttachment(const GameActivityJniAttachment &) = delete;
        GameActivityJniAttachment &operator=(const GameActivityJniAttachment &) = delete;

        /** @brief Returns the current thread's borrowed environment or null when unavailable. */
        [[nodiscard]] JNIEnv *Environment() const noexcept {
            return environment_;
        }

    private:
        JavaVM *vm_{};
        JNIEnv *environment_{};
        bool attached_{};
    };

    /** @brief Owns a JNI global reference while keeping the underlying Java object opaque. */
    using GameActivityGlobalRef = std::shared_ptr<std::remove_pointer_t<jobject>>;

    /** @brief Acquired process application Context and exact Activity from a live GameActivity.
     * @details The two owners allow application composition to retain the Context until loader unload
     * and retire the Activity with its native instance.
     */
    struct GameActivityJniApplication final {
        JavaVM *vm{};
        GameActivityGlobalRef applicationContext;
        GameActivityGlobalRef activity;
    };

    /** @brief Failure reason for the cold GameActivity JNI acquisition. */
    enum class GameActivityJniFailure {
        ActivityUnavailable,
        EnvironmentUnavailable,
        ApplicationContextUnavailable,
        ReferenceUnavailable
    };

    /** @brief Acquires independently owned JNI globals from the current GameActivity.
     * @param app Live GameActivity glue instance, read on its native application thread.
     * @return Retained application values, or a typed failure with no partial reference ownership.
     * @pre The host serializes acquisition with Activity destruction.
     */
    [[nodiscard]] std::variant<GameActivityJniApplication, GameActivityJniFailure> AcquireGameActivityJniApplication(
        const android_app &app);
}  // namespace Horo::Platform::Android
