#include "Horo/Editor/NotificationService.h"

#include <atomic>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::uint64_t NextNotificationId() noexcept {
            static std::atomic<std::uint64_t> counter{1};
            return counter.fetch_add(1);
        }
    }  // namespace

    void NotificationService::Publish(std::string_view source, NotificationSeverity severity, std::string message, std::string title,
                                      std::string deduplicationKey, float durationSeconds, std::vector<NotificationAction> actions) const {
        Publish(NotificationEvent{.id = NextNotificationId(),
                                  .source = std::string(source),
                                  .severity = severity,
                                  .title = std::move(title),
                                  .message = std::move(message),
                                  .deduplicationKey = std::move(deduplicationKey),
                                  .durationSeconds = durationSeconds,
                                  .dismissible = true,
                                  .actions = std::move(actions)});
    }

    void NotificationService::Publish(NotificationEvent event) const {
        if (events_ == nullptr)
            return;

        if (event.id == 0)
            event.id = NextNotificationId();

        if (!event.deduplicationKey.empty() && event.deduplicationKey.find("::") == std::string::npos) {
            event.deduplicationKey = event.source + "::" + event.deduplicationKey;
        }

        events_->Publish(event);
    }

    /** @copydoc NotificationService::PublishApplicationError */
    void NotificationService::PublishApplicationError(const Hosts::TranslatedError &error, std::string localizedMessage,
                                                      std::string localizedTitle) const {
        NotificationSeverity severity = NotificationSeverity::Error;
        if (error.Failure().severity == ErrorSeverity::Info)
            severity = NotificationSeverity::Info;
        else if (error.Failure().severity == ErrorSeverity::Warning)
            severity = NotificationSeverity::Warning;
        Publish(NotificationEvent{.source = error.Failure().domain.Value(),
                                  .severity = severity,
                                  .title = std::move(localizedTitle),
                                  .message = std::move(localizedMessage),
                                  .deduplicationKey = error.Failure().code.Value(),
                                  .durationSeconds = error.Failure().severity == ErrorSeverity::Critical ? 0.0f : 5.0f,
                                  .errorDetail = error});
    }
}  // namespace Horo::Editor
