#include "Horo/Platform/NativeCredentialBackend.h"
#include "Horo/Security/SecurityErrors.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <libsecret/secret.h>
#include <mutex>
#include <string>
#include <thread>

namespace Horo::Platform {
    namespace {
        constexpr auto Deadline = std::chrono::seconds{3};
        constexpr std::size_t MaximumBytes = 2560;
        constexpr char ContentType[] = "application/octet-stream";
        constexpr auto Schema = [] {
            SecretSchema schema{};
            schema.name = "dev.horo.engine.Credential";
            schema.flags = SECRET_SCHEMA_NONE;
            schema.attributes[0] = {"reference", SECRET_SCHEMA_ATTRIBUTE_STRING};
            return schema;
        }();

        struct ReleaseObject {
            void operator()(gpointer object) const noexcept {
                g_object_unref(object);
            }
        };

        template <typename T> using Object = std::unique_ptr<T, ReleaseObject>;

        /** @brief Keeps native requests bounded even when the desktop service initiates interaction. */
        class Request final {
        public:
            Request()
                : timer_([this](const std::stop_token stop) {
                      std::unique_lock lock{mutex_};
                      condition_.wait_for(lock, stop, Deadline, [] {
                          return false;
                      });
                      if (!stop.stop_requested())
                          g_cancellable_cancel(cancellable_.get());
                  }) {}

            [[nodiscard]] GCancellable *Token() const noexcept {
                return cancellable_.get();
            }

        private:
            Object<GCancellable> cancellable_{g_cancellable_new()};
            std::mutex mutex_;
            std::condition_variable_any condition_;
            // Joins before releasing any state the cancellation thread can observe.
            std::jthread timer_;
        };

        /** @brief Releases the returned list and every copied proxy reference. */
        void FreeItems(GList *items) noexcept {
            g_list_free_full(items, &g_object_unref);
        }

        using Items = std::unique_ptr<GList, decltype(&FreeItems)>;
        using Value = std::unique_ptr<SecretValue, decltype(&secret_value_unref)>;
        using Attributes = std::unique_ptr<GHashTable, decltype(&g_hash_table_unref)>;

        /** @brief Opens the unlocked default collection and rejects a session-only alias. */
        Object<SecretCollection> OpenCollection(Request &request) {
            const Object<SecretService> service{secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, request.Token(), nullptr)};
            if (!service)
                return {};
            Object<SecretCollection> collection{
                secret_collection_for_alias_sync(service.get(), "default", SECRET_COLLECTION_NONE, request.Token(), nullptr)};
            if (!collection || secret_collection_get_locked(collection.get()))
                return {};
            GError *error{};
            const Object<SecretCollection> session{
                secret_collection_for_alias_sync(service.get(), "session", SECRET_COLLECTION_NONE, request.Token(), &error)};
            if (error) {
                g_error_free(error);
                return {};
            }
            if (session && g_str_equal(g_dbus_proxy_get_object_path(G_DBUS_PROXY(session.get())),
                                       g_dbus_proxy_get_object_path(G_DBUS_PROXY(collection.get()))))
                return {};
            return collection;
        }

        /** @brief Restricts every native operation to the product schema and one exact identity. */
        Attributes Match(const CredentialReference &reference) {
            const std::string account{reference.Value()};
            return {secret_attributes_build(&Schema, "reference", account.c_str(), nullptr), &g_hash_table_unref};
        }

        /** @brief Searches the selected collection without unlocking it or reading any values. */
        Items Find(SecretCollection &collection, GHashTable &attributes, Request &request, GError **error) {
            return {secret_collection_search_sync(&collection, &Schema, &attributes, SECRET_SEARCH_ALL, request.Token(), error),
                    &FreeItems};
        }

        /** @brief Maps system failures without returning native error text. */
        Error Unavailable() {
            return MakeError(SecurityErrors::CredentialBackendUnavailable);
        }

