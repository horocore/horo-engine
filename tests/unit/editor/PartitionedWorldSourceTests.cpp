#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "WorldCompositionSourceTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        using WorldCompositionTests::WorldSource;

        void ReplaceSource(std::string &source, const std::string_view from, const std::string_view to) {
            const auto position = source.find(from);
            REQUIRE(position != std::string::npos);
            source.replace(position, from.size(), to);
        }

        void RequireSourceFailure(const std::string_view source, const ErrorCodeDescriptor *expected) {
            const auto decoded = DecodePartitionedWorldDocument(source);
            REQUIRE(decoded.HasError());
            if (expected)
                CHECK(decoded.ErrorValue().code == expected->code);
        }
    }  // namespace

    TEST_CASE("Partitioned world source opens real metadata and source coverage without cooked payloads", "[editor][world_source]") {
        auto decoded = DecodePartitionedWorldDocument(WorldSource());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().pages.front().objects.front().descriptor.bounds.minimum.Millimeters()[0] == -1);
        const auto owner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000009").Value();
        PartitionedWorldWorkspace workspace{owner};
        const auto current = workspace.Open(decoded.Value()).Value();
        CHECK(workspace.Browse({}, current->fence).Value().cells.size() == 3);
        CHECK(current->openPages.empty());
        CHECK_FALSE(current->dirty);
    }

    TEST_CASE("Partitioned world source rejects malformed duplicate missing and version-skewed fields", "[editor][world_source]") {
        auto source = WorldSource();
        const ErrorCodeDescriptor *error = &WorldDocumentErrors::Invalid;
        SECTION("malformed") {
            source.pop_back();
        }
        SECTION("duplicate root key") {
            ReplaceSource(source, "\"version\":1", "\"version\":1,\"version\":1");
        }
        SECTION("duplicate nested key") {
            ReplaceSource(source, "\"id\":0", "\"id\":0,\"id\":0");
        }
        SECTION("unknown field") {
            ReplaceSource(source, "\"version\":1", "\"version\":1,\"extra\":0");
        }
        SECTION("new schema") {
            ReplaceSource(source, "\"version\":1", "\"version\":2");
            error = &WorldDocumentErrors::Unsupported;
        }
        SECTION("missing world") {
            ReplaceSource(source, "\"world\"", "\"missing\"");
        }
        RequireSourceFailure(source, error);
    }

    TEST_CASE("Partitioned world source rejects lossy numerics and invalid identity membership", "[editor][world_source]") {
        auto source = WorldSource();
        const ErrorCodeDescriptor *error = &WorldDocumentErrors::Invalid;
        SECTION("coordinate float") {
            ReplaceSource(source, "[-1,0,0]", "[-1.5,0,0]");
        }
        SECTION("unsigned overflow") {
            ReplaceSource(source, "[-1,0,0]", "[18446744073709551615,0,0]");
        }
        SECTION("boolean priority") {
            ReplaceSource(source, "\"priority\":1", "\"priority\":true");
        }
        SECTION("negative unsigned") {
            ReplaceSource(source, "\"id\":1", "\"id\":-1");
        }
        SECTION("narrowing layer id") {
            ReplaceSource(source, "\"id\":0", "\"id\":65536");
        }
        SECTION("invalid identity") {
            ReplaceSource(source, "00000000-0000-0000-0000-000000000001", "00000000-0000-0000-0000-000000000000");
        }
        SECTION("wrong layer membership") {
            ReplaceSource(source, "\"layer\":0", "\"layer\":7");
            error = &WorldDocumentErrors::Conflict;
        }
        SECTION("unsupported placement") {
            ReplaceSource(source, "\"placement\":0", "\"placement\":255");
            error = nullptr;
        }
        RequireSourceFailure(source, error);
    }

    TEST_CASE("Partitioned world source capacity and cancellation reject without admitting a document", "[editor][world_source]") {
        auto source = WorldSource();
        std::stop_source stop;
        stop.request_stop();
        CHECK(DecodePartitionedWorldDocument(source, stop.get_token()).ErrorValue().code == WorldDocumentErrors::Cancelled.code);
        source.assign(8 * 1024 * 1024 + 1, ' ');
        CHECK(DecodePartitionedWorldDocument(source).ErrorValue().code == WorldDocumentErrors::Capacity.code);
        source = std::string(40, '[') + "0" + std::string(40, ']');
        CHECK(DecodePartitionedWorldDocument(source).ErrorValue().code == WorldDocumentErrors::Capacity.code);
    }
}  // namespace Horo::Editor
