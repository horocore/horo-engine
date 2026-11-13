#include "VfxGpuSpirvEvidence.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace Horo::Render::VfxGpuCookDetail {
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
            std::uint32_t kind{};
            std::uint32_t width{};
            std::uint32_t count{};
            std::uint32_t signedness{};
            bool pointer{};
            bool block{};
            bool readonlyMember{};
            std::vector<std::uint32_t> members;
            std::vector<std::uint32_t> offsets;
            std::vector<std::uint32_t> major;
            std::vector<std::uint32_t> matrixStride;
            std::vector<std::string> memberNames;
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
            VfxGpuProgramRole role;
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
            const auto bytes = std::as_bytes(words);
            const auto end = std::ranges::find(bytes, std::byte{0});
            if (end == bytes.end())
                throw InvalidEvidence{};
            std::string text(static_cast<std::size_t>(end - bytes.begin()), '\0');
            std::transform(bytes.begin(), end, text.begin(), [](std::byte value) {
                return std::to_integer<char>(value);
            });
            return text;
        }

        bool PointerType(Proof &proof, std::uint32_t type, std::uint32_t depth = 0) {
            if (depth > 16)
                throw InvalidEvidence{};
            const auto &node = Id(proof, type);
            if (node.pointer)
                return true;
            if (node.element && PointerType(proof, node.element, depth + 1))
                return true;
            return std::ranges::any_of(node.members, [&](auto member) {
                return PointerType(proof, member, depth + 1);
            });
        }

        bool Pointer(Proof &proof, std::uint32_t value) {
            const auto type = Id(proof, value).type;
            return type && PointerType(proof, type);
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

        void MatrixDecoration(Node &node, Words w) {
            RequireSize(w, 4);
            if (w[3] != 4 && w[3] != 5 && w[3] != 7)
                return;
            auto &values = w[3] == 7 ? node.matrixStride : node.major;
            values.resize(std::max(values.size(), std::size_t{w[2] + 1}), 0);
            const auto value = w[3] == 7 ? (RequireSize(w, 5), w[4]) : w[3];
            if (values[w[2]] && values[w[2]] != value)
                throw InvalidEvidence{};
            values[w[2]] = value;
        }

        void MemberDecoration(Proof &proof, Words w) {
            RequireSize(w, 4);
            auto &node = Id(proof, w[1]);
            if (w[2] >= 8)
                throw InvalidEvidence{};
            if (w[3] == 24 && w[2] == 0)
                node.readonlyMember = true;
            MatrixDecoration(node, w);
            if (w[3] != 35)
                return;
            RequireSize(w, 5);
            node.offsets.resize(std::max(node.offsets.size(), std::size_t{w[2] + 1}), ~0u);
            node.offsets[w[2]] = w[4];
        }

        void Primitive(Node &node, const Instruction &instruction) {
            const auto w = instruction.words;
            if (instruction.op == 21 || instruction.op == 22) {
                RequireSize(w, instruction.op == 21 ? 4 : 3);
                node.width = w[2];
                if (instruction.op == 21)
                    node.signedness = w[3];
            }
            if (instruction.op == 23 || instruction.op == 24 || instruction.op == 28) {
                RequireSize(w, 4);
                node.element = w[2];
                node.count = w[3];
            }
        }

        void Type(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            RequireSize(w, 2);
            auto &node = Id(proof, w[1]);
            node.kind = instruction.op;
            Primitive(node, instruction);
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
            if (proof.entry || w[1] != (Compute(proof.role) ? 5u : 0u) ||
                Text(w.subspan(3)) != EntryName(proof.role, Compute(proof.role) ? ShaderStage::Compute : ShaderStage::Vertex))
                throw InvalidEvidence{};
            proof.entry = w[2];
        }

        bool FloatMode(Words w) {
            return w[2] == 4461 && w.size() == 4 && w[3] == 32;
        }

        void Execution(Proof &proof, Words w) {
            RequireSize(w, 3);
            if (w[1] != proof.entry)
                throw InvalidEvidence{};
            if (FloatMode(w))
                return;
            if (!Compute(proof.role) || w[2] != 17)
                throw InvalidEvidence{};
            if (w.size() != 6 || w[3] != 64 || w[4] != 1 || w[5] != 1)
                throw InvalidEvidence{};
            ++proof.localSizes;
        }

        bool ScalarResult(std::uint32_t op) {
            constexpr std::array ops{1u, 12u, 41u, 42u, 43u, 44u, 46u, 77u, 78u, 79u, 80u, 81u, 82u, 83u, 84u, 245u};
            return (op >= 109 && op <= 124) || (op >= 126 && op <= 200) || std::ranges::find(ops, op) != ops.end();
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
            if (instruction.op == 14 && (w.size() != 3 || w[1] != 0 || w[2] != 1))
                throw InvalidEvidence{};
            if (instruction.op == 17)
                Capability(w);
        }

        void IndexName(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.words;
            if (instruction.op == 5) {
                RequireSize(w, 3);
                Id(proof, w[1]).name = Text(w.subspan(2));
            }
            if (instruction.op == 6) {
                RequireSize(w, 4);
                if (w[2] >= 8)
                    throw InvalidEvidence{};
                auto &names = Id(proof, w[1]).memberNames;
                names.resize(std::max(names.size(), std::size_t{w[2] + 1}));
                names[w[2]] = Text(w.subspan(3));
            }
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
            IndexName(proof, instruction);
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
            if (!proof.entry || proof.functions != 1 || proof.localSizes != (Compute(proof.role) ? 1u : 0u) || proof.functionOpen ||
                !proof.functionEnded)
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

        Proof Decode(Words words, VfxGpuProgramRole role) {
            RequireSize(words, 5);
            if (words[0] != 0x07230203 || words[3] < 2 || words[3] > 65536 || words[4])
                throw InvalidEvidence{};
            Proof proof;
            proof.role = role;
            proof.ids.resize(words[3]);
            Instructions(proof, words);
            for (const auto &instruction : proof.instructions)
                IndexInstruction(proof, instruction);
            for (const auto &instruction : proof.instructions)
                IndexMemory(proof, instruction);
            CompleteFunction(proof);
            return proof;
        }

        bool LocalStorage(std::uint32_t storage) {
            constexpr std::array local{1u, 3u, 6u, 7u};
            return std::ranges::find(local, storage) != local.end();
        }

        void BindResource(Proof &proof, Words w) {
            const auto &value = Id(proof, w[2]);
            if (const auto count = ResourceNames(proof.role).size();
                w.size() != 4 || (w[3] != 12 && w[3] != 2) || value.set != 0 || value.binding >= count || proof.resources[value.binding])
                throw InvalidEvidence{};
            proof.resources[value.binding] = w[2];
        }

        void Resource(Proof &proof, Words w) {
            RequireSize(w, 4);
            auto &value = Id(proof, w[2]);
            if (const auto &pointer = Id(proof, w[1]); !pointer.pointer || pointer.storage != w[3] || PointerType(proof, pointer.element))
                throw InvalidEvidence{};
            value.root = w[2];
            if (w.size() == 5)
                ValueOperands(proof, w.subspan(4));
            if (w.size() != 4 && w.size() != 5)
                throw InvalidEvidence{};
            if (!LocalStorage(w[3]))
                BindResource(proof, w);
        }

        bool Uint(Proof &proof, std::uint32_t type) {
            const auto &node = Id(proof, type);
            return node.kind == 21 && node.width == 32 && node.signedness == 0;
        }

        bool Vector(Proof &proof, std::uint32_t type, bool integer) {
            const auto &node = Id(proof, type);
            const auto &scalar = Id(proof, node.element);
            const bool element = integer ? Uint(proof, node.element) : scalar.kind == 22 && scalar.width == 32;
            return node.kind == 23 && node.count == 4 && element;
        }

        void StorageBlock(Proof &proof, const Node &block, std::size_t index) {
            if (const bool readonly = !Compute(proof.role) || index == 0 || index == 2;
                block.members.size() != 1 || block.offsets != std::vector<std::uint32_t>{0} || block.readonlyMember != readonly)
                throw InvalidEvidence{};
            const auto &array = Id(proof, block.members[0]);
            if (array.kind != 29 || array.stride != 4 || !Uint(proof, array.element))
                throw InvalidEvidence{};
        }

        void MatrixField(Proof &proof, const Node &block, std::uint32_t type) {
            if (const auto &matrix = Id(proof, type); matrix.kind != 24 || matrix.count != 4 || !Vector(proof, matrix.element, false))
                throw InvalidEvidence{};
            if (block.major.empty() || (block.major[0] != 4 && block.major[0] != 5) || block.matrixStride.empty() ||
                block.matrixStride[0] != 16)
                throw InvalidEvidence{};
        }

        void UniformField(Proof &proof, const Node &block, std::size_t index) {
            const auto type = block.members[index];
            if (!Compute(proof.role) && index == 0) {
                MatrixField(proof, block, type);
                return;
            }
            if (!Vector(proof, type, Compute(proof.role) || index == 4))
                throw InvalidEvidence{};
        }

        void UniformBlock(Proof &proof, const Node &block) {
            const auto names = UniformNames(proof.role);
            if (const std::vector<std::uint32_t> offsets = Compute(proof.role) ? std::vector<std::uint32_t>{0, 16, 32, 48, 64, 80}
                                                                               : std::vector<std::uint32_t>{0, 64, 80, 96, 112};
                block.offsets != offsets || block.members.size() != names.size() || block.memberNames.size() != names.size())
                throw InvalidEvidence{};
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (block.memberNames[i] != names[i])
                    throw InvalidEvidence{};
                UniformField(proof, block, i);
            }
        }

        void ValidateResource(Proof &proof, std::size_t index) {
            const auto names = ResourceNames(proof.role);
            const bool uniform = index + 1 == names.size();
            const auto &value = Id(proof, proof.resources[index]);
            const auto &pointer = Id(proof, value.type);
            const auto &block = Id(proof, pointer.element);
            if (value.name != names[index] || !block.block || pointer.storage != (uniform ? 2u : 12u))
                throw InvalidEvidence{};
            if (uniform)
                UniformBlock(proof, block);
            else
                StorageBlock(proof, block, index);
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
            const auto &base = Id(proof, w[3]);
            const auto &pointer = Id(proof, base.type);
            if (Pointer(proof, w[2]) || !base.root || !pointer.pointer || pointer.element != w[1])
                throw InvalidEvidence{};
        }

        bool WritableRoot(Proof &proof, std::uint32_t root) {
            const auto &node = Id(proof, root);
            if (const auto storage = Id(proof, node.type).storage; storage == 3 || storage == 6 || storage == 7)
                return true;
            return Compute(proof.role) && (root == proof.resources[1] || root == proof.resources[3]);
        }

        void Store(Proof &proof, Words w) {
            if (w.size() != 3)
                throw InvalidEvidence{};
            if (const auto root = Id(proof, w[1]).root; !root || !WritableRoot(proof, root))
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

        /** @brief Admits the captured strict-float NMin instruction only as two same-type float32 scalar values. */
        void ScalarMinimum(Proof &proof, Words w) {
            if (w.size() != 7 || w[3] != proof.extensionSet)
                throw InvalidEvidence{};
            if (const auto &type = Id(proof, w[1]); type.kind != 22 || type.width != 32)
                throw InvalidEvidence{};
            for (const auto operand : w.subspan(5))
                if (Id(proof, operand).type != w[1] || Pointer(proof, operand))
                    throw InvalidEvidence{};
        }

        void ExtendedValue(Proof &proof, Words w) {
            RequireSize(w, 6);
            if (w[4] == 79) {
                ScalarMinimum(proof, w);
                return;
            }
            if (constexpr std::array pure{4u, 13u, 14u, 31u, 37u, 38u, 39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u, 48u, 49u, 50u, 66u};
                w[3] != proof.extensionSet || std::ranges::find(pure, w[4]) == pure.end())
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
            if (instruction.op == 82) {
                RequireSize(w, 5);
                ValueOperands(proof, w.subspan(3, 2));
                return;
            }
            if (instruction.op == 81) {
                RequireSize(w, 4);
                ValueOperands(proof, w.subspan(3, 1));
                return;
            }
            if (instruction.op == 41 || instruction.op == 42 || instruction.op == 43 || instruction.op == 46)
                return;
            ValueOperands(proof, w.subspan(3));
        }

        bool Metadata(std::uint32_t op) {
            constexpr std::array ops{3u,  5u,  6u,  11u, 14u, 15u, 16u, 17u, 19u,  20u,  21u,  22u,  23u,  24u,  28u,
                                     29u, 30u, 32u, 33u, 54u, 56u, 71u, 72u, 246u, 247u, 248u, 249u, 250u, 251u, 253u};
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

        BinaryEvidence Evidence(Proof &proof) {
            const auto count = ResourceNames(proof.role).size();
            const auto &variable = Id(proof, proof.resources[count - 1]);
            const auto &uniform = Id(proof, Id(proof, variable.type).element);
            BinaryEvidence evidence;
            evidence.rowMajor = !Compute(proof.role) && uniform.major[0] == 4;
            for (std::size_t index = 0; index < count; ++index) {
                const auto &resource = Id(proof, proof.resources[index]);
                evidence.blockNames.push_back(Id(proof, Id(proof, resource.type).element).name);
                evidence.memberNames.push_back(Id(proof, Id(proof, resource.type).element).memberNames);
            }
            return evidence;
        }
    }  // namespace

    BinaryEvidence ProveSpirv(std::span<const std::uint8_t> bytes, VfxGpuProgramRole role) {
        if (bytes.empty() || bytes.size() > 1024 * 1024 || bytes.size() % 4)
            throw InvalidEvidence{};
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        auto proof = Decode(words, role);
        for (const auto &instruction : proof.instructions)
            if (instruction.op == 59)
                Resource(proof, instruction.words);
        const auto count = ResourceNames(role).size();
        for (std::size_t index = 0; index < count; ++index)
            ValidateResource(proof, index);
        for (const auto &instruction : proof.instructions)
            if (instruction.op != 59)
                ValidateInstruction(proof, instruction);
        return Evidence(proof);
    }
}  // namespace Horo::Render::VfxGpuCookDetail
