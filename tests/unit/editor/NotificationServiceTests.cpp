#include "../hosts/HostErrorFixture.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/NotificationService.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

TEST_CASE("NotificationService publishes events across EditorDataBus", "[editor][notifications]") {
    Horo::Editor::EditorDataBus dataBus;
    Horo::Editor::NotificationService notificationService(dataBus);

    std::vector<Horo::Editor::NotificationEvent> receivedEvents;
    const auto subscription =
        dataBus.Subscribe<Horo::Editor::NotificationEvent>([&receivedEvents](const Horo::Editor::NotificationEvent &event) {
        receivedEvents.push_back(event);
    });

    SECTION("Publish with default parameters") {
        notificationService.Publish("gameplay", Horo::Editor::NotificationSeverity::Error, "A behavior type ID is duplicated.");

        REQUIRE(receivedEvents.size() == 1);
        CHECK(receivedEvents[0].id != 0);
        CHECK(receivedEvents[0].source == "gameplay");
        CHECK(receivedEvents[0].severity == Horo::Editor::NotificationSeverity::Error);
        CHECK(receivedEvents[0].message == "A behavior type ID is duplicated.");
        CHECK(receivedEvents[0].durationSeconds == 5.0f);
        CHECK(receivedEvents[0].actions.empty());
    }

    SECTION("Publish with deduplication key namespacing and action button") {
        notificationService.Publish("gameplay", Horo::Editor::NotificationSeverity::Error, "Play session blocked due to invalid manifest",
                                    "Play session blocked", "play_blocked", 0.0f,
                                    {Horo::Editor::NotificationAction{.label = "Open logs", .actionId = "open_logs"}});

        REQUIRE(receivedEvents.size() == 1);
        CHECK(receivedEvents[0].source == "gameplay");
        CHECK(receivedEvents[0].title == "Play session blocked");
        CHECK(receivedEvents[0].deduplicationKey == "gameplay::play_blocked");
        CHECK(receivedEvents[0].durationSeconds == 0.0f);
        REQUIRE(receivedEvents[0].actions.size() == 1);
        CHECK(receivedEvents[0].actions[0].label == "Open logs");
        CHECK(receivedEvents[0].actions[0].actionId == "open_logs");
    }
}

TEST_CASE("Editor error notifications retain canonical application details beneath localized display copy",
          "[editor][notifications][errors]") {
    using namespace HostErrorFixture;
    Horo::Editor::EditorDataBus dataBus;
    Horo::Editor::NotificationService service{dataBus};
    std::vector<Horo::Editor::NotificationEvent> received;
    const auto subscription = dataBus.Subscribe<Horo::Editor::NotificationEvent>([&received](const auto &event) {
        received.push_back(event);
    });
    const auto translator = ErrorTranslator::Create(Registry(), Mappings());
    REQUIRE(translator);
    for (const auto severity : {ErrorSeverity::Info, ErrorSeverity::Warning, ErrorSeverity::Error, ErrorSeverity::Critical}) {
        Error error = Failure(severity == ErrorSeverity::Critical ? 7 : 3);
        error.severity = severity;
        const auto translated = translator->Translate(error);
        REQUIRE(translated);
        service.PublishApplicationError(*translated, "Yerelleştirilmiş ileti", "Yerelleştirilmiş başlık");
        REQUIRE(received.back().errorDetail);
        CHECK(received.back().errorDetail->Json() == translated->Json());
        CHECK(received.back().errorDetail->Failure().code.Value() == error.code.Value());
        CHECK(received.back().message == "Yerelleştirilmiş ileti");
        CHECK(received.back().source == error.domain.Value());
        CHECK(received.back().deduplicationKey == error.domain.Value() + "::" + error.code.Value());
        const auto expected = severity == ErrorSeverity::Info      ? Horo::Editor::NotificationSeverity::Info
                              : severity == ErrorSeverity::Warning ? Horo::Editor::NotificationSeverity::Warning
                                                                   : Horo::Editor::NotificationSeverity::Error;
        CHECK(received.back().severity == expected);
        CHECK(received.back().durationSeconds == (severity == ErrorSeverity::Critical ? 0.0f : 5.0f));
    }
}
