#include "AnimationCookedFormat.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Animation::CookedInternal {
    namespace {
        constexpr std::array<std::uint8_t, 8> Magic{'H', 'A', 'N', 'I', 'M', '0', '0', '1'};
        constexpr std::string_view Contribution = "horo.animation.cooked";
        constexpr std::string_view CodecIdentity = "horo.animation.cooked.v1;little-endian;finite-positive-zero;nfc;explicit-fields";

        /** @brief Encodes the exact contribution identity in its fixed zero-padded header field. */
        std::array<std::uint8_t, 64> ContributionBytes() {
            std::array<std::uint8_t, 64> bytes{};
            std::ranges::copy(Contribution, bytes.begin());
            return bytes;
        }

        /** @brief Binds payload compatibility to the versioned domain wire semantics, independent from a runtime pointer. */
        Sha256Digest CodecDigest() {
            return ComputeSha256(std::as_bytes(std::span{CodecIdentity.data(), CodecIdentity.size()}));
        }

        /** @brief Hashes header, metadata, tables and complete sections with only the checksum field zeroed. */
        Sha256Digest CompleteDigest(std::span<const std::uint8_t> bytes) {
            std::array<std::byte, 32> zero{};
            Sha256Builder digest;
            static_cast<void>(digest.Update(std::as_bytes(bytes.first(208))));
            static_cast<void>(digest.Update(zero));
            static_cast<void>(digest.Update(std::as_bytes(bytes.subspan(HeaderBytes))));
            return digest.Finalize();
        }

        /** @brief Reads one digest explicitly rather than aliasing native storage. */
        Sha256Digest ReadDigest(Reader &reader) {
            Sha256Digest value;
            const auto bytes = reader.Raw(32);
            if (reader.policy.Good())
                std::ranges::copy(bytes, value.bytes.begin());
            return value;
        }

        /** @brief Keeps every immutable header identity/version/reserved field exact before any table allocation. */
        bool ReadPrefix(Reader &reader, Sections &sections) {
            if (!std::ranges::equal(reader.Raw(8), Magic))
                return reader.policy.Reject(AnimationAssetErrors::Unsupported);
            const auto major = reader.Scalar<std::uint16_t>();
            const auto minor = reader.Scalar<std::uint16_t>();
            const auto header = reader.Scalar<std::uint32_t>();
            if (major != 1 || minor != 0 || header != HeaderBytes)
                return reader.policy.Reject(AnimationAssetErrors::Unsupported);
            sections.kind = static_cast<AnimationCookedKind>(reader.Scalar<std::uint32_t>());
            for (auto &version : sections.version)
                version = reader.Scalar<std::uint16_t>();
            if (reader.Scalar<std::uint16_t>() != 0)
                return reader.policy.Reject(AnimationAssetErrors::Malformed);
            Transfer(reader, sections.asset);
            return reader.policy.Good();
        }

        /** @brief Rejects alternate cooker identity/version/padding rather than inferring compatibility. */
        bool ReadContribution(Reader &reader) {
            if (reader.Scalar<std::uint32_t>() != Contribution.size())
                return reader.policy.Reject(AnimationAssetErrors::Unsupported);
            const auto major = reader.Scalar<std::uint16_t>();
            const auto minor = reader.Scalar<std::uint16_t>();
            const auto patch = reader.Scalar<std::uint16_t>();
            const auto reserved = reader.Scalar<std::uint16_t>();
            if (major != 1 || minor != 0 || patch != 0 || reserved != 0)
                return reader.policy.Reject(AnimationAssetErrors::Unsupported);
            if (!std::ranges::equal(reader.Raw(64), ContributionBytes()) || ReadDigest(reader) != CodecDigest())
                return reader.policy.Reject(AnimationAssetErrors::Unsupported);
            return reader.policy.Good();
        }

        /** @brief Checks one canonical variable section against the preceding exact end and actual total bytes. */
        bool ReadSection(Reader &reader, std::uint32_t expectedKind, std::uint32_t expectedCount, std::size_t &next,
                         std::span<const std::uint8_t> &section) {
            const auto kind = reader.Scalar<std::uint32_t>();
            const auto flags = reader.Scalar<std::uint32_t>();
            const auto offset = reader.Scalar<std::uint64_t>();
            const auto length = reader.Scalar<std::uint64_t>();
            const auto count = reader.Scalar<std::uint32_t>();
            const auto stride = reader.Scalar<std::uint32_t>();
            if (kind != expectedKind || flags != 0 || offset != next || count != expectedCount || stride != 0)
                return reader.policy.Reject(AnimationAssetErrors::Malformed);
            if (length > reader.bytes.size() - next)
                return reader.policy.Reject(AnimationAssetErrors::Malformed);
            section = reader.bytes.subspan(next, static_cast<std::size_t>(length));
            next += static_cast<std::size_t>(length);
            return reader.policy.Good();
        }

        /** @brief Separates count/storage admission from table interpretation. */
        bool ReadInventory(Reader &reader, Sections &sections, Sha256Digest &dependencyDigest, Sha256Digest &checksum) {
            dependencyDigest = ReadDigest(reader);
            const auto total = reader.Scalar<std::uint64_t>();
            const auto table = reader.Scalar<std::uint64_t>();
            const auto count = reader.Scalar<std::uint32_t>();
            sections.dependencyCount = reader.Scalar<std::uint32_t>();
            checksum = ReadDigest(reader);
            if (total != reader.bytes.size() || table != HeaderBytes || count != SectionCount)
                return reader.policy.Reject(AnimationAssetErrors::Malformed);
            if (sections.dependencyCount > reader.policy.context.limits.dependencies)
                return reader.policy.Reject(AnimationAssetErrors::Capacity);
            return reader.policy.Good();
        }

        /** @brief Emits the fixed semantic identity region using the same explicit scalar spelling as its reader. */
        void WritePrefix(Writer &writer, AnimationCookedKind kind, Assets::AssetId asset, std::array<std::uint16_t, 3> version) {
            writer.Raw(Magic);
            writer.Scalar<std::uint16_t>(1);
            writer.Scalar<std::uint16_t>(0);
            writer.Scalar<std::uint32_t>(HeaderBytes);
            writer.Scalar<std::uint32_t>(static_cast<std::uint32_t>(kind));
            for (const auto part : version)
                writer.Scalar(part);
            writer.Scalar<std::uint16_t>(0);
            Transfer(writer, asset);
            writer.Scalar<std::uint32_t>(static_cast<std::uint32_t>(Contribution.size()));
            writer.Scalar<std::uint16_t>(1);
            writer.Scalar<std::uint16_t>(0);
            writer.Scalar<std::uint16_t>(0);
            writer.Scalar<std::uint16_t>(0);
            writer.Raw(ContributionBytes());
            writer.Raw(CodecDigest().bytes);
        }

        /** @brief Emits one exact variable-section descriptor; no ABI-dependent object bytes enter the file. */
        void WriteSection(Writer &writer, std::uint32_t kind, std::uint64_t offset, std::uint64_t length, std::uint32_t count) {
            writer.Scalar(kind);
            writer.Scalar<std::uint32_t>(0);
            writer.Scalar(offset);
            writer.Scalar(length);
            writer.Scalar(count);
            writer.Scalar<std::uint32_t>(0);
        }

        bool PositiveLimits(const AnimationCookedLimits &limits) {
            return limits.bytes > 0 && limits.ownedBytes > 0 && limits.values > 0 && limits.dependencies > 0 &&
                   limits.dependencyBytes > 0 && limits.stringBytes > 0;
        }

        bool WithinHardLimits(const AnimationCookedLimits &limits) {
            return limits.bytes <= AnimationCookedHardLimits::Bytes && limits.ownedBytes <= AnimationCookedHardLimits::OwnedBytes &&
                   limits.values <= AnimationCookedHardLimits::Values && limits.dependencies <= AnimationCookedHardLimits::Dependencies &&
                   limits.dependencyBytes <= AnimationCookedHardLimits::DependencyBytes &&
                   limits.stringBytes <= AnimationCookedHardLimits::StringBytes;
        }
    }  // namespace

    /** @copydoc ValidateContext */
    Result<void> ValidateContext(const AnimationCookedContext &context) {
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Cancelled));
        if (!context.accepting)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Unavailable));
        if (!PositiveLimits(context.limits) || !WithinHardLimits(context.limits))
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        return Result<void>::Success();
    }

    namespace {
        /** @brief Checks every fixed header component before section borrows are formed. */
        bool ReadCompleteHeader(Reader &reader, Sections &sections, Sha256Digest &dependencies, Sha256Digest &checksum) {
            return ReadPrefix(reader, sections) && ReadContribution(reader) && ReadInventory(reader, sections, dependencies, checksum);
        }

        /** @brief Bounds both exact sections and forbids gaps, overlaps or trailing bytes. */
        bool ReadCompleteSections(Reader &reader, Sections &sections) {
            std::size_t next = BodyOffset;
            if (!ReadSection(reader, 1, 1, next, sections.body) ||
                !ReadSection(reader, 2, sections.dependencyCount, next, sections.dependencies))
                return false;
            if (next != reader.bytes.size())
                return reader.policy.Reject(AnimationAssetErrors::Malformed);
            return true;
        }

        /** @brief Retains exact selected identity and byte integrity without an asset-readiness claim. */
        bool MatchingContainer(const Sections &sections, AnimationCookedKind kind, Assets::AssetId asset, Sha256Digest dependencies,
                               Sha256Digest checksum, std::span<const std::uint8_t> bytes) {
            return sections.kind == kind && sections.asset == asset &&
                   dependencies == ComputeSha256(std::as_bytes(sections.dependencies)) && checksum == CompleteDigest(bytes);
        }
    }  // namespace

    /** @copydoc ReadSections */
    Result<Sections> ReadSections(std::span<const std::uint8_t> bytes, AnimationCookedKind expected, Assets::AssetId asset,
                                  const AnimationCookedContext &context) {
        using Return = Result<Sections>;
        if (auto valid = ValidateContext(context); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (bytes.size() < BodyOffset || bytes.size() > context.limits.bytes)
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        Reader reader{bytes, context};
        Sections sections;
        Sha256Digest dependencies;
        Sha256Digest checksum;
        if (!ReadCompleteHeader(reader, sections, dependencies, checksum) || !ReadCompleteSections(reader, sections))
            return Return::Failure(MakeError(*reader.policy.failure));
        if (!MatchingContainer(sections, expected, asset, dependencies, checksum, bytes))
            return Return::Failure(MakeError(AnimationAssetErrors::Malformed));
        sections.work = reader.policy.work;
        return Return::Success(sections);
    }

    /** @copydoc WriteSections */
    Result<std::vector<std::uint8_t>> WriteSections(AnimationCookedKind kind, Assets::AssetId asset, std::array<std::uint16_t, 3> version,
                                                    std::span<const std::uint8_t> body, std::span<const std::uint8_t> dependencies,
                                                    std::uint32_t dependencyCount, const AnimationCookedContext &context) {
        using Return = Result<std::vector<std::uint8_t>>;
        if (auto valid = ValidateContext(context); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (body.size() > context.limits.bytes || dependencies.size() > context.limits.bytes - body.size())
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        const auto contents = body.size() + dependencies.size();
        if (BodyOffset > context.limits.bytes - contents || dependencyCount > context.limits.dependencies)
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        Writer writer{context};
        WritePrefix(writer, kind, asset, version);
        writer.Raw(ComputeSha256(std::as_bytes(dependencies)).bytes);
        writer.Scalar<std::uint64_t>(BodyOffset + contents);
        writer.Scalar<std::uint64_t>(HeaderBytes);
        writer.Scalar<std::uint32_t>(SectionCount);
        writer.Scalar(dependencyCount);
        writer.Raw(std::array<std::uint8_t, 32>{});
        WriteSection(writer, 1, BodyOffset, body.size(), 1);
        WriteSection(writer, 2, BodyOffset + body.size(), dependencies.size(), dependencyCount);
        writer.Raw(body);
        writer.Raw(dependencies);
        if (!writer.policy.Good())
            return Return::Failure(MakeError(*writer.policy.failure));
        const auto checksum = CompleteDigest(writer.bytes);
        std::ranges::copy(checksum.bytes, writer.bytes.begin() + 208);
        return Return::Success(std::move(writer.bytes));
    }

    /** @copydoc ValidateDependencies */
    Result<void> ValidateDependencies(std::span<const Assets::AssetDependency> expected, std::span<const Assets::AssetDependency> actual) {
        if (expected.size() != actual.size())
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Malformed));
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (expected[index].id != actual[index].id || expected[index].expectedType != actual[index].expectedType)
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Malformed));
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::CookedInternal
