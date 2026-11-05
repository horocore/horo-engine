#include "ExposureGpuReflectionInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::ExposureGpuCookDetail {
    namespace {
        using Words = std::span<const std::uint32_t>;

        struct Node {
            std::uint32_t type{}, storage{}, element{}, root{}, binding{~0u}, set{~0u}, kind{}, width{}, count{}, stride{}, signedness{};
            bool pointer{}, readonly{};
            std::vector<std::uint32_t> members, offsets, image;
            std::string name;
        };

        struct Instruction {
            std::uint32_t op;
            Words w;
        };

        struct Proof {
            std::vector<Node> ids;
            std::vector<Instruction> instructions;
            std::array<std::uint32_t, 5> resources{};
            ExposureGpuStage stage;
            std::uint32_t entry{}, functions{}, localSizes{}, extensionSet{};
        };

        /** @brief Bounds every binary ID before reading proof state. */
        Node &Id(Proof &proof, std::uint32_t id) {
            if (!id || id >= proof.ids.size())
                throw EvidenceFailure{__func__, __LINE__};
            return proof.ids[id];
        }

        /** @brief Requires the fixed leading operands before indexing an instruction. */
        void Size(Words words, std::size_t minimum) {
            if (words.size() < minimum)
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Extracts only terminated bounded binary names. */
        std::string Text(Words words) {
            const auto *begin = reinterpret_cast<const char *>(words.data());
            const auto end = std::find(begin, begin + words.size_bytes(), '\0');
            if (end == begin + words.size_bytes())
                throw EvidenceFailure{__func__, __LINE__};
            return {begin, end};
        }

        /** @brief Rejects nested pointer aggregates as well as direct pointer values. */
        bool PointerType(Proof &proof, std::uint32_t type, std::uint32_t depth = 0) {
            if (depth > 16)
                throw EvidenceFailure{__func__, __LINE__};
            const auto &node = Id(proof, type);
            if (node.pointer)
                return true;
            if (node.element && PointerType(proof, node.element, depth + 1))
                return true;
            return std::ranges::any_of(node.members, [&](auto member) {
                return PointerType(proof, member, depth + 1);
            });
        }

        /** @brief Identifies pointer values from their actual declared SPIR-V result type. */
        bool Pointer(Proof &proof, std::uint32_t value) {
            const auto type = Id(proof, value).type;
            return type && PointerType(proof, type);
        }

        /** @brief Rejects any pointer escaping into a value-only instruction. */
        void Values(Proof &proof, Words operands) {
            for (const auto operand : operands)
                if (Pointer(proof, operand))
                    throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Indexes result types while forbidding duplicate binary definitions. */
        void Typed(Proof &proof, Words w) {
            Size(w, 3);
            auto &node = Id(proof, w[2]);
            if (node.type)
                throw EvidenceFailure{__func__, __LINE__};
            node.type = w[1];
            (void)Id(proof, w[1]);
        }

        /** @brief Admits the closed scalar/vector arithmetic used by the histogram family. */
        bool Scalar(std::uint32_t op) {
            if (op >= 109 && op <= 116)
                return true;
            if (op >= 124 && op <= 152)
                return true;
            if (op >= 154 && op <= 205)
                return true;
            constexpr std::array ops{1u,  12u, 41u, 42u, 43u, 44u,  45u,  46u,  77u,  78u,  79u,
                                     80u, 81u, 82u, 84u, 95u, 100u, 103u, 104u, 106u, 107u, 245u};
            return std::ranges::find(ops, op) != ops.end();
        }

        /** @brief Indexes module decorations needed to distinguish predecessor storage from writable resources. */
        void Decoration(Proof &proof, Words w) {
            Size(w, 3);
            auto &node = Id(proof, w[1]);
            if (w[2] == 33) {
                Size(w, 4);
                node.binding = w[3];
            }
            if (w[2] == 34) {
                Size(w, 4);
                node.set = w[3];
            }
            if (w[2] == 24)
                node.readonly = true;
            if (w[2] == 6) {
                Size(w, 4);
                node.stride = w[3];
            }
        }

        /** @brief Proves the selected binary compute entry identity. */
        void EntryPoint(Proof &proof, Words w) {
            Size(w, 4);
            if (proof.entry || w[1] != 5 || Text(w.subspan(3)) != EntryName(proof.stage))
                throw EvidenceFailure{__func__, __LINE__};
            proof.entry = w[2];
        }

        /** @brief Checks the exact closed native group size independently of JSON. */
        void LocalSize(Proof &proof, Words w) {
            const bool build = proof.stage == ExposureGpuStage::BuildHistogram;
            if (w.size() != 6 || w[2] != 17 || w[3] != (build ? 8u : 256u) || w[4] != (build ? 8u : 1u) || w[5] != 1)
                throw EvidenceFailure{__func__, __LINE__};
            ++proof.localSizes;
        }

        /** @brief Proves the exact entry and declared local group size. */
        void Entry(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op == 15) {
                EntryPoint(proof, w);
                return;
            }
            Size(w, 3);
            if (w[1] != proof.entry)
                throw EvidenceFailure{__func__, __LINE__};
            if (w[2] == 4461 && w.size() == 4 && w[3] == 32)
                return;
            LocalSize(proof, w);
        }

        /** @brief Restricts imported instructions to the standard arithmetic extension. */
        void Extension(Proof &proof, Words w) {
            Size(w, 3);
            if (proof.extensionSet || Text(w.subspan(2)) != "GLSL.std.450")
                throw EvidenceFailure{__func__, __LINE__};
            proof.extensionSet = w[1];
        }

        /** @brief Restricts the module to shader arithmetic without pointer extension capabilities. */
        void Module(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op == 17) {
                if (w.size() != 2 || (w[1] != 1 && w[1] != 4466))
                    throw EvidenceFailure{__func__, __LINE__};
            }
            if (instruction.op == 11)
                Extension(proof, w);
            if (instruction.op == 14)
                if (w.size() != 3 || w[1] != 0 || w[2] != 1)
                    throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Indexes exact primitive widths and member offsets from the validated module. */
        void Layout(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op == 21 || instruction.op == 22) {
                Size(w, instruction.op == 21 ? 4 : 3);
                auto &node = Id(proof, w[1]);
                node.width = w[2];
                if (instruction.op == 21)
                    node.signedness = w[3];
            }
            if (instruction.op != 72)
                return;
            Size(w, 4);
            auto &node = Id(proof, w[1]);
            if (w[2] >= 8)
                throw EvidenceFailure{__func__, __LINE__};
            if (w[3] == 24 && w[2] == 0)
                node.readonly = true;
            if (w[3] == 35) {
                Size(w, 5);
                node.offsets.resize(std::max(node.offsets.size(), std::size_t{w[2] + 1}), ~0u);
                node.offsets[w[2]] = w[4];
            }
        }

        /** @brief Indexes nested aggregate elements so pointer composites cannot escape the proof. */
        void Aggregate(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op >= 19 && instruction.op <= 33) {
                Size(w, 2);
                Id(proof, w[1]).kind = instruction.op;
            }
            if (instruction.op == 23) {
                Size(w, 4);
                Id(proof, w[1]).count = w[3];
            }
            if (instruction.op == 25) {
                Size(w, 9);
                Id(proof, w[1]).image.assign(w.begin() + 2, w.end());
            }
            if (instruction.op == 23 || instruction.op == 24 || instruction.op == 28 || instruction.op == 29) {
                Size(w, 3);
                Id(proof, w[1]).element = w[2];
            }
            if (instruction.op == 30) {
                Size(w, 2);
                Id(proof, w[1]).members.assign(w.begin() + 2, w.end());
            }
        }

        /** @brief Indexes pointer types and result definitions before any executable memory proof. */
        void Index(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            Module(proof, instruction);
            Aggregate(proof, instruction);
            Layout(proof, instruction);
            if (instruction.op == 32) {
                Size(w, 4);
                auto &node = Id(proof, w[1]);
                node.pointer = true;
                node.storage = w[2];
                node.element = w[3];
            }
            if (instruction.op == 5) {
                Size(w, 3);
                Id(proof, w[1]).name = Text(w.subspan(2));
            }
            if (instruction.op == 71)
                Decoration(proof, w);
            if (instruction.op == 15 || instruction.op == 16)
                Entry(proof, instruction);
            constexpr std::array typed{59u,  61u,  65u,  66u,  83u,  227u, 229u, 230u, 231u, 232u,
                                       233u, 234u, 235u, 236u, 237u, 238u, 239u, 240u, 241u, 242u};
            if (Scalar(instruction.op) || std::ranges::find(typed, instruction.op) != typed.end())
                Typed(proof, w);
        }

        /** @brief Records exact set-zero resource variables without accepting duplicate register evidence. */
        void GlobalVariable(Proof &proof, Words w, Node &node) {
            if (w[3] != 0 && w[3] != 2 && w[3] != 12)
                throw EvidenceFailure{__func__, __LINE__};
            if (w.size() != 4 || node.set != 0 || node.binding >= 5 || proof.resources[node.binding])
                throw EvidenceFailure{__func__, __LINE__};
            proof.resources[node.binding] = w[2];
        }

        /** @brief Admits actual invocation/workgroup/function variables or exact canonical global resource slots. */
        void Variable(Proof &proof, Words w) {
            Size(w, 4);
            auto &node = Id(proof, w[2]);
            const auto &type = Id(proof, w[1]);
            if (!type.pointer || type.storage != w[3])
                throw EvidenceFailure{__func__, __LINE__};
            node.root = w[2];
            node.storage = w[3];
            if (w[3] == 1 || w[3] == 4 || w[3] == 7)
                return;
            GlobalVariable(proof, w, node);
        }

        /** @brief Proves a complete 32-bit scalar or vector element without inferring types from its name. */
        void Primitive(Proof &proof, std::uint32_t id, bool floating, std::uint32_t columns) {
            const auto &node = Id(proof, id);
            if (columns != 1) {
                if (node.kind != 23 || node.count != columns)
                    throw EvidenceFailure{__func__, __LINE__};
                id = node.element;
            }
            const auto &scalar = Id(proof, id);
            if (scalar.kind != (floating ? 22u : 21u) || scalar.width != 32 || (!floating && scalar.signedness))
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Proves the exact binary exposure-state member types and byte offsets. */
        void StateLayout(Proof &proof, std::uint32_t id) {
            const auto &state = Id(proof, id);
            if (state.kind != 30 || state.members.size() != 6 || state.offsets != std::vector<std::uint32_t>{0, 4, 8, 12, 16, 24})
                throw EvidenceFailure{__func__, __LINE__};
            for (std::size_t i = 0; i < 6; ++i)
                Primitive(proof, state.members[i], i < 2, i >= 4 ? 2u : 1u);
        }

        /** @brief Proves the complete six-register binary dispatch layout. */
        void DispatchLayout(Proof &proof, const Node &block) {
            if (block.kind != 30 || block.members.size() != 6 || block.offsets != std::vector<std::uint32_t>{0, 16, 32, 48, 64, 80})
                throw EvidenceFailure{__func__, __LINE__};
            for (std::size_t i = 0; i < 6; ++i)
                Primitive(proof, block.members[i], i >= 1 && i <= 3, 4);
        }

        /** @brief Requires exact unsized storage-array strides and predecessor readonly decoration. */
        void StorageLayout(Proof &proof, const Node &block, std::size_t index) {
            if (block.kind != 30 || block.members.size() != 1 || block.offsets != std::vector<std::uint32_t>{0})
                throw EvidenceFailure{__func__, __LINE__};
            if (index == 2 && !block.readonly)
                throw EvidenceFailure{__func__, __LINE__};
            const auto &array = Id(proof, block.members[0]);
            if (array.kind != 29 || array.stride != (index == 1 ? 4u : 32u))
                throw EvidenceFailure{__func__, __LINE__};
            if (index == 1)
                Primitive(proof, array.element, false, 1);
            else
                StateLayout(proof, array.element);
        }

        /** @brief Accepts nondepth or unspecified depth classification only for the exact sampled float2D shape.
         * @details Unspecified depth never establishes a native resource kind: admission additionally requires the
         * actual Cross texture2D declaration and the actual selected MSL float texture2d readonly argument.
         */
        bool ImageShape(const Node &image) {
            return image.image[1] == 1 && (image.image[2] == 0 || image.image[2] == 2) && image.image[3] == 0 && image.image[4] == 0 &&
                   image.image[5] == 1 && image.image[6] == 0;
        }

        /** @brief Proves each resource's binary block ABI as well as its reflected name and register. */
        void ResourceLayout(Proof &proof, const Node &resource, std::size_t index) {
            const auto &pointer = Id(proof, resource.type);
            const auto &block = Id(proof, pointer.element);
            if (index == 0) {
                if (block.kind != 25 || block.image.size() != 7)
                    throw EvidenceFailure{__func__, __LINE__};
                Primitive(proof, block.image[0], true, 1);
                if (!ImageShape(block))
                    throw EvidenceFailure{__func__, __LINE__};
                return;
            }
            if (index == 4)
                DispatchLayout(proof, block);
            else
                StorageLayout(proof, block, index);
        }

        /** @brief Requires all and only the selected stage resources, including their real storage classes. */
        void Resources(Proof &proof) {
            const auto names = ExposureGpuMetalReflectionNames(proof.stage);
            std::array<bool, 5> used{};
            for (const auto &name : names) {
                const auto i = name.id.value - 1;
                used[i] = true;
                const auto &node = Id(proof, proof.resources[i]);
                const auto expected = i == 0 ? 0u : i == 4 ? 2u : 12u;
                if (node.name != (i == 4 ? "Dispatch" : name.name) || node.storage != expected)
                    throw EvidenceFailure{__func__, __LINE__};
                ResourceLayout(proof, node, i);
            }
            for (std::size_t i = 0; i < 5; ++i)
                if (!used[i] && proof.resources[i])
                    throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Tracks every derived pointer back to its actual source storage variable. */
        void Access(Proof &proof, Words w) {
            Size(w, 5);
            auto &result = Id(proof, w[2]);
            const auto &base = Id(proof, w[3]);
            const auto &type = Id(proof, result.type);
            if (!base.root || !type.pointer || type.storage != Id(proof, base.type).storage)
                throw EvidenceFailure{__func__, __LINE__};
            Values(proof, w.subspan(4));
            result.root = base.root;
        }

        /** @brief Prohibits stores and atomics into the predecessor, source image, dispatch or input variables. */
        void Writable(Proof &proof, std::uint32_t pointer) {
            const auto root = Id(proof, pointer).root;
            if (!root)
                throw EvidenceFailure{__func__, __LINE__};
            const auto &node = Id(proof, root);
            if (node.storage == 4 || node.storage == 7)
                return;
            if (root != proof.resources[1] && root != proof.resources[3])
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Allows direct loads only, with no pointer-valued load or opaque pointer alias. */
        void Load(Proof &proof, Words w) {
            Size(w, 4);
            if (Pointer(proof, w[2]) || !Id(proof, w[3]).root)
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Preserves tracked roots only for exact same-type pointer copies. */
        void Copy(Proof &proof, Words w) {
            if (w.size() != 4)
                throw EvidenceFailure{__func__, __LINE__};
            if (!Pointer(proof, w[2])) {
                Values(proof, w.subspan(3));
                return;
            }
            auto &result = Id(proof, w[2]);
            const auto &source = Id(proof, w[3]);
            if (!source.root || result.type != source.type)
                throw EvidenceFailure{__func__, __LINE__};
            result.root = source.root;
        }

        /** @brief Validates all scalar operands without interpreting composite literal indices as IDs. */
        void ScalarOperands(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (Pointer(proof, w[2]))
                throw EvidenceFailure{__func__, __LINE__};
            if (instruction.op >= 41 && instruction.op <= 46)
                return;
            if (instruction.op == 81) {
                Size(w, 4);
                Values(proof, w.subspan(3, 1));
                return;
            }
            if (instruction.op == 79 || instruction.op == 82) {
                Size(w, 5);
                Values(proof, w.subspan(3, 2));
                return;
            }
            if (instruction.op == 12) {
                Size(w, 6);
                if (w[3] != proof.extensionSet)
                    throw EvidenceFailure{__func__, __LINE__};
                Values(proof, w.subspan(5));
                return;
            }
            Values(proof, w.subspan(3));
        }

        /** @brief Permits atomic integer updates only on independently proven writable storage. */
        void Atomic(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op == 228) {
                Size(w, 5);
                Writable(proof, w[1]);
                Values(proof, w.subspan(2));
                return;
            }
            Size(w, 6);
            Writable(proof, w[3]);
            if (Pointer(proof, w[2]))
                throw EvidenceFailure{__func__, __LINE__};
            Values(proof, w.subspan(4));
        }

        /** @brief Enumerates non-memory declarations and structured control flow; unknown instructions fail closed. */
        bool Metadata(std::uint32_t op) {
            if (op >= 19 && op <= 33)
                return true;
            constexpr std::array ops{0u,  3u,  4u,   5u,   6u,   7u,   8u,   10u,  11u,  14u,  15u,  16u,  17u,  54u, 56u,
                                     71u, 72u, 224u, 225u, 246u, 247u, 248u, 249u, 250u, 251u, 253u, 255u, 317u, 330u};
            return std::ranges::find(ops, op) != ops.end();
        }

        /** @brief Checks single-entry inlined function boundaries before proving its memory instructions. */
        void Function(Proof &proof, const Instruction &instruction) {
            if (instruction.op != 54)
                return;
            Size(instruction.w, 5);
            if (++proof.functions != 1 || instruction.w[2] != proof.entry)
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Handles all admitted direct memory instructions before value-only instruction checks. */
        bool Memory(Proof &proof, const Instruction &instruction) {
            const auto w = instruction.w;
            if (instruction.op == 59)
                return true;
            if (instruction.op == 65 || instruction.op == 66) {
                Access(proof, w);
                return true;
            }
            if (instruction.op == 61) {
                Load(proof, w);
                return true;
            }
            if (instruction.op == 62) {
                if (w.size() != 3)
                    throw EvidenceFailure{__func__, __LINE__};
                Writable(proof, w[1]);
                Values(proof, w.subspan(2));
                return true;
            }
            if (instruction.op == 83) {
                Copy(proof, w);
                return true;
            }
            if (instruction.op >= 227 && instruction.op <= 242) {
                Atomic(proof, instruction);
                return true;
            }
            return false;
        }

        /** @brief Proves every executable operation; unsupported pointer-producing operations fail closed. */
        void Execute(Proof &proof, const Instruction &instruction) {
            if (Memory(proof, instruction))
                return;
            if (Scalar(instruction.op)) {
                ScalarOperands(proof, instruction);
                return;
            }
            if (!Metadata(instruction.op))
                throw EvidenceFailure{__func__, __LINE__};
            Function(proof, instruction);
        }

        /** @brief Checks complete bounded instruction records before indexing any operand. */
        void Instructions(Proof &proof, Words words) {
            for (std::size_t at = 5; at < words.size();) {
                const auto count = words[at] >> 16;
                if (!count || count > words.size() - at || proof.instructions.size() == 65536)
                    throw EvidenceFailure{__func__, __LINE__};
                proof.instructions.push_back({words[at] & 65535, words.subspan(at, count)});
                at += count;
            }
        }

        /** @brief Parses only bounded whole validated modules without retaining borrowed byte storage. */
        Proof Decode(Words words, ExposureGpuStage stage) {
            Size(words, 5);
            if (words[0] != 0x07230203 || words[3] < 2 || words[3] > 65536 || words[4])
                throw EvidenceFailure{__func__, __LINE__};
            Proof proof;
            proof.stage = stage;
            proof.ids.resize(words[3]);
            Instructions(proof, words);
            for (const auto &instruction : proof.instructions)
                Index(proof, instruction);
            if (!proof.entry || proof.localSizes != 1)
                throw EvidenceFailure{__func__, __LINE__};
            return proof;
        }
    }  // namespace

    /** @copydoc ProveSpirv */
    void ProveSpirv(std::span<const std::uint8_t> bytes, ExposureGpuStage stage) {
        if (bytes.empty() || bytes.size() > 1024 * 1024 || bytes.size() % 4)
            throw EvidenceFailure{__func__, __LINE__};
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        auto proof = Decode(words, stage);
        for (const auto &instruction : proof.instructions)
            if (instruction.op == 59)
                Variable(proof, instruction.w);
        Resources(proof);
        for (const auto &instruction : proof.instructions)
            Execute(proof, instruction);
        if (proof.functions != 1)
            throw EvidenceFailure{__func__, __LINE__};
    }
}  // namespace Horo::Render::ExposureGpuCookDetail
