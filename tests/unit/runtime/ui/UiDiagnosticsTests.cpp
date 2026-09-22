#include "Horo/Runtime/Ui/UiDiagnostics.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Creates one valid persistent Runtime UI identity for a diagnostic fixture. */
        template <typename Identity> Identity DiagnosticIdentity(const std::uint8_t marker) {
            auto bytes = SerializedUiId{};
            bytes.back() = marker;
            const auto identity = Identity::Create(bytes);
            REQUIRE(identity.HasValue());
            return identity.Value();
        }

        /** @brief Requires one exact Runtime UI diagnostic-construction failure. */
        void RequireUiFailure(const Result<UiDiagnosticRecord> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE((result.HasError() && result.ErrorValue().code.Value() == expected.code.Value()));
        }

        TEST_CASE("Runtime UI diagnostic categories expose stable derived names", "[runtime_ui][diagnostics]") {
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Document) == "runtime_ui.document");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Layout) == "runtime_ui.layout");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Text) == "runtime_ui.text");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Input) == "runtime_ui.input");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Focus) == "runtime_ui.focus");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Binding) == "runtime_ui.binding");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Render) == "runtime_ui.render");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Accessibility) == "runtime_ui.accessibility");
            REQUIRE(UiDiagnosticCategoryName(UiDiagnosticCategory::Lifecycle) == "runtime_ui.lifecycle");
            REQUIRE(UiDiagnosticCategoryName(static_cast<UiDiagnosticCategory>(255)).empty());
        }

        TEST_CASE("Runtime UI diagnostic records map every canonical error and reject invented sources", "[runtime_ui][diagnostics]") {
            REQUIRE(UiDiagnosticErrorDescriptors().size() == 141);
            for (const ErrorCodeDescriptor *descriptor : UiDiagnosticErrorDescriptors()) {
                CAPTURE(descriptor->code.Value());
                const auto record = MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, MakeError(*descriptor));
                REQUIRE((record.HasValue() && record.Value().code.Value() == descriptor->code.Value()));
            }

            SECTION("unknown code") {
                auto invented = MakeError(UiErrors::DiagnosticInvalid);
                invented.code = ErrorCode{"runtime_ui.future.invented"};
                RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, invented), UiErrors::DiagnosticUnsupported);
            }
            SECTION("foreign domain") {
                auto foreign = MakeError(UiErrors::DiagnosticInvalid);
                foreign.domain = ErrorDomainId{"horo.physics"};
                RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, foreign), UiErrors::DiagnosticUnsupported);
            }
        }

        TEST_CASE("Runtime UI diagnostic severity mapping is complete and preserves fatal evidence", "[runtime_ui][diagnostics]") {
            const auto verify = [](const ErrorSeverity source, const DiagnosticSeverity expected) {
                auto error = MakeError(UiErrors::DocumentInvalid);
                error.severity = source;
                const auto record = MakeUiDiagnosticRecord(UiDiagnosticCategory::Lifecycle, error);
                REQUIRE((record.HasValue() && record.Value().severity == expected));
            };
            verify(ErrorSeverity::Info, DiagnosticSeverity::Note);
            verify(ErrorSeverity::Warning, DiagnosticSeverity::Warning);
            verify(ErrorSeverity::Error, DiagnosticSeverity::Error);
            verify(ErrorSeverity::Critical, DiagnosticSeverity::Fatal);

            auto error = MakeError(UiErrors::DocumentInvalid);
            error.severity = static_cast<ErrorSeverity>(255);
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Lifecycle, error), UiErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime UI diagnostic records own complete ordered correlation evidence", "[runtime_ui][diagnostics]") {
            const auto document = DiagnosticIdentity<UiDocumentId>(1);
            const auto element = DiagnosticIdentity<UiElementId>(2);
            const auto canvas = DiagnosticIdentity<UiCanvasId>(3);
            std::vector<UiDiagnosticCorrelationEntry> correlation{
                {UiDiagnosticCorrelationKey::Document, document},
                {UiDiagnosticCorrelationKey::Element, element},
                {UiDiagnosticCorrelationKey::Canvas, canvas},
                {UiDiagnosticCorrelationKey::Player, std::uint64_t{0}},
                {UiDiagnosticCorrelationKey::Viewport, std::uint64_t{9}},
                {UiDiagnosticCorrelationKey::Operation, std::uint64_t{17}},
            };
            auto error = MakeError(UiErrors::CapacityExceeded, "Layout work exceeded its declared budget.");
            const auto record = MakeUiDiagnosticRecord(UiDiagnosticCategory::Layout, error, correlation);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().schemaVersion == 1);
            REQUIRE(record.Value().category == UiDiagnosticCategory::Layout);
            REQUIRE(record.Value().correlationCount == MaximumUiDiagnosticCorrelationEntries);
            REQUIRE(std::get<UiDocumentId>(record.Value().correlation[0].value) == document);
            REQUIRE(std::get<std::uint64_t>(record.Value().correlation[3].value) == 0);
            REQUIRE(std::get<std::uint64_t>(record.Value().correlation[5].value) == 17);

            error.message[0] = 'X';
            correlation.clear();
            REQUIRE(record.Value().message == "Layout work exceeded its declared budget.");
            REQUIRE(std::get<UiCanvasId>(record.Value().correlation[2].value) == canvas);
        }

        TEST_CASE("Runtime UI diagnostic construction rejects correlation ordering type and range errors", "[runtime_ui][diagnostics]") {
            const auto error = MakeError(UiErrors::DiagnosticInvalid);
            std::array<UiDiagnosticCorrelationEntry, MaximumUiDiagnosticCorrelationEntries + 1> oversized{};
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Input, error, oversized), UiErrors::DiagnosticInvalid);

            const std::array duplicate{
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Player, std::uint64_t{0}},
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Player, std::uint64_t{1}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Input, error, duplicate), UiErrors::DiagnosticInvalid);
            const std::array unordered{
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Operation, std::uint64_t{2}},
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Viewport, std::uint64_t{1}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Render, error, unordered), UiErrors::DiagnosticInvalid);
            const std::array wrongType{
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Document, std::uint64_t{1}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, error, wrongType), UiErrors::DiagnosticInvalid);
            const std::array unknownKey{
                UiDiagnosticCorrelationEntry{static_cast<UiDiagnosticCorrelationKey>(255), std::uint64_t{1}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, error, unknownKey), UiErrors::DiagnosticInvalid);

            const std::array playerOverflow{
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Player, std::uint64_t{256}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Input, error, playerOverflow), UiErrors::DiagnosticInvalid);
            for (const UiDiagnosticCorrelationKey key : {UiDiagnosticCorrelationKey::Viewport, UiDiagnosticCorrelationKey::Operation}) {
                const std::array zero{UiDiagnosticCorrelationEntry{key, std::uint64_t{0}}};
                RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Lifecycle, error, zero), UiErrors::DiagnosticInvalid);
            }
        }

        TEST_CASE("Runtime UI diagnostic construction rejects invalid identities and message boundaries transactionally",
                  "[runtime_ui][diagnostics]") {
            auto error = MakeError(UiErrors::DocumentInvalid);
            const std::array invalidDocument{
                UiDiagnosticCorrelationEntry{UiDiagnosticCorrelationKey::Document, UiDocumentId{}},
            };
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Document, error, invalidDocument), UiErrors::DiagnosticInvalid);

            error.message.assign(MaximumUiDiagnosticMessageBytes, 'a');
            const auto boundary = MakeUiDiagnosticRecord(UiDiagnosticCategory::Text, error);
            REQUIRE(boundary.HasValue());
            error.message.push_back('b');
            const auto original = error.message;
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Text, error), UiErrors::DiagnosticInvalid);
            REQUIRE(error.message == original);

            error.message.clear();
            RequireUiFailure(MakeUiDiagnosticRecord(UiDiagnosticCategory::Text, error), UiErrors::DiagnosticInvalid);
            RequireUiFailure(MakeUiDiagnosticRecord(static_cast<UiDiagnosticCategory>(255), MakeError(UiErrors::DocumentInvalid)),
                             UiErrors::DiagnosticUnsupported);
        }

        TEST_CASE("Runtime UI diagnostic evidence survives source retirement without hidden owner state", "[runtime_ui][diagnostics]") {
            std::vector<UiDiagnosticRecord> retained;
            {
                auto source = MakeError(UiErrors::InstanceStateInvalid, "The source runtime is shutting down.");
                std::vector<UiDiagnosticCorrelationEntry> correlation{
                    {UiDiagnosticCorrelationKey::Document, DiagnosticIdentity<UiDocumentId>(7)},
                    {UiDiagnosticCorrelationKey::Operation, std::uint64_t{21}},
                };
                auto record = MakeUiDiagnosticRecord(UiDiagnosticCategory::Lifecycle, source, correlation);
                REQUIRE(record.HasValue());
                retained.push_back(std::move(record).Value());
            }
            REQUIRE(retained.front().message == "The source runtime is shutting down.");
            REQUIRE(retained.front().correlationCount == 2);
            REQUIRE(std::get<std::uint64_t>(retained.front().correlation[1].value) == 21);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
