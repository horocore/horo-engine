#include "AndroidGameActivityJni.h"

#include <exception>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <new>
#include <utility>

namespace Horo::Platform::Android {
    /** @copydoc GameActivityJniAttachment::GameActivityJniAttachment */
    GameActivityJniAttachment::GameActivityJniAttachment(JavaVM *const vm) noexcept : vm_(vm) {
        if (!vm_)
            return;
        void *environment{};
        const auto status = vm_->GetEnv(&environment, JNI_VERSION_1_6);
        if (status == JNI_OK) {
            environment_ = static_cast<JNIEnv *>(environment);
        } else if (status == JNI_EDETACHED) {
            attached_ = vm_->AttachCurrentThread(&environment_, nullptr) == JNI_OK;
            if (!attached_)
                environment_ = nullptr;
        }
    }

    /** @copydoc GameActivityJniAttachment::~GameActivityJniAttachment */
    GameActivityJniAttachment::~GameActivityJniAttachment() {
        if (attached_)
            vm_->DetachCurrentThread();
    }

    namespace {
        /** @brief Translates an exception raised by this native operation to a typed failure. */
        bool ClearException(JNIEnv &env) noexcept {
            if (!env.ExceptionCheck())
                return false;
            env.ExceptionClear();
            return true;
        }

        /** @brief Resolves the app-scoped Context through the real Activity, with no process-global cached reference. */
        jobject FindApplicationContext(JNIEnv &env, jobject activity) noexcept {
            const auto type = env.GetObjectClass(activity);
            if (ClearException(env) || !type)
                return nullptr;
            const auto method = env.GetMethodID(type, "getApplicationContext", "()Landroid/content/Context;");
            env.DeleteLocalRef(type);
            if (ClearException(env) || !method)
                return nullptr;
            const auto context = env.CallObjectMethod(activity, method);
            return ClearException(env) ? nullptr : context;
        }

        /** @brief Deletes a global reference through the releasing thread's own JNI environment. */
        struct DeleteGlobalReference final {
            JavaVM *vm;

            void operator()(jobject reference) const noexcept {
                const GameActivityJniAttachment attachment{vm};
                auto *env = attachment.Environment();
                if (!env)
                    std::terminate();  // The host's VM lifetime must cover every outstanding reference.
                env->DeleteGlobalRef(reference);
            }
        };

        /** @brief Promotes the borrow, deleting the global even if shared-owner allocation fails. */
        GameActivityGlobalRef PromoteReference(JNIEnv &env, JavaVM *vm, jobject reference) {
            const auto global = env.NewGlobalRef(reference);
            if (ClearException(env) || !global) {
                env.DeleteGlobalRef(global);
                return {};
            }
            return GameActivityGlobalRef{global, DeleteGlobalReference{vm}};
        }
    }  // namespace

    /** @copydoc AcquireGameActivityJniApplication */
    std::variant<GameActivityJniApplication, GameActivityJniFailure> AcquireGameActivityJniApplication(const android_app &app) {
        using enum GameActivityJniFailure;
        if (app.destroyRequested || !app.activity || !app.activity->javaGameActivity || !app.activity->vm)
            return ActivityUnavailable;
        auto *vm = app.activity->vm;
        const auto activity = app.activity->javaGameActivity;
        const GameActivityJniAttachment attachment{vm};
        auto *env = attachment.Environment();
        if (!env || env->ExceptionCheck())
            return EnvironmentUnavailable;

        const auto deleteLocal = [env](jobject reference) noexcept {
            env->DeleteLocalRef(reference);
        };
        const std::unique_ptr<std::remove_pointer_t<jobject>, decltype(deleteLocal)> context{FindApplicationContext(*env, activity),
                                                                                             deleteLocal};
        if (!context)
            return ApplicationContextUnavailable;

        try {
            auto contextOwner = PromoteReference(*env, vm, context.get());
            if (!contextOwner)
                return ReferenceUnavailable;
            auto activityOwner = PromoteReference(*env, vm, activity);
            if (!activityOwner)
                return ReferenceUnavailable;
            return GameActivityJniApplication{vm, std::move(contextOwner), std::move(activityOwner)};
        } catch (const std::bad_alloc &) {
            return ReferenceUnavailable;
        }
    }
}  // namespace Horo::Platform::Android
