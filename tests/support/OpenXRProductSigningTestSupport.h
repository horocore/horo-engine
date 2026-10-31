#pragma once
/** @file OpenXRProductSigningTestSupport.h
 * @brief Test-owned ECDSA root and exact-byte signatures for real product installation fixtures.
 */
#include "Horo/Security/ArtifactSignature.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <mbedtls/ecdsa.h>
#include <vector>

namespace Horo::Tests::OpenXRProduct {
    /** @brief Test-owned key material, unrelated to product release roots; every partial failure has RAII cleanup. */
    struct SigningKey final {
        SigningKey(const SigningKey &) = delete;
        SigningKey &operator=(const SigningKey &) = delete;

        struct Group final {
            Group(const Group &) = delete;
            Group &operator=(const Group &) = delete;
            mbedtls_ecp_group value;

            Group() {
                mbedtls_ecp_group_init(&value);
            }

            ~Group() {
                mbedtls_ecp_group_free(&value);
            }
        } group;

        struct Point final {
            Point(const Point &) = delete;
            Point &operator=(const Point &) = delete;
            mbedtls_ecp_point value;

            Point() {
                mbedtls_ecp_point_init(&value);
            }

            ~Point() {
                mbedtls_ecp_point_free(&value);
            }
        } point;

        struct Integer final {
            Integer(const Integer &) = delete;
            Integer &operator=(const Integer &) = delete;
            mbedtls_mpi value;

            Integer() {
                mbedtls_mpi_init(&value);
            }

            ~Integer() {
                mbedtls_mpi_free(&value);
            }
        } secret;

        std::uint32_t random{0x48305230U};
        Security::TrustedSigningKey root;

        static int Random(void *state, unsigned char *output, const std::size_t size) {
            auto &value = *static_cast<std::uint32_t *>(state);
            for (std::size_t index = 0; index < size; ++index) {
                value = value * 1664525U + 1013904223U;
                output[index] = static_cast<unsigned char>(value >> 24U);
            }
            return 0;
        }

        SigningKey() {
            REQUIRE(mbedtls_ecp_group_load(&group.value, MBEDTLS_ECP_DP_SECP256R1) == 0);
            REQUIRE(mbedtls_mpi_lset(&secret.value, 1) == 0);
            REQUIRE(mbedtls_ecp_mul(&group.value, &point.value, &secret.value, &group.value.G, Random, &random) == 0);
            root.publisherId = "com.horo.tests";
            root.keyId = "host-test-key";
            root.publicKey.resize(65);
            std::size_t size{};
            REQUIRE(mbedtls_ecp_point_write_binary(&group.value, &point.value, MBEDTLS_ECP_PF_UNCOMPRESSED, &size,
                                                   reinterpret_cast<unsigned char *>(root.publicKey.data()), root.publicKey.size()) == 0);
            REQUIRE(size == root.publicKey.size());
        }

        Security::DetachedSignatureEnvelope Sign(const std::span<const std::byte> bytes) {
            Security::DetachedSignatureEnvelope envelope{.publisherId = root.publisherId,
                                                         .keyId = root.keyId,
                                                         .artifactDigest = ComputeSha256(bytes),
                                                         .signature = std::vector<std::byte>(64)};
            const auto digest = Security::ComputeSignaturePayloadDigest(envelope);
            Integer r, s;
            REQUIRE(mbedtls_ecdsa_sign(&group.value, &r.value, &s.value, &secret.value, digest.bytes.data(), digest.bytes.size(), Random,
                                       &random) == 0);
            REQUIRE(mbedtls_mpi_write_binary(&r.value, reinterpret_cast<unsigned char *>(envelope.signature.data()), 32) == 0);
            REQUIRE(mbedtls_mpi_write_binary(&s.value, reinterpret_cast<unsigned char *>(envelope.signature.data()) + 32, 32) == 0);
            return envelope;
        }
    };
}  // namespace Horo::Tests::OpenXRProduct
