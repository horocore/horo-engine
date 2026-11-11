#include "LightCullingMetalReflectionInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::LightCullingMetalCookDetail {
    namespace {
        using Words = std::span<const std::uint32_t>;

        struct Node {
            std::uint32_t type{};
            std::uint32_t storage{};
            std::uint32_t element{};
            std::uint32_t root{};
            std::uint32_t binding{~0u};
            std::uint32_t set{~0u};
            std::uint32_t stride{};
            bool pointer{};
            bool block{};
            bool readonlyMember{};
            std::vector<std::uint32_t> members;
            std::vector<std::uint32_t> offsets;
            std::string name;
        };

        struct Instruction {
            std::uint32_t op;
            Words words;
        };

        struct Proof {
            std::vector<Node> ids;
            std::vector<Instruction> instructions;
            std::array<std::uint32_t, 5> resources{};
            std::uint32_t entry{};
            std::uint32_t functions{};
            std::uint32_t localSizes{};
            std::uint32_t extensionSet{};
            bool functionOpen{};
            bool functionEnded{};
        };

        Node &Id(Proof &proof, std::uint32_t id) {
            if (!id || id >= proof.ids.size())
                throw InvalidEvidence{};
            return proof.ids[id];
        }

        void RequireSize(Words words, std::size_t minimum) {
            if (words.size() < minimum)
                throw InvalidEvidence{};
        }

        std::string Text(Words words) {
            const auto *bytes = reinterpret_cast<const char *>(words.data());
            const auto length = words.size_bytes();
            const auto end = std::find(bytes, bytes + length, '\0');
            if (end == bytes + length)
                throw InvalidEvidence{};
            return {bytes, end};
        }

        bool Pointer(Proof &proof, std::uint32_t value) {
            const auto type = Id(proof, value).type;
            return type && Id(proof, type).pointer;
        }

        void ValueOperands(Proof &proof, Words words) {
            for (const auto value : words)
                if (Pointer(proof, value))
                    throw InvalidEvidence{};
        }

        void Typed(Proof &proof, Words words) {
            RequireSize(words, 3);
            if (Id(proof, words[2]).type)
                throw InvalidEvidence{};
            Id(proof, words[2]).type = words[1];
            (void)Id(proof, words[1]);
        }

        void Decoration(Proof &proof, Words w) {
            RequireSize(w, 3);
            auto &node = Id(proof, w[1]);
            if (w[2] == 2)
                node.block = true;
            if (w[2] == 6) {
                RequireSize(w, 4);
                node.stride = w[3];
            }
            if (w[2] == 33) {
                RequireSize(w, 4);
                node.binding = w[3];
            }
            if (w[2] == 34) {
                RequireSize(w, 4);
                node.set = w[3];
            }
        }

        void MemberDecoration(Proof &proof, Words w) {
            RequireSize(w, 4);
            auto &node = Id(proof, w[1]);
            if (w[2] >= 8)
                throw InvalidEvidence{};
            if (w[3] == 24 && w[2] == 0)
                node.readonlyMember = true;
            if (w[3] != 35)
                return;
            RequireSize(w, 5);
            node.offsets.resize(std::max(node.offsets.size(), std::size_t{w[2] + 1}), ~0u);
            node.offsets[w[2]] = w[4];
        }

        void Type(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            RequireSize(w, 2);
            auto &node = Id(proof, w[1]);
            if (instruction.op == 32) {
                RequireSize(w, 4);
                node.pointer = true;
                node.storage = w[2];
                node.element = w[3];
            } else if (instruction.op == 30) {
                if (w.size() > 10)
                    throw InvalidEvidence{};
                node.members.assign(w.begin() + 2, w.end());
            } else if (instruction.op == 29) {
                RequireSize(w, 3);
                node.element = w[2];
            }
        }

        void Entry(Proof &proof, Words w) {
            RequireSize(w, 4);
            if (proof.entry || w[1] != 5 || Text(w.subspan(3)) != "CullLights")
                throw InvalidEvidence{};
            proof.entry = w[2];
        }

        void Execution(Proof &proof, Words w) {
            RequireSize(w, 3);
            if (w[1] != proof.entry)
                throw InvalidEvidence{};
            if (w[2] == 4461 && w.size() == 4 && w[3] == 32)
                return;
            if (w[2] != 17)
                throw InvalidEvidence{};
            if (w.size() != 6 || w[3] != 64 || w[4] != 1 || w[5] != 1)
                throw InvalidEvidence{};
            ++proof.localSizes;
        }

        bool ScalarResult(std::uint32_t op) {
            constexpr std::array ops{1u,   12u,  41u,  42u,  43u,  44u,  46u,  79u,  80u,  81u,  83u,  127u,
                                     128u, 129u, 132u, 133u, 148u, 168u, 170u, 174u, 176u, 180u, 184u, 245u};
            return std::ranges::find(ops, op) != ops.end();
        }

        void Capability(Words w) {
            if (w.size() != 2 || (w[1] != 1 && w[1] != 4466))
                throw InvalidEvidence{};
        }

        void ModuleDeclaration(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            if (instruction.op == 11) {
                RequireSize(w, 3);
                if (proof.extensionSet || Text(w.subspan(2)) != "GLSL.std.450")
                    throw InvalidEvidence{};
                proof.extensionSet = w[1];
            }
            if (instruction.op == 14)
                if (w.size() != 3 || w[1] != 0 || w[2] != 1)
                    throw InvalidEvidence{};
            if (instruction.op == 17)
                Capability(w);
        }

        void IndexInstruction(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            ModuleDeclaration(proof, instruction);
            if (instruction.op >= 19 && instruction.op <= 33) {
                Type(proof, instruction);
                return;
            }
            if (ScalarResult(instruction.op)) {
                Typed(proof, w);
                return;
            }
            if (instruction.op == 5) {
                RequireSize(w, 3);
                Id(proof, w[1]).name = Text(w.subspan(2));
            }
            if (instruction.op == 71)
                Decoration(proof, w);
            if (instruction.op == 72)
                MemberDecoration(proof, w);
            if (instruction.op == 15)
                Entry(proof, w);
            if (instruction.op == 16)
                Execution(proof, w);
        }

        void FunctionBoundary(Proof &proof, const Instruction &instruction) {
            if (instruction.op == 54) {
                if (proof.functionOpen || proof.functionEnded)
                    throw InvalidEvidence{};
                proof.functionOpen = true;
                return;
            }
            if (instruction.op == 56) {
                if (!proof.functionOpen || instruction.words.size() != 1)
                    throw InvalidEvidence{};
                proof.functionOpen = false;
                proof.functionEnded = true;
                return;
            }
            if (proof.functionEnded)
                throw InvalidEvidence{};
        }

        void CompleteFunction(const Proof &proof) {
            if (!proof.entry || proof.functions != 1 || proof.localSizes != 1 || proof.functionOpen || !proof.functionEnded)
                throw InvalidEvidence{};
        }

        void IndexMemory(Proof &proof, const Instruction &instruction) {
            FunctionBoundary(proof, instruction);
            const auto w = instruction.words;
            if (instruction.op == 54) {
                RequireSize(w, 5);
                ++proof.functions;
                if (w[2] != proof.entry)
                    throw InvalidEvidence{};
            }
            constexpr std::array typed{59u, 61u, 65u, 66u};
            if (std::ranges::find(typed, instruction.op) != typed.end())
                Typed(proof, w);
        }

        void Instructions(Proof &proof, Words words) {
            for (std::size_t at = 5; at < words.size();) {
                const auto count = words[at] >> 16;
                if (!count || count > words.size() - at || proof.instructions.size() == 65536)
                    throw InvalidEvidence{};
                proof.instructions.emplace_back(words[at] & 65535, words.subspan(at, count));
                at += count;
            }
        }

        Proof Decode(Words words) {
            RequireSize(words, 5);
            if (words[0] != 0x07230203 || words[3] < 2 || words[3] > 65536 || words[4])
                throw InvalidEvidence{};
            Proof proof;
            proof.ids.resize(words[3]);
            Instructions(proof, words);
            for (const auto &instruction : proof.instructions)
                IndexInstruction(proof, instruction);
            for (const auto &instruction : proof.instructions)
                IndexMemory(proof, instruction);
            CompleteFunction(proof);
            return proof;
        }

        void Resource(Proof &proof, Words w) {
            RequireSize(w, 4);
            if (w.size() != 4)
                throw InvalidEvidence{};
            auto &value = Id(proof, w[2]);
            if (const auto &pointer = Id(proof, w[1]); !pointer.pointer || pointer.storage != w[3])
                throw InvalidEvidence{};
            value.root = w[2];
            if (w[3] == 1)
                return;  // Invocation builtin input, never storage.
            if (w[3] != 12 && w[3] != 2)
                throw InvalidEvidence{};
            if (value.set != 0 || value.binding >= 5 || proof.resources[value.binding])
                throw InvalidEvidence{};
            proof.resources[value.binding] = w[2];
        }

        void StorageBlock(Proof &proof, const Node &block, std::size_t index) {
            constexpr std::array strides{64u, 96u, 16u, 4u};
            if (block.members.size() != 1 || block.offsets != std::vector<std::uint32_t>{0} || block.readonlyMember != (index < 2))
                throw InvalidEvidence{};
            const auto &array = Id(proof, block.members[0]);
            if (array.stride != strides[index])
                throw InvalidEvidence{};
            if (index == 0 && Id(proof, array.element).offsets != std::vector<std::uint32_t>{0, 16, 28, 32, 48, 56})
                throw InvalidEvidence{};
            if (index == 1 && Id(proof, array.element).offsets != std::vector<std::uint32_t>{0})
                throw InvalidEvidence{};
        }

        void ValidateResource(Proof &proof, std::size_t index) {
            constexpr std::array names{"Lights", "Clusters", "Membership", "References", "Dispatch"};
            const auto &value = Id(proof, proof.resources[index]);
            const auto &pointer = Id(proof, value.type);
            const auto &block = Id(proof, pointer.element);
            if (value.name != names[index] || !block.block || pointer.storage != (index == 4 ? 2u : 12u))
                throw InvalidEvidence{};
            if (index != 4) {
                StorageBlock(proof, block, index);
                return;
            }
            if (block.offsets != std::vector<std::uint32_t>{0, 4, 8, 12} || block.members.size() != 4)
                throw InvalidEvidence{};
        }

        void Access(Proof &proof, Words w) {
            RequireSize(w, 5);
            auto &value = Id(proof, w[2]);
            const auto &base = Id(proof, w[3]);
            if (const auto &resultType = Id(proof, w[1]);
                !base.root || !resultType.pointer || resultType.storage != Id(proof, base.type).storage)
                throw InvalidEvidence{};
            ValueOperands(proof, w.subspan(4));
            value.root = base.root;
        }

        void Load(Proof &proof, Words w) {
            RequireSize(w, 4);
            if (Pointer(proof, w[2]) || !Id(proof, w[3]).root)
                throw InvalidEvidence{};
        }

        void Store(Proof &proof, Words w) {
            if (w.size() != 3)
                throw InvalidEvidence{};
            if (const auto root = Id(proof, w[1]).root; !root || (root != proof.resources[2] && root != proof.resources[3]))
                throw InvalidEvidence{};
            ValueOperands(proof, w.subspan(2));
        }

        void Copy(Proof &proof, Words w) {
            if (w.size() != 4)
                throw InvalidEvidence{};
            if (!Pointer(proof, w[2])) {
                ValueOperands(proof, w.subspan(3));
                return;
            }
            auto &value = Id(proof, w[2]);
            const auto &source = Id(proof, w[3]);
            if (!source.root || value.type != source.type)
                throw InvalidEvidence{};
            value.root = source.root;
        }

        void ExtendedValue(Proof &proof, Words w) {
            if (w.size() != 6 || w[3] != proof.extensionSet || w[4] != 66)
                throw InvalidEvidence{};
            ValueOperands(proof, w.subspan(5));
        }

        void Scalar(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            if (Pointer(proof, w[2]))
                throw InvalidEvidence{};
            if (instruction.op == 12) {
                ExtendedValue(proof, w);
                return;
            }
            if (instruction.op == 79) {
                RequireSize(w, 5);
                ValueOperands(proof, w.subspan(3, 2));
                return;
            }
            if (instruction.op == 81) {
                RequireSize(w, 4);
                ValueOperands(proof, w.subspan(3, 1));
                return;
            }
            if (instruction.op >= 41 && instruction.op <= 46)
                return;
            ValueOperands(proof, w.subspan(3));
        }

        bool Metadata(std::uint32_t op) {
            constexpr std::array ops{3u,  5u,  6u,  11u, 14u, 15u, 16u, 17u,  19u,  20u,  21u,  22u,  23u,  28u, 29u,
                                     30u, 32u, 33u, 54u, 56u, 71u, 72u, 246u, 247u, 248u, 249u, 250u, 251u, 253u};
            return std::ranges::find(ops, op) != ops.end();
        }

        void ValidateInstruction(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            if (instruction.op == 59) {
                Resource(proof, w);
                return;
            }
            if (instruction.op == 65 || instruction.op == 66) {
                Access(proof, w);
                return;
            }
            if (instruction.op == 61) {
                Load(proof, w);
                return;
            }
            if (instruction.op == 62) {
                Store(proof, w);
                return;
            }
            if (instruction.op == 83) {
                Copy(proof, w);
                return;
            }
            if (ScalarResult(instruction.op)) {
                Scalar(proof, instruction);
                return;
            }
            if (!Metadata(instruction.op))
                throw InvalidEvidence{};
        }
    }  // namespace

    void ProveLightCullingSpirv(std::span<const std::uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > 1024 * 1024 || bytes.size() % 4)
            throw InvalidEvidence{};
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        auto proof = Decode(words);
        for (const auto &instruction : proof.instructions)
            if (instruction.op == 59)
                Resource(proof, instruction.words);
        for (std::size_t index = 0; index < 5; ++index)
            ValidateResource(proof, index);
        // Variables are already indexed; avoid admitting duplicate resources on the executable pass.
        for (const auto &instruction : proof.instructions)
            if (instruction.op != 59)
                ValidateInstruction(proof, instruction);
    }
}  // namespace Horo::Render::LightCullingMetalCookDetail
