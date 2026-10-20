#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Platform/NativePublicationFileSystem.h"
#include "assets/AssetCookOutputFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Assets::OutputTestSupport;

    enum class Denial {
        None,
        Read,
        Inspect,
        Inventory
    };

    /** @brief Synchronous test authority maps a virtual namespace onto owned fixture data; counters are inspected after each call. */
    class ReadAuthority final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::filesystem::path logical;
        std::filesystem::path backing;
        Denial deny{};
        std::filesystem::path deniedFile;
        Error failure{WrapError(FileStorageErrors::IoFailed, MakeError(FileStorageErrors::UnsafeObject))};
        mutable std::size_t reads{};
        mutable std::size_t inspections{};
        mutable std::size_t inventories{};

        Result<std::vector<std::uint8_t>> ReadFileBounded(const std::filesystem::path &path, std::size_t maximumBytes,
                                                          const CancellationToken &cancel = {}) const override {
            ++reads;
            if (deny == Denial::Read && (deniedFile.empty() || path.filename() == deniedFile))
                return Result<std::vector<std::uint8_t>>::Failure(failure);
            return native.ReadFileBounded(Map(path), maximumBytes, cancel);
        }

        Result<std::optional<FileEntryInfo>> InspectEntry(const std::filesystem::path &path) const override {
            ++inspections;
            return deny == Denial::Inspect ? Result<std::optional<FileEntryInfo>>::Failure(failure) : native.InspectEntry(Map(path));
        }

        Result<std::vector<FileDirectoryEntry>> ListDirectory(const std::filesystem::path &path, std::size_t maximumEntries,
                                                              const CancellationToken &cancel = {}) const override {
            ++inventories;
            return deny == Denial::Inventory ? Result<std::vector<FileDirectoryEntry>>::Failure(failure)
                                             : native.ListDirectory(Map(path), maximumEntries, cancel);
        }

    private:
        std::filesystem::path Map(const std::filesystem::path &path) const {
            const auto relative = path.lexically_relative(logical);
            REQUIRE(!relative.empty());
            REQUIRE_FALSE(relative.is_absolute());
            REQUIRE(*relative.begin() != "..");
            return relative == "." ? backing : (backing / relative).lexically_normal();
        }
    };

    /** @brief Owns genuine valid bytes whose logical read namespace need not exist on the ambient filesystem. */
    struct ReadFixture {
        TempDir directory;
        const AssetId id{Id("00000000-0000-0000-0000-000000000001")};
        const std::vector<std::uint8_t> bytes{MakePayload(id, 8, 1)};
        PublicationFiles writer;
        AssetCookGeneration generation;
        ReadAuthority reader;

        ReadFixture() {
            writer.root = directory.path;
            auto published = PublishCookArtifactReplacement(directory.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                                            Policy(writer, "10000000-0000-0000-0000-000000000001"));
            REQUIRE(published.HasValue());
            generation = std::move(published).Value();
            reader.logical = directory.path;
            reader.backing = directory.path;
        }
    };

    /** @brief Requires exact provider identity and its retained diagnostic cause. */
    void SameFailure(const Error &actual, const Error &expected) {
        CHECK(actual.domain.Value() == expected.domain.Value());
        CHECK(actual.code.Value() == expected.code.Value());
        REQUIRE(actual.cause.Get());
        REQUIRE(expected.cause.Get());
        CHECK(actual.cause.Get()->code.Value() == expected.cause.Get()->code.Value());
    }
}  // namespace

TEST_CASE("Cook generations resolve and load through an authority whose logical namespace is absent on disk", "[cook][authority]") {
    ReadFixture fixture;
    fixture.reader.logical = fixture.directory.path / "virtual namespace ü";
    REQUIRE_FALSE(std::filesystem::exists(fixture.reader.logical));
    const auto resolved = ResolveCurrentCookGeneration(fixture.reader, fixture.reader.logical);
    REQUIRE(resolved.HasValue());
    CHECK(resolved.Value().generationRoot ==
          fixture.reader.logical / fixture.generation.generationRoot.lexically_relative(fixture.directory.path));
    const auto contents = ReadCookGenerationContents(fixture.reader, resolved.Value(), 4096U);
    REQUIRE(contents.HasValue());
    REQUIRE(contents.Value().artifacts.size() == 1);
    CHECK(contents.Value().artifacts.front() == fixture.bytes);
    CHECK(fixture.reader.reads > 0);
    CHECK(fixture.reader.inspections > 0);
    CHECK(fixture.reader.inventories > 0);
    CHECK_FALSE(std::filesystem::exists(fixture.reader.logical));
}

TEST_CASE("Cook read and inventory denial preserves exact provider errors without native fallback", "[cook][authority]") {
    ReadFixture fixture;
    fixture.reader.deny = GENERATE(Denial::Read, Denial::Inspect, Denial::Inventory);
    const auto resolved = ResolveCurrentCookGeneration(fixture.reader, fixture.directory.path);
    REQUIRE(resolved.HasError());
    SameFailure(resolved.ErrorValue(), fixture.reader.failure);
    const auto contents = ReadCookGenerationContents(fixture.reader, fixture.generation, 4096U);
    REQUIRE(contents.HasError());
    SameFailure(contents.ErrorValue(), fixture.reader.failure);
    CHECK(ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, fixture.generation, 4096U).HasValue());
}

