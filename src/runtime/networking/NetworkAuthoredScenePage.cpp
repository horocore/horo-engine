#include "Horo/Network/NetworkAuthoredScenePage.h"

#include "AuthoredSceneBindingValidation.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkAdmissionWire.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        using AdmissionWire::Reader;
        using AdmissionWire::Writer;
        constexpr std::uint16_t Version = 1;
        constexpr std::size_t RowBytes = 20;
        static_assert(MaximumNetworkAuthoredSceneObjects <= std::numeric_limits<std::uint16_t>::max());
        static_assert(MaximumNetworkAuthoredScenePageBytes ==
                      MinimumNetworkAuthoredScenePageBytes + MaximumNetworkAuthoredScenePageObjects * RowBytes);

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(NetworkErrors::MessageEnvelopeInvalid));
        }

        template <typename T> Result<T> Capacity() {
            return Result<T>::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        }

        /** @brief Checks declared counts without underflow or trusting a remote allocation size. */
        Result<void> ValidateLayout(const NetworkAuthoredScenePage &page, std::size_t count) {
            if (page.totalObjects > MaximumNetworkAuthoredSceneObjects || count > MaximumNetworkAuthoredScenePageObjects)
                return Capacity<void>();
            if (!page.session.IsValid() || !page.authority.IsValid() || page.firstIndex > page.totalObjects)
                return Invalid<void>();
            if (count > page.totalObjects - page.firstIndex || (count == 0 && page.totalObjects != 0))
                return Invalid<void>();
            return Result<void>::Success();
        }

        /** @brief Checks canonical IDs and stable provenance across the supplied range. */
        Result<void> ValidateRows(const NetworkAuthoredScenePage &page, std::optional<Runtime::SceneObjectId> previous = {}) {
            std::uint64_t slot = page.firstIndex + 1;
            for (const auto &row : page.objects) {
                if (!Detail::ValidAuthoredBinding(row, page.authority, slot++, previous))
                    return Invalid<void>();
                previous = row.provenance.authoredObject;
            }
            return Result<void>::Success();
        }

        /** @brief Writes the complete versioned page header. */
        bool WriteHeader(Writer &writer, const NetworkAuthoredScenePage &page) {
            return writer.Number(Version, 2) && writer.Number(0, 2) && writer.Number(page.session.Value(), 8) &&
                   writer.Number(page.authority.Value(), 8) && writer.Number(page.totalObjects, 2) && writer.Number(page.firstIndex, 2) &&
                   writer.Number(page.objects.size(), 2);
        }

        /** @brief Writes one validated authored row in stable provenance order. */
        bool WriteRow(Writer &writer, const ReplicationAuthoredObjectBinding &row) {
            const auto &provenance = row.provenance;
            return writer.Number(provenance.authoredObject->value, 8) && writer.Number(provenance.schema.Value(), 8) &&
                   writer.Number(provenance.schemaVersion.major, 2) && writer.Number(provenance.schemaVersion.minor, 2);
        }

        /** @brief Reads the scope after checking the payload version and reserved bits. */
        bool ReadScope(Reader &reader, NetworkAuthoredScenePage &page) {
            std::uint16_t version{}, reserved{};
            std::uint64_t sessionValue{}, epochValue{};
            if (!reader.Number(version, 2) || version != Version || !reader.Number(reserved, 2) || reserved != 0 ||
                !reader.Number(sessionValue, 8) || !reader.Number(epochValue, 8))
                return false;
            const auto session = NetworkSessionGeneration::Create(sessionValue);
            const auto authority = ReplicationAuthorityEpoch::Create(epochValue);
            if (session.HasError() || authority.HasError())
                return false;
            page.session = session.Value();
            page.authority = authority.Value();
            return true;
        }

        /** @brief Decodes and validates the fixed header before touching row storage. */
        Result<void> ReadHeader(Reader &reader, NetworkAuthoredScenePage &page, std::uint16_t &count) {
            if (!ReadScope(reader, page) || !reader.Number(page.totalObjects, 2) || !reader.Number(page.firstIndex, 2) ||
                !reader.Number(count, 2))
                return Invalid<void>();
            return ValidateLayout(page, count);
        }

        /** @brief Rebuilds the initial ID from the authority's wire index and epoch. */
        Result<ReplicationAuthoredObjectBinding> ReadRow(Reader &reader, ReplicationAuthorityEpoch authority, std::uint64_t slot) {
            Runtime::SceneObjectId authored;
            std::uint64_t schemaValue{};
            ReplicationSchemaVersion version;
            if (!reader.Number(authored.value, 8) || !reader.Number(schemaValue, 8) || !reader.Number(version.major, 2) ||
                !reader.Number(version.minor, 2))
                return Invalid<ReplicationAuthoredObjectBinding>();
            const auto schema = ReplicationSchemaId::Create(schemaValue);
            const auto object = NetworkObjectId::Create(authority, slot, 1);
            if (schema.HasError() || object.HasError())
                return Invalid<ReplicationAuthoredObjectBinding>();
            return Result<ReplicationAuthoredObjectBinding>::Success({object.Value(), {schema.Value(), version, authored}});
        }
    }  // namespace

    /** @copydoc MakeNetworkAuthoredScenePage */
    Result<NetworkAuthoredScenePage> MakeNetworkAuthoredScenePage(const ReplicationAuthoredSceneInventory &inventory,
                                                                  std::size_t firstIndex, std::size_t maximumObjects) {
        using Outcome = Result<NetworkAuthoredScenePage>;
        if (firstIndex > inventory.objects.size() || maximumObjects == 0 || maximumObjects > MaximumNetworkAuthoredScenePageObjects)
            return Invalid<NetworkAuthoredScenePage>();
        const auto count = std::min(maximumObjects, inventory.objects.size() - firstIndex);
        NetworkAuthoredScenePage page{inventory.session, inventory.authority, inventory.objects.size(), firstIndex};
        if (auto valid = ValidateLayout(page, count); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            const auto source = std::span{inventory.objects}.subspan(firstIndex, count);
            page.objects.assign(source.begin(), source.end());
            if (auto valid = ValidateRows(page); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(std::move(page));
        } catch (const std::bad_alloc &) {
            return Capacity<NetworkAuthoredScenePage>();
        }
    }

    /** @copydoc EncodeNetworkAuthoredScenePage */
    Result<std::vector<std::byte>> EncodeNetworkAuthoredScenePage(const NetworkAuthoredScenePage &page) {
        using Outcome = Result<std::vector<std::byte>>;
        if (auto valid = ValidateLayout(page, page.objects.size()); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (auto valid = ValidateRows(page); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            std::vector<std::byte> bytes(MinimumNetworkAuthoredScenePageBytes + page.objects.size() * RowBytes);
            Writer writer{bytes};
            if (!WriteHeader(writer, page))
                return Invalid<std::vector<std::byte>>();
            for (const auto &row : page.objects)
                if (!WriteRow(writer, row))
                    return Invalid<std::vector<std::byte>>();
            return Outcome::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Capacity<std::vector<std::byte>>();
        }
    }

    /** @copydoc DecodeNetworkAuthoredScenePage */
    Result<NetworkAuthoredScenePage> DecodeNetworkAuthoredScenePage(std::span<const std::byte> bytes) {
        using Outcome = Result<NetworkAuthoredScenePage>;
        if (bytes.size() > MaximumNetworkAuthoredScenePageBytes)
            return Capacity<NetworkAuthoredScenePage>();
        Reader reader{bytes};
        NetworkAuthoredScenePage page;
        std::uint16_t count{};
        if (auto valid = ReadHeader(reader, page, count); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (bytes.size() != MinimumNetworkAuthoredScenePageBytes + count * RowBytes)
            return Invalid<NetworkAuthoredScenePage>();
        try {
            page.objects.reserve(count);
            for (std::uint16_t index = 0; index < count; ++index) {
                auto row = ReadRow(reader, page.authority, page.firstIndex + index + 1);
                if (row.HasError())
                    return Outcome::Failure(row.ErrorValue());
                page.objects.push_back(std::move(row).Value());
            }
            if (auto valid = ValidateRows(page); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(std::move(page));
        } catch (const std::bad_alloc &) {
            return Capacity<NetworkAuthoredScenePage>();
        }
    }

    /** @copydoc NetworkAuthoredSceneAssembly::Append */
    Result<void> NetworkAuthoredSceneAssembly::Append(const NetworkAuthoredScenePage &page) {
        if (page.session != inventory_.session || page.authority != inventory_.authority)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        if (auto valid = ValidateLayout(page, page.objects.size()); valid.HasError())
            return valid;
        if (IsComplete() || page.firstIndex != inventory_.objects.size() || (expectedRows_ && page.totalObjects != *expectedRows_))
            return Invalid<void>();
        const auto previous = inventory_.objects.empty() ? std::nullopt : inventory_.objects.back().provenance.authoredObject;
        if (auto valid = ValidateRows(page, previous); valid.HasError())
            return valid;
        try {
            if (!expectedRows_)
                inventory_.objects.reserve(page.totalObjects);
            inventory_.objects.insert(inventory_.objects.end(), page.objects.begin(), page.objects.end());
            expectedRows_ = page.totalObjects;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Capacity<void>();
        }
    }

    /** @copydoc NetworkAuthoredSceneAssembly::IsComplete */
    bool NetworkAuthoredSceneAssembly::IsComplete() const noexcept {
        return expectedRows_ && inventory_.objects.size() == *expectedRows_;
    }

    /** @copydoc NetworkAuthoredSceneAssembly::Take */
    Result<ReplicationAuthoredSceneInventory> NetworkAuthoredSceneAssembly::Take() {
        if (!IsComplete())
            return Result<ReplicationAuthoredSceneInventory>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        expectedRows_.reset();
        return Result<ReplicationAuthoredSceneInventory>::Success(std::exchange(inventory_, {}));
    }
}  // namespace Horo::Network
