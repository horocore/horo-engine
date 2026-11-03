#include "EditorPlaySaveFailurePublication.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using Application::Internal::PreservePlaySaveFailure;

    Error OriginalFailure() {
        auto error = MakeError(SaveErrors::RestoreParticipantInvalid, std::string(512, 'e'));
        error.diagnostics.emplace_back(DiagnosticCode{std::string(128, 'd')}, DiagnosticSeverity::Note, std::string(512, 'm'),
                                       SourceLocation{std::string(256, 'p'), 17, 4});
        error.diagnostics.back().path = std::string(256, 'q');
        return WithCause(std::move(error), MakeError(SaveErrors::ReferenceResolutionInvalid, std::string(256, 'c')));
    }

    SaveOperationController Producer() {
        auto created = CreateSaveOperation({.operation = 41, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 1});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    void SameFailure(const Error &actual, const Error &expected) {
        CHECK(actual.code.Value() == expected.code.Value());
        CHECK(actual.domain.Value() == expected.domain.Value());
        CHECK(actual.severity == expected.severity);
        CHECK(actual.message == expected.message);
        REQUIRE(actual.diagnostics.size() == expected.diagnostics.size());
        REQUIRE(actual.diagnostics.size() == 1);
        CHECK(actual.diagnostics.front().code.Value() == expected.diagnostics.front().code.Value());
        CHECK(actual.diagnostics.front().severity == expected.diagnostics.front().severity);
        CHECK(actual.diagnostics.front().message == expected.diagnostics.front().message);
        CHECK(actual.diagnostics.front().location.source == expected.diagnostics.front().location.source);
        CHECK(actual.diagnostics.front().location.line == expected.diagnostics.front().location.line);
        CHECK(actual.diagnostics.front().location.column == expected.diagnostics.front().location.column);
        CHECK(actual.diagnostics.front().path == expected.diagnostics.front().path);
        REQUIRE(actual.cause.Get());
        REQUIRE(expected.cause.Get());
        CHECK(actual.cause.Get() == expected.cause.Get());
        CHECK(actual.cause.Get()->code.Value() == expected.cause.Get()->code.Value());
        CHECK(actual.cause.Get()->message == expected.cause.Get()->message);
    }

    std::size_t ErrorCopyAllocations(const Error &error) {
        std::optional<Error> copied;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            copied.emplace(error);
            measured = measurement.Snapshot();
        }
        REQUIRE(measured.requests > 0);
        return measured.requests;
    }
}  // namespace

TEST_CASE("Play first-error publication preserves its returned error on the normal path", "[save][operation][editor][failure]") {
    auto producer = Producer();
    const auto expected = OriginalFailure();
    auto returned = PreservePlaySaveFailure(producer, Result<void>::Failure(expected));
    REQUIRE(returned.HasError());
    SameFailure(returned.ErrorValue(), expected);
    const auto terminal = producer.Handle().Snapshot();
    REQUIRE(terminal);
    REQUIRE(terminal->state == SaveOperationState::Failed);
    REQUIRE(terminal->terminalError);
    SameFailure(*terminal->terminalError, expected);
    CHECK(producer.RequestShutdownCancellation() == SaveCancellationRequestResult::AlreadyTerminal);
}

TEST_CASE("Every actual Play first-error copy allocation failure publishes the owned original before rethrow",
          "[save][operation][editor][failure][allocation]") {
    const auto expected = OriginalFailure();
    const auto allocations = ErrorCopyAllocations(expected);
    for (std::size_t index = 0; index < allocations; ++index) {
        INFO(index);
        std::size_t callbacks{};
        auto producer = Producer();
        REQUIRE(producer.Handle()
                    .OnCompletion([&callbacks](const auto &) {
            ++callbacks;
        }).HasValue());
        auto original = Result<void>::Failure(expected);
        bool threw{};
        std::optional<Result<void>> returned;
        {
            Tests::AllocationProbe::ScopedFailure failure{index};
            try {
                returned.emplace(PreservePlaySaveFailure(producer, std::move(original)));
            } catch (const std::bad_alloc &) {
                threw = true;
            }
        }
        REQUIRE(threw);
        CHECK_FALSE(returned);
        const auto terminal = producer.Handle().Snapshot();
        REQUIRE(terminal);
        REQUIRE(terminal->state == SaveOperationState::Failed);
        REQUIRE(terminal->terminalError);
        SameFailure(*terminal->terminalError, expected);
        CHECK(callbacks == 1);
        CHECK(producer.RequestShutdownCancellation() == SaveCancellationRequestResult::AlreadyTerminal);
        static_cast<void>(producer.ObserveCancellation());
        const auto afterRollbackCancellation = producer.Handle().Snapshot();
        REQUIRE(afterRollbackCancellation->terminalError);
        SameFailure(*afterRollbackCancellation->terminalError, expected);
        CHECK(callbacks == 1);
    }
}

TEST_CASE("Play exceptional first-error fallback keeps cancellation that already won",
          "[save][operation][editor][failure][allocation][cancel]") {
    auto producer = Producer();
    REQUIRE(producer.Handle().RequestCancellation() == SaveCancellationRequestResult::Requested);
    auto original = Result<void>::Failure(OriginalFailure());
    bool threw{};
    {
        Tests::AllocationProbe::ScopedFailure failure{0};
        try {
            static_cast<void>(PreservePlaySaveFailure(producer, std::move(original)));
        } catch (const std::bad_alloc &) {
            threw = true;
        }
    }
    REQUIRE(threw);
    const auto terminal = producer.Handle().Snapshot();
    REQUIRE(terminal);
    REQUIRE(terminal->state == SaveOperationState::Cancelled);
    REQUIRE(terminal->terminalError);
    CHECK(terminal->terminalError->code.Value() == SaveErrors::OperationCancelled.code.Value());
}

TEST_CASE("Successful Play preparation does not publish a failure", "[save][operation][editor][failure]") {
    auto producer = Producer();
    auto returned = PreservePlaySaveFailure(producer, Result<bool>::Success(true));
    REQUIRE(returned.HasValue());
    CHECK(returned.Value());
    const auto current = producer.Handle().Snapshot();
    REQUIRE(current);
    CHECK_FALSE(current->IsTerminal());
}