TEST_CASE("Manifest and artifact reads cannot hide provider failures behind malformed-generation errors", "[cook][authority]") {
    ReadFixture fixture;
    fixture.reader.deny = Denial::Read;
    fixture.reader.deniedFile =
        GENERATE(std::filesystem::path{"manifest.json"}, std::filesystem::path{"00000000-0000-0000-0000-000000000001.cooked"});
    const auto resolved = ResolveCurrentCookGeneration(fixture.reader, fixture.directory.path);
    REQUIRE(resolved.HasError());
    SameFailure(resolved.ErrorValue(), fixture.reader.failure);
    const auto contents = ReadCookGenerationContents(fixture.reader, fixture.generation, 4096U);
    REQUIRE(contents.HasError());
    SameFailure(contents.ErrorValue(), fixture.reader.failure);
}

TEST_CASE("Writer recovery uses its injected read authority and retains the previous selector on denial", "[cook][authority]") {
    ReadFixture fixture;
    fixture.reader.deny = GENERATE(Denial::Read, Denial::Inspect, Denial::Inventory);
    const auto before = ReadText(fixture.directory.path / "current.json");
    const auto bytes = MakePayload(fixture.id, 8, 2);
    const auto failed =
        PublishCookArtifactReplacement(fixture.directory.path, Target("headless-null"), Entry(fixture.id, bytes), bytes, 4096U, {},
                                       {.files = &fixture.reader, .operationId = "10000000-0000-0000-0000-000000000002"});
    REQUIRE(failed.HasError());
    SameFailure(failed.ErrorValue(), fixture.reader.failure);
    CHECK(ReadText(fixture.directory.path / "current.json") == before);
    const auto current = ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.directory.path);
    REQUIRE(current.HasValue());
    CHECK(current.Value().manifestDigest == fixture.generation.manifestDigest);
}

TEST_CASE("Root-bound native authority completes real cook publication recovery and pinned reads", "[cook][authority][rooted]") {
    TempDir directory;
    const auto anchor = std::filesystem::canonical(directory.path);
    const auto root = anchor / "derived" / "cook";
    auto files = Horo::Platform::CreateNativePublicationFileSystem(anchor, "derived/cook");
    REQUIRE(files.HasValue());
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto firstBytes = MakePayload(id, 8, 1);
    auto first = PublishCookArtifactReplacement(root, Target("headless-null"), Entry(id, firstBytes), firstBytes, 4096U, {},
                                                {.files = files.Value().get(), .operationId = "10000000-0000-0000-0000-000000000001"});
    INFO((first.HasError() ? first.ErrorValue().code.Value() : std::string_view{}));
    REQUIRE(first.HasValue());
    const auto secondBytes = MakePayload(id, 8, 2);
    auto second = PublishCookArtifactReplacement(root, Target("headless-null"), Entry(id, secondBytes), secondBytes, 4096U, {},
                                                 {.files = files.Value().get(), .operationId = "10000000-0000-0000-0000-000000000002"});
    INFO((second.HasError() ? second.ErrorValue().code.Value() : std::string_view{}));
    REQUIRE(second.HasValue());
    CHECK(first.Value().manifestDigest != second.Value().manifestDigest);
    CHECK(ReadCookGenerationContents(*files.Value(), first.Value(), 4096U).Value().artifacts.front() == firstBytes);
    CHECK(ReadCookGenerationContents(*files.Value(), second.Value(), 4096U).Value().artifacts.front() == secondBytes);
    CHECK(ResolveCurrentCookGeneration(*files.Value(), root).Value().manifestDigest == second.Value().manifestDigest);
}

TEST_CASE("Real cook commit stays inside its retained project when the former project path is replaced", "[cook][authority][rooted]") {
    TempDir directory;
    const auto parent = std::filesystem::canonical(directory.path);
    const auto anchor = parent / "project";
    std::filesystem::create_directory(anchor);
    auto files = Horo::Platform::CreateNativePublicationFileSystem(anchor, "derived/cook");
    REQUIRE(files.HasValue());
    std::filesystem::rename(anchor, parent / "retained");
    std::filesystem::create_directory(anchor);
    const auto root = anchor / "derived" / "cook";
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8, 1);
    const auto published =
        PublishCookArtifactReplacement(root, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                       {.files = files.Value().get(), .operationId = "10000000-0000-0000-0000-000000000001"});
    INFO((published.HasError() ? published.ErrorValue().code.Value() : std::string_view{}));
    REQUIRE(published.HasValue());
    CHECK_FALSE(std::filesystem::exists(anchor / "derived"));
    CHECK(std::filesystem::exists(parent / "retained" / "derived" / "cook" / "current.json"));
    CHECK(ReadCookGenerationContents(*files.Value(), published.Value(), 4096U).Value().artifacts.front() == bytes);
}
