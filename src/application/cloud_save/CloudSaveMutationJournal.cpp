#include "Horo/Application/CloudSaveMutationJournal.h"

#include "Horo/Application/CloudSaveTransport.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Application {
    namespace {
        using namespace PlatformServices;
        using namespace Runtime;
        using Bytes = std::vector<std::uint8_t>;

        constexpr std::string_view recordDomain = "horo.cloud-save.mutation.v1";
        constexpr auto identityBytes = SaveIdentityDetail::Bytes{}.size();
        constexpr auto digestBytes = Sha256Digest{}.bytes.size();
        constexpr auto keyBytes = 2 * digestBytes;
        constexpr auto sizeBytes = sizeof(std::uint64_t);
        constexpr auto minimumBytes = recordDomain.size() + 4 * identityBytes + 4 * digestBytes + keyBytes + sizeBytes;
        constexpr auto maximumBytes = minimumBytes + MaximumCloudObjectAddressBytes;

        /** @brief Validates trusted and recovered intents under the same finite contract. */
        bool ValidIntent(const CloudSaveMutationIntent &intent) {
            if (!intent.mutation.IsValid() || !intent.slot.IsValid() || !intent.generation.IsValid() ||
                (intent.parent && (!intent.parent->IsValid() || *intent.parent == intent.generation)) ||
                (intent.revision && !intent.revision->IsValid()))
                return false;
            if (!intent.upload)
                return intent.revision.has_value();
            return intent.upload->size > 0 && intent.upload->size <= MaximumCloudObjectPayloadBytes;
        }

        /** @brief Copies canonical scalar bytes into an existing bounded document. */
        template <typename Range> void Append(Bytes &out, const Range &bytes) {
            std::ranges::transform(bytes, std::back_inserter(out), [](const auto value) { return static_cast<std::uint8_t>(value); });
        }

        /** @brief Encodes size in fixed-width big-endian form on every supported host. */
        std::array<std::uint8_t, sizeBytes> EncodeSize(std::uint64_t size) {
            std::array<std::uint8_t, sizeBytes> encoded{};
            for (auto &byte : encoded | std::views::reverse) {
                byte = static_cast<std::uint8_t>(size);
                size >>= 8U;
            }
            return encoded;
        }

        /** @brief Encodes one portable scope-bound journal record and its integrity trailer. */
        Bytes Encode(const CloudSaveMutationIntent &intent, const Sha256Digest &partition, const CloudSaveObjectKey &key) {
            Bytes out;
            out.reserve(minimumBytes + (intent.revision ? intent.revision->Bytes().size() : 0));
            Append(out, recordDomain);
            Append(out, partition.bytes);
            Append(out, intent.mutation.Bytes());
            Append(out, intent.slot.Bytes());
            Append(out, intent.generation.Bytes());
            Append(out, intent.parent.value_or(SlotGenerationId{}).Bytes());
            Append(out, intent.archive.value.bytes);
            Append(out, key.Bytes());
            Append(out, EncodeSize(intent.upload ? intent.upload->size : 0));
            Append(out, intent.upload ? intent.upload->digest.bytes : Sha256Digest{}.bytes);
            if (intent.revision)
                Append(out, intent.revision->Bytes());
            const auto integrity = ComputeSha256(std::as_bytes(std::span{out}));
            Append(out, integrity.bytes);
            return out;
        }

        /** @brief Consumes fixed fields from an already length-checked body. */
        struct Reader final {
            std::span<const std::uint8_t> rest;

            template <std::size_t Count> std::array<std::uint8_t, Count> Take() {
                std::array<std::uint8_t, Count> value{};
                std::ranges::copy(rest.first<Count>(), value.begin());
                rest = rest.subspan(Count);
                return value;
            }
        };

        /** @brief Detached record whose opaque key still requires validation against the live binding. */
        struct Decoded final {
            CloudSaveMutationIntent intent;
            std::array<std::uint8_t, keyBytes> key;
        };

        /** @brief Parses fixed generation facts and bounded optional provider evidence. */
        std::optional<CloudSaveMutationIntent> DecodeIntent(Reader &reader, const SaveCloudMutationId expected,
                                                             std::array<std::uint8_t, keyBytes> &key) {
            if (reader.Take<identityBytes>() != expected.Bytes())
                return std::nullopt;
            const auto slot = SaveGameSlotId::FromBytes(reader.Take<identityBytes>());
            const auto generation = SlotGenerationId::FromBytes(reader.Take<identityBytes>());
            const auto parent = reader.Take<identityBytes>();
            if (slot.HasError() || generation.HasError())
                return std::nullopt;
            CloudSaveMutationIntent intent{.mutation = expected, .slot = slot.Value(), .generation = generation.Value()};
            if (parent != SaveIdentityDetail::Bytes{})
                intent.parent = SlotGenerationId::FromBytes(parent).Value();
            intent.archive = ArchiveContentHash{Sha256Digest{reader.Take<digestBytes>()}};
            key = reader.Take<keyBytes>();
            std::uint64_t size{};
            for (const auto byte : reader.Take<sizeBytes>())
                size = (size << 8U) | byte;
            const Sha256Digest digest{reader.Take<digestBytes>()};
            if (size != 0)
                intent.upload = CloudSaveUploadEvidence{digest, size};
            else if (digest != Sha256Digest{})
                return std::nullopt;
            if (!reader.rest.empty()) {
                auto revision = ProviderObjectRevision::Copy(std::as_bytes(reader.rest));
                if (revision.HasError())
                    return std::nullopt;
                intent.revision = std::move(revision).Value();
            }
            return ValidIntent(intent) ? std::optional{std::move(intent)} : std::nullopt;
        }

        /** @brief Accepts only complete canonical records for this exact journal and mutation. */
        std::optional<Decoded> Decode(const std::span<const std::uint8_t> bytes, const Sha256Digest &partition,
                                      const SaveCloudMutationId expected) {
            if (bytes.size() < minimumBytes || bytes.size() > maximumBytes)
                return std::nullopt;
            const auto body = bytes.first(bytes.size() - digestBytes);
            const auto digest = ComputeSha256(std::as_bytes(body));
            if (!std::ranges::equal(bytes.last<digestBytes>(), digest.bytes))
                return std::nullopt;
            Reader reader{body};
            if (!std::ranges::equal(reader.Take<recordDomain.size()>(), recordDomain) || reader.Take<digestBytes>() != partition.bytes)
                return std::nullopt;
            Decoded decoded;
            auto intent = DecodeIntent(reader, expected, decoded.key);
            if (!intent)
                return std::nullopt;
            decoded.intent = std::move(*intent);
            return decoded;
        }
    }

    CloudSaveDurableMutation::CloudSaveDurableMutation(CloudSaveMutationIntent intent, const Sha256Digest partition,
                                                       CloudSaveObjectKey key)
        : intent_(std::move(intent)), partition_(partition), key_(std::move(key)) {}

    CloudSaveMutationJournal::CloudSaveMutationJournal(std::shared_ptr<const ImmutableFileStorage> storage,
                                                       const CloudSaveTransport &transport)
        : storage_(std::move(storage)), transport_(&transport) {}

    /** @copydoc CloudSaveMutationJournal::Bind */
    Result<CloudSaveMutationJournal> CloudSaveMutationJournal::Bind(std::shared_ptr<const ImmutableFileStorage> storage,
                                                                   CloudSaveTransport &transport) {
        if (!storage)
            return Result<CloudSaveMutationJournal>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        return Result<CloudSaveMutationJournal>::Success(CloudSaveMutationJournal{std::move(storage), transport});
    }

    /** @copydoc CloudSaveMutationJournal::MatchesNamespace */
    bool CloudSaveMutationJournal::MatchesNamespace(const SaveNamespaceId &name) const noexcept {
        return transport_->scope_.localNamespace == name;
    }

    /** @copydoc CloudSaveMutationJournal::EntryName */
    std::string CloudSaveMutationJournal::EntryName(const SaveCloudMutationId mutation) const {
        const auto formatted = FormatSha256(transport_->journalPartition_);
        return formatted.substr(std::string_view{"sha256:"}.size()) + "/" + mutation.ToString() + ".intent";
    }

    /** @copydoc CloudSaveMutationJournal::Store */
    Result<CloudSaveDurableMutation> CloudSaveMutationJournal::Store(CloudSaveMutationIntent intent,
                                                                    const CancellationToken &cancellation) const {
        using Response = Result<CloudSaveDurableMutation>;
        if (!ValidIntent(intent))
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        auto key = transport_->Key(intent.slot);
        if (key.HasError())
            return Response::Failure(key.ErrorValue());
        const auto bytes = Encode(intent, transport_->journalPartition_, key.Value());
        const auto published = storage_->Store(EntryName(intent.mutation), bytes, cancellation);
        if (published.HasError())
            return Response::Failure(published.ErrorValue());
        return Response::Success(CloudSaveDurableMutation{std::move(intent), transport_->journalPartition_, std::move(key).Value()});
    }

    /** @copydoc CloudSaveMutationJournal::Load */
    Result<std::optional<CloudSaveDurableMutation>> CloudSaveMutationJournal::Load(const SaveCloudMutationId mutation,
                                                                                  const CancellationToken &cancellation) const {
        using Response = Result<std::optional<CloudSaveDurableMutation>>;
        if (!mutation.IsValid())
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        const auto bytes = storage_->Load(EntryName(mutation), maximumBytes, cancellation);
        if (bytes.HasError())
            return Response::Failure(bytes.ErrorValue());
        if (!bytes.Value())
            return Response::Success(std::nullopt);
        auto decoded = Decode(*bytes.Value(), transport_->journalPartition_, mutation);
        if (!decoded)
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        auto key = transport_->Key(decoded->intent.slot);
        if (key.HasError() || !std::ranges::equal(key.Value().Bytes(), std::as_bytes(std::span{decoded->key})))
            return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
        return Response::Success(CloudSaveDurableMutation{std::move(decoded->intent), transport_->journalPartition_,
                                                           std::move(key).Value()});
    }
}