        /** @brief Stores only in the current desktop user's durable Secret Service collection. */
        class LinuxCredentialBackend final : public Security::CredentialBackend {
        public:
            /** @copydoc Security::CredentialBackend::Available */
            bool Available() const noexcept override {
                try {
                    Request request;
                    return static_cast<bool>(OpenCollection(request));
                } catch (const std::exception &) {
                    return false;
                }
            }

            /** @copydoc Security::CredentialBackend::Put */
            Result<void> Put(const CredentialReference &reference, Security::SecureBytes secret) override {
                if (reference.Value().empty() || secret.Empty() || secret.View().size() > MaximumBytes)
                    return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                Request request;
                const auto collection = OpenCollection(request);
                if (!collection)
                    return Result<void>::Failure(Unavailable());
                const auto attributes = Match(reference);
                const auto bytes = secret.View();
                const Value value{secret_value_new(reinterpret_cast<const gchar *>(bytes.data()), static_cast<gssize>(bytes.size()),
                                                   ContentType),
                                  &secret_value_unref};
                if (!attributes || !value)
                    return Result<void>::Failure(Unavailable());
                const Object<SecretItem> stored{secret_item_create_sync(collection.get(), &Schema, attributes.get(), "Horo Engine",
                                                                        value.get(), SECRET_ITEM_CREATE_REPLACE, request.Token(), nullptr)};
                return stored ? Result<void>::Success() : Result<void>::Failure(Unavailable());
            }

            /** @copydoc Security::CredentialBackend::Resolve */
            Result<Security::SecureBytes> Resolve(const CredentialReference &reference) override {
                using Outcome = Result<Security::SecureBytes>;
                if (reference.Value().empty())
                    return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
                Request request;
                const auto collection = OpenCollection(request);
                const auto attributes = Match(reference);
                if (!collection || !attributes)
                    return Outcome::Failure(Unavailable());
                GError *error{};
                const auto matches = Find(*collection, *attributes, request, &error);
                if (error) {
                    g_error_free(error);
                    return Outcome::Failure(Unavailable());
                }
                if (!matches)
                    return Outcome::Failure(MakeError(SecurityErrors::CredentialNotFound));
                auto *item = SECRET_ITEM(matches->data);
                if (matches->next || secret_item_get_locked(item) || !secret_item_load_secret_sync(item, request.Token(), nullptr))
                    return Outcome::Failure(Unavailable());
                const Value value{secret_item_get_secret(item), &secret_value_unref};
                if (!value)
                    return Outcome::Failure(Unavailable());
                gsize size{};
                // Some services label returned values as text. The supplied length is authoritative for binary credentials.
                const auto *bytes = secret_value_get(value.get(), &size);
                if (!bytes || size == 0 || size > MaximumBytes)
                    return Outcome::Failure(Unavailable());
                return Outcome::Success(Security::SecureBytes{{reinterpret_cast<const std::byte *>(bytes), size}});
            }

            /** @copydoc Security::CredentialBackend::Remove */
            Result<void> Remove(const CredentialReference &reference) noexcept override {
                try {
                    if (reference.Value().empty())
                        return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                    Request request;
                    const auto collection = OpenCollection(request);
                    const auto attributes = Match(reference);
                    if (!collection || !attributes)
                        return Result<void>::Failure(Unavailable());
                    GError *error{};
                    const auto matches = Find(*collection, *attributes, request, &error);
                    if (error) {
                        g_error_free(error);
                        return Result<void>::Failure(Unavailable());
                    }
                    for (const auto *entry = matches.get(); entry; entry = entry->next) {
                        auto *item = SECRET_ITEM(entry->data);
                        if (secret_item_get_locked(item) || !secret_item_delete_sync(item, request.Token(), nullptr))
                            return Result<void>::Failure(Unavailable());
                    }
                    return Result<void>::Success();
                } catch (const std::exception &) {
                    return Result<void>::Failure(Unavailable());
                }
            }
        };
    }  // namespace

    /** @copydoc CreateNativeCredentialBackend */
    std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend() {
        return std::make_shared<LinuxCredentialBackend>();
    }
}  // namespace Horo::Platform
