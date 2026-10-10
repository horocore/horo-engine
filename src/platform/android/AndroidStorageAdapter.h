/** @file @brief Target-private Android host storage composition and native capability admission. */
#pragma once

#include "Horo/Platform/AndroidStorage.h"

#include <filesystem>

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#endif

namespace Horo::Platform::Android {
    /** @brief Composes process-owned storage and consumes real ContentResolver-opened descriptors. */
    class AndroidStorageAdapter final {
    public:
        /** @brief Composes exclusively host-owned existing roots; rejects symlinks and overlapping roots.
         * @param files Borrowed durable filesystem, outliving the result.
         * @param appPrivate Android Context.getFilesDir() path, supplied by the host.
         * @param cache Android Context.getCacheDir() path, supplied by the host.
         * @return Owner-thread storage service or typed admission failure. */
        [[nodiscard]] static Result<std::unique_ptr<AndroidStorage>> Create(DurableFileSystem &files,
                                                                            const std::filesystem::path &appPrivate,
                                                                            const std::filesystem::path &cache);
#if defined(__ANDROID__)
        /** @brief Binds the real process-owned AssetManager; the host retains its Java/global reference until service teardown. */
        [[nodiscard]] static Result<void> BindAssets(AndroidStorage &storage, AAssetManager &assets);
#endif
        /** @brief Transfers a host-opened ContentResolver descriptor into an opaque single-use capability.
         * @param storage Owner-thread service. @param descriptor Owned descriptor from
         * ContentResolver.openFileDescriptor(uri, "r") and ParcelFileDescriptor.detachFd(); consumed on every path.
         * @param lifetime Host-verified activity or persisted grant; URI stays exclusively in the host grant registry.
         * @return Opaque capability or actionable invalid/missing/access failure. The host checks URI permission
         * before opening, reports Java SecurityException via AccessFailure, and calls Revoke on grant loss.
         * @details Pipes and regular files are supported; directories/devices/sockets are rejected. No native
         * descriptor is exposed to a consumer. Blocking provider reads require host cancellation by provider policy. */
        [[nodiscard]] static Result<AndroidDocumentHandle> AdoptDocument(AndroidStorage &storage, int descriptor,
                                                                         AndroidDocumentGrantLifetime lifetime);
        /** @brief Thread-safe grant revocation; stops data publication and wakes polling reads within 100 ms.
         * @details The host drains callbacks before service destruction; descriptor close remains owner-thread work. */
        [[nodiscard]] static Result<void> Revoke(AndroidStorage &storage, AndroidDocumentHandle document);
        /** @brief Thread-safe retirement of activity grants; host calls this before admitting replacement activity grants. */
        [[nodiscard]] static Result<void> RetireActivityGrants(AndroidStorage &storage);
        /** @brief Maps host ContentResolver permission/missing-source exceptions without leaking URI text. */
        [[nodiscard]] static Error AccessFailure(bool revoked, bool missing);
    };
}  // namespace Horo::Platform::Android
