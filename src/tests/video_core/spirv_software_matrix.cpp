// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using namespace Shader;

struct TensorMode {
    std::string previous;
    bool existed{};

    explicit TensorMode(const char* mode) {
        if (const char* value = std::getenv("EDEN_NX2_TENSOR")) {
            previous = value;
            existed = true;
        }
        Set(mode);
    }

    static void Set(const char* value) {
#ifdef _WIN32
        _putenv_s("EDEN_NX2_TENSOR", value ? value : "");
#else
        if (value) {
            setenv("EDEN_NX2_TENSOR", value, 1);
        } else {
            unsetenv("EDEN_NX2_TENSOR");
        }
#endif
    }

    ~TensorMode() {
        Set(existed ? previous.c_str() : nullptr);
    }
};

struct TensorProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit TensorProgram(u32 count = 1, u32 software_depth = 0, bool store_output = true,
                           bool gpu_fixture = false) {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.uses_subgroup_shuffles = count != 0 && software_depth != 0;
        program.info.uses_subgroup_invocation_id = count != 0;
        profile.supported_spirv = 0x00010500;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
        profile.max_subgroup_size = 32;
        IR::IREmitter ir{block};
        if (count == 0) {
            return;
        }
        const auto lane = ir.LaneId();
        IR::U32 global;
        if (gpu_fixture) {
            program.workgroup_size = {128, 1, 1};
            program.info.uses_workgroup_id = true;
            program.info.uses_local_invocation_id = true;
            program.info.used_storage_buffer_types = IR::Type::U32;
            program.info.storage_buffers_descriptors = {{0, 0, 1, false}, {0, 16, 1, true}};
            global = ir.IAdd(ir.ShiftLeftLogical(ir.WorkgroupIdX(), ir.Imm32(7)),
                             ir.LocalInvocationIdX());
        }
        std::array<IR::U32, 8> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            if (gpu_fixture) {
                const auto offset = ir.IAdd(ir.ShiftLeftLogical(global, ir.Imm32(5)), ir.Imm32(i * 4));
                inputs[i] = IR::U32{&*block.PrependNewInst(block.end(), IR::Opcode::LoadStorage32,
                                                        {ir.Imm32(0), offset})};
            } else {
                inputs[i] = software_depth ? ir.IAdd(ir.Imm32(0x3c003c00U + i * 0x01000100U),
                ir.ShiftLeftLogical(lane, ir.Imm32(3))) : ir.IAdd(ir.Imm32((i + 1) * 0x01000100U), lane);
            }
        }
        IR::U32 shared_offset;
        if (gpu_fixture) {
            shared_offset = ir.ShiftLeftLogical(ir.LocalInvocationIdX(), ir.Imm32(2));
            block.AppendNewInst(IR::Opcode::WriteSharedU32, {shared_offset, inputs[0]});
        }
        for (u32 i = 0; i < count; ++i) {
            const auto result = ir.TensorMulAdd(
                                       ir.CompositeConstruct(inputs[0], inputs[1], inputs[2], inputs[3]),
                                       ir.CompositeConstruct(inputs[4], inputs[5]),
                                       ir.CompositeConstruct(inputs[6], inputs[7]), software_depth);
            inputs[6] = IR::U32{ir.CompositeExtract(result, 0)};
            inputs[7] = IR::U32{ir.CompositeExtract(result, 1)};
        }
        if (software_depth && store_output) {
            program.info.used_storage_buffer_types = IR::Type::U32;
            if (!gpu_fixture) program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
            program.info.uses_local_invocation_id = true;
            const auto offset = ir.ShiftLeftLogical(gpu_fixture ? global : ir.LocalInvocationIdX(), ir.Imm32(3));
            if (gpu_fixture) {
                const IR::U32 saved{&*block.PrependNewInst(block.end(), IR::Opcode::LoadSharedU32,
                                                        {shared_offset})};
                inputs[6] = ir.BitwiseXor(inputs[6], saved);
            }
            const auto output_binding = ir.Imm32(gpu_fixture ? 1 : 0);
            block.AppendNewInst(IR::Opcode::WriteStorage32, {output_binding, offset, inputs[6]});
            block.AppendNewInst(IR::Opcode::WriteStorage32,
                {output_binding, ir.IAdd(offset, ir.Imm32(4)), inputs[7]});
        }
    }

    std::vector<u32> Emit(std::string_view name = {}) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EDEN_TENSOR_TEST_OUTPUT");
            directory && !name.empty()) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == opcode) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + size);
        }
        offset += size;
    }
    return result;
}

bool HasCapability(const std::vector<u32>& code, u32 capability) {
    const auto capabilities = Instructions(code, 17);
    return std::ranges::any_of(capabilities, [=](const auto& words) {
        return words[0] == capability;
    });
}

namespace Op {
constexpr u32 ExtInst = 12, Constant = 43, Function = 54, FunctionParameter = 55, Load = 61,
              Store = 62, CompositeExtract = 81, Select = 169, IEqual = 170, ULessThan = 176,
              LoopMerge = 246, Label = 248, GroupNonUniformShuffle = 345;
constexpr u32 RoundEven = 2, Fma = 50;
} // namespace Op

/// The instructions of a module in order, with each result's defining instruction.
struct Module {
    struct Inst {
        u32 opcode;
        std::vector<u32> words;
    };
    std::vector<Inst> insts;
    std::map<u32, size_t> defs;

    explicit Module(const std::vector<u32>& code) {
        for (size_t offset = 5; offset < code.size();) {
            const u32 size = code[offset] >> 16;
            REQUIRE(size != 0);
            const u32 opcode = code[offset] & 0xffffU;
            insts.push_back({opcode, {code.begin() + offset + 1, code.begin() + offset + size}});
            offset += size;
        }
        // OpTypeVoid..OpTypeFunction declare types; an instruction whose first operand is a type
        // defines a typed result next. Debug, mode and decoration instructions (whose first
        // operand can equal a type id) are skipped.
        std::vector<u32> types;
        for (const auto& inst : insts) {
            if (inst.opcode >= 19 && inst.opcode <= 33) {
                types.push_back(inst.words[0]);
            }
        }
        for (size_t index = 0; index < insts.size(); ++index) {
            const auto& inst = insts[index];
            const bool annotation =
                (inst.opcode >= 2 && inst.opcode <= 33 && inst.opcode != Op::ExtInst) ||
                inst.opcode == 71 || inst.opcode == 72 || inst.opcode == 332 ||
                inst.opcode == 5632;
            if (inst.opcode == Op::Label) {
                defs.emplace(inst.words[0], index);
            } else if (!annotation && inst.words.size() >= 2 &&
                       std::ranges::find(types, inst.words[0]) != types.end()) {
                defs.emplace(inst.words[1], index);
            }
        }
    }

    const Inst* Def(u32 id) const {
        const auto it = defs.find(id);
        return it == defs.end() ? nullptr : &insts[it->second];
    }

    /// Ids an instruction reads (literal operands skipped where the opcode has them).
    static std::vector<u32> Operands(const Inst& inst) {
        switch (inst.opcode) {
        case Op::Constant:
        case Op::FunctionParameter:
        case 59: // OpVariable
            return {};
        case Op::CompositeExtract:
            return {inst.words[2]};
        case Op::ExtInst:
            return {inst.words.begin() + 4, inst.words.end()};
        default:
            return {inst.words.begin() + 2, inst.words.end()};
        }
    }

    bool IsExtInst(const Inst& inst, u32 number) const {
        return inst.opcode == Op::ExtInst && inst.words[3] == number;
    }

    /// Whether `id` is computed from an OpExtInst `number`, through values only (not memory).
    bool DependsOn(u32 id, u32 number) const {
        std::vector<u32> pending{id};
        std::vector<u32> seen;
        while (!pending.empty()) {
            const u32 current = pending.back();
            pending.pop_back();
            if (std::ranges::find(seen, current) != seen.end()) {
                continue;
            }
            seen.push_back(current);
            const Inst* const inst = Def(current);
            if (!inst || inst->opcode == Op::Load) {
                continue;
            }
            if (IsExtInst(*inst, number)) {
                return true;
            }
            for (const u32 operand : Operands(*inst)) {
                pending.push_back(operand);
            }
        }
        return false;
    }

    size_t IndexOf(const Inst& inst) const {
        return static_cast<size_t>(&inst - insts.data());
    }
};

/// Where a software matrix operand comes from: parameter (0 = A, 1 = B) and component.
std::pair<u32, u32> OperandSource(const Module& module, u32 id,
                                  const std::vector<u32>& parameters) {
    const auto* const extract = module.Def(id);
    REQUIRE(extract);
    REQUIRE(extract->opcode == Op::CompositeExtract);
    const auto parameter = std::ranges::find(parameters, extract->words[2]);
    REQUIRE(parameter != parameters.end());
    return {static_cast<u32>(parameter - parameters.begin()), extract->words[3]};
}

}

TEST_CASE("Software HMMA shares one helper per depth without tensor extensions",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram single{1, depth};
        const auto first = single.Emit(depth == 8 ? "software-k8" : "software-k16");
        REQUIRE_FALSE(HasCapability(first, 5357));
        REQUIRE(Instructions(first, 246).size() == (depth == 16 ? 1U : 0U));
        REQUIRE(Instructions(first, 54).size() == 2); // OpFunction: main and the helper
        REQUIRE(Instructions(first, 57).size() == 1); // OpFunctionCall
        TensorProgram repeated{64, depth};
        const auto many = repeated.Emit(depth == 8 ? "software-k8-many" : "software-k16-many");
        REQUIRE(Instructions(many, 57).size() == 64);
        // Three fragment-vector constructions, the call and two result extracts
        // take at most 40 words per site; the matrix helper itself is never duplicated.
        REQUIRE(many.size() < first.size() + 64 * 40);
    }
}

namespace {
struct HelperCounts {
    u32 fma{};
    u32 shuffles{};
    u32 quad_broadcasts{};
    u32 conversions{};
    std::vector<u32> fma_before_rounding; ///< FMAs emitted before each RoundEven
    std::vector<u32> quad_indices;
    std::vector<u32> conversion_ids;
};

HelperCounts CountHelper(const std::vector<u32>& code) {
    std::map<u32, u32> constants;
    for (const auto& words : Instructions(code, 43)) { // OpConstant
        constants.emplace(words[1], words[2]);
    }
    HelperCounts counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 length = code[offset] >> 16;
        const u32 opcode = code[offset] & 0xffffU;
        REQUIRE(length != 0);
        REQUIRE(offset + length <= code.size());
        const auto words = std::span{code}.subspan(offset + 1, length - 1);
        if (opcode == 12 && words[3] == 2) { // GLSL.std.450 RoundEven
            counts.fma_before_rounding.push_back(counts.fma);
        } else if (opcode == 12 && words[3] == 50) { // GLSL.std.450 Fma
            ++counts.fma;
        } else if (opcode == 345) { // OpGroupNonUniformShuffle
            ++counts.shuffles;
        } else if (opcode == 365) { // OpGroupNonUniformQuadBroadcast
            ++counts.quad_broadcasts;
            REQUIRE(constants.contains(words[4]));
            counts.quad_indices.push_back(constants.at(words[4]));
        } else if (opcode == 115) { // OpFConvert
            ++counts.conversions;
            counts.conversion_ids.push_back(words[1]);
        }
        offset += length;
    }
    return counts;
}
} // namespace

TEST_CASE("Software HMMA rounds once per K8 group", "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool quad : {false, true}) {
            TensorProgram fixture{1, depth};
            fixture.profile.support_quad_shuffles = quad;
            const auto code = fixture.Emit();
            std::map<u32, u32> constants;
            for (const auto& words : Instructions(code, 43)) { // OpConstant
                constants.emplace(words[1], words[2]);
            }
            REQUIRE(Instructions(code, 247).empty()); // OpSelectionMerge: no lane-dependent branch
            REQUIRE(HasCapability(code, 68) == quad); // GroupNonUniformQuad
            // Only K16 needs a loop; K8 emits the group directly.
            const auto limits = Instructions(code, 176); // OpULessThan
            REQUIRE(limits.size() == (depth == 16 ? 1U : 0U));
            if (depth == 16) REQUIRE(constants.at(limits.front()[3]) == 2);
            const HelperCounts counts = CountHelper(code);
            // Four K pairs x 2 rows x 2 columns x 2 halves, then the four accumulators rounded.
            REQUIRE(counts.fma == 32);
            REQUIRE(counts.fma_before_rounding == std::vector<u32>(4, 32));
            // B always needs a general shuffle (two columns); A comes from its own quad.
            REQUIRE(counts.shuffles == (quad ? 8U : 16U));
            REQUIRE(counts.quad_broadcasts == (quad ? 8U : 0U));
            for (u32 i = 0; i < counts.quad_indices.size(); ++i) {
                REQUIRE(counts.quad_indices[i] == i / 2);
            }
        }
    }
}

TEST_CASE("Software HMMA rounds through a half when the shader has one",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.program.info.uses_fp16 = true;
        const auto code = fixture.Emit();
        const HelperCounts counts = CountHelper(code);
        REQUIRE(counts.fma_before_rounding.empty());
        // Narrow and widen each of the four accumulators, both kept exact.
        REQUIRE(counts.conversions == 8);
        const auto decorations = Instructions(code, 71); // OpDecorate
        for (const u32 id : counts.conversion_ids) {
            REQUIRE(std::ranges::any_of(decorations, [=](const auto& words) {
                return words[0] == id && words[1] == 42; // NoContraction
            }));
        }
    }
}

TEST_CASE("Software HMMA rounds once per uniform K8 group", "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        const auto code = fixture.Emit();
        std::map<u32, u32> constants;
        for (const auto& words : Instructions(code, 43)) {
            constants.emplace(words[1], words[2]);
        }
        // One K=8 group is emitted once; K=16 runs it twice from a uniform counter.
        const auto loops = Instructions(code, 246); // OpLoopMerge
        if (depth == 8) {
            REQUIRE(loops.empty());
        } else {
            REQUIRE(loops.size() == 1);
            const auto limits = Instructions(code, 176); // OpULessThan
            REQUIRE(limits.size() == 1);
            REQUIRE(constants.at(limits.front()[3]) == 2);
            const auto loads = Instructions(code, 61); // OpLoad
            const auto counter_load = std::ranges::find_if(
                loads, [&](const auto& words) { return words[1] == limits.front()[2]; });
            REQUIRE(counter_load != loads.end());
            const u32 counter = (*counter_load)[2];
            u32 counter_stores = 0;
            for (const auto& store : Instructions(code, 62)) { // OpStore
                if (store[0] != counter) {
                    continue;
                }
                ++counter_stores;
                if (constants.contains(store[1])) {
                    REQUIRE(constants.at(store[1]) == 0);
                }
            }
            REQUIRE(counter_stores == 2); // Zero, then the increment: no lane input.
        }
        u32 rounding_operations = 0;
        u32 fma_operations = 0;
        for (const auto& words : Instructions(code, 12)) { // OpExtInst
            rounding_operations += words[3] == 2;          // GLSL.std.450 RoundEven
            fma_operations += words[3] == 50;              // GLSL.std.450 Fma
        }
        REQUIRE(rounding_operations == 4); // One per accumulator, after each K=8 group.
        REQUIRE(fma_operations == 32);
        REQUIRE(Instructions(code, 345).size() == 16); // OpGroupNonUniformShuffle
        REQUIRE(Instructions(code, 247).empty());      // No per-pair rounding branch.
    }
}

TEST_CASE("Software HMMA stores each group's rounded accumulators and reads the right halves",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        CAPTURE(depth);
        TensorProgram fixture{1, depth};
        const Module module{fixture.Emit()};
        const auto& insts = module.insts;

        // The helper: the DontInline function with three parameters (A, B, C).
        const auto function = std::ranges::find_if(insts, [](const Module::Inst& inst) {
            return inst.opcode == Op::Function && (inst.words[2] & 0x2) != 0;
        });
        REQUIRE(function != insts.end());
        std::vector<u32> parameters;
        for (auto it = function + 1; it != insts.end() && it->opcode == Op::FunctionParameter;
             ++it) {
            parameters.push_back(it->words[1]);
        }
        REQUIRE(parameters.size() == 3);

        // The group body: the loop body for K=16, the whole helper for K=8.
        size_t body_begin = module.IndexOf(*function);
        size_t body_end = insts.size();
        const auto loop = std::ranges::find_if(
            insts, [](const Module::Inst& inst) { return inst.opcode == Op::LoopMerge; });
        u32 group = 0;
        if (depth == 16) {
            REQUIRE(loop != insts.end());
            const u32 continue_label = loop->words[1];
            body_begin = module.IndexOf(*loop);
            body_end = module.defs.at(continue_label);
            REQUIRE(body_begin < body_end);
            const auto limit = std::ranges::find_if(
                insts, [](const Module::Inst& inst) { return inst.opcode == Op::ULessThan; });
            REQUIRE(limit != insts.end());
            group = limit->words[2];
            REQUIRE(module.Def(group)->opcode == Op::Load);
        } else {
            REQUIRE(loop == insts.end());
        }
        const auto in_body = [&](size_t index) { return index > body_begin && index < body_end; };

        size_t last_fma = 0;
        u32 roundings = 0, fmas = 0, shuffles = 0;
        std::vector<std::pair<std::pair<u32, u32>, std::pair<u32, u32>>> halves;
        for (size_t index = 0; index < insts.size(); ++index) {
            const auto& inst = insts[index];
            if (module.IsExtInst(inst, Op::RoundEven)) {
                CHECK(in_body(index));
                ++roundings;
            } else if (module.IsExtInst(inst, Op::Fma)) {
                CHECK(in_body(index));
                last_fma = (std::max)(last_fma, index);
                ++fmas;
            } else if (inst.opcode == Op::GroupNonUniformShuffle) {
                CHECK(in_body(index));
                ++shuffles;
                const u32 value = inst.words[3];
                if (depth == 8) {
                    // One group: always the low halves.
                    const auto source = OperandSource(module, value, parameters);
                    halves.push_back({source, source});
                    continue;
                }
                // K=16: the group counter picks the high half in the second iteration.
                const auto* const select = module.Def(value);
                REQUIRE(select);
                REQUIRE(select->opcode == Op::Select);
                const auto* const condition = module.Def(select->words[2]);
                REQUIRE(condition);
                REQUIRE(condition->opcode == Op::IEqual);
                const u32 compared = condition->words[2] == group ? condition->words[3]
                                                                  : condition->words[2];
                CHECK((condition->words[2] == group || condition->words[3] == group));
                const auto* const one = module.Def(compared);
                REQUIRE(one);
                REQUIRE(one->opcode == Op::Constant);
                CHECK(one->words[2] == 1);
                halves.push_back({OperandSource(module, select->words[3], parameters),
                                  OperandSource(module, select->words[4], parameters)});
            }
        }
        CHECK(roundings == 4);
        CHECK(fmas == 32);
        CHECK(shuffles == 16);

        // A shuffles read A[row] then A[row + 2]; B shuffles read B[0] then B[1].
        using Source = std::pair<u32, u32>;
        const std::vector<std::pair<Source, Source>> allowed =
            depth == 8 ? std::vector<std::pair<Source, Source>>{{{0, 0}, {0, 0}},
                                                                {{0, 1}, {0, 1}},
                                                                {{1, 0}, {1, 0}}}
                       : std::vector<std::pair<Source, Source>>{{{0, 2}, {0, 0}},
                                                                {{0, 3}, {0, 1}},
                                                                {{1, 1}, {1, 0}}};
        for (const auto& pair : halves) {
            CHECK(std::ranges::find(allowed, pair) != allowed.end());
        }
        for (const auto& expected : allowed) {
            CHECK(std::ranges::count(halves, expected) == (expected.first.first == 0 ? 4 : 8));
        }

        // After the group's last FMA, every accumulator is stored rounded inside the body.
        std::vector<u32> rounded_targets;
        std::vector<u32> fma_targets;
        for (size_t index = body_begin + 1; index < body_end; ++index) {
            const auto& inst = insts[index];
            if (inst.opcode != Op::Store) {
                continue;
            }
            if (module.DependsOn(inst.words[1], Op::RoundEven)) {
                CHECK(index > last_fma);
                rounded_targets.push_back(inst.words[0]);
            } else if (module.DependsOn(inst.words[1], Op::Fma)) {
                fma_targets.push_back(inst.words[0]);
            }
        }
        std::ranges::sort(rounded_targets);
        CHECK(std::ranges::adjacent_find(rounded_targets) == rounded_targets.end());
        CHECK(rounded_targets.size() == 4);
        for (const u32 target : rounded_targets) {
            CHECK(std::ranges::find(fma_targets, target) != fma_targets.end());
        }
    }
}

TEST_CASE("Software matrix helpers also emit for 64-lane host subgroups",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.profile.max_subgroup_size = 64;
        fixture.profile.warp_size_potentially_larger_than_guest = true;
        const auto code = fixture.Emit(depth == 8 ? "software-k8-wave64" : "software-k16-wave64");
        REQUIRE_FALSE(HasCapability(code, 5357));
        REQUIRE(Instructions(code, 246).size() == (depth == 16 ? 1U : 0U));
        REQUIRE(Instructions(code, 345).size() == 16); // OpGroupNonUniformShuffle
    }
}

TEST_CASE("Software matrix broadcasts require a supported 32-lane compute pipeline",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool quad : {false, true}) {
            for (const u32 required_size : {0U, 32U, 64U}) {
                for (const bool supported : {false, true}) {
                    for (const bool preferred : {false, true}) {
                        TensorProgram fixture{1, depth};
                        fixture.profile.max_subgroup_size = 64;
                        fixture.profile.warp_size_potentially_larger_than_guest = true;
                        fixture.profile.required_compute_subgroup_size = required_size;
                        fixture.profile.support_subgroup_broadcast = supported;
                        fixture.profile.prefer_software_matrix_broadcast = preferred;
                        fixture.profile.support_quad_shuffles = quad;
                        const bool optimized = required_size == 32 && supported && preferred;
                        const auto code = fixture.Emit(
                            "software-guard-k" + std::to_string(depth) + "-quad" +
                            std::to_string(quad) + "-required" + std::to_string(required_size) +
                            "-supported" + std::to_string(supported) + "-preferred" +
                            std::to_string(preferred));
                        const auto counts = CountHelper(code);
                        CAPTURE(depth, quad, required_size, supported, preferred);
                        REQUIRE(Instructions(code, 337).size() == (optimized ? 32U : 0U));
                        REQUIRE(counts.shuffles == (quad ? 8U : 16U) - (optimized ? 8U : 0U));
                        REQUIRE(counts.quad_broadcasts == (quad ? 8U : 0U));
                        REQUIRE(HasCapability(code, 64) == optimized);
                        REQUIRE(HasCapability(code, 65)); // GroupNonUniformShuffle
                        REQUIRE(HasCapability(code, 68) == quad);
                        REQUIRE(counts.fma == 32);
                        REQUIRE(counts.fma_before_rounding == std::vector<u32>(4, 32));
                        REQUIRE(Instructions(code, 246).size() == (depth == 16 ? 1U : 0U));
                        REQUIRE(Instructions(code, 247).empty());
                    }
                }
            }
        }
    }
}

namespace {

constexpr u32 OP_COMPOSITE_EXTRACT = 81;
constexpr u32 OP_COMPOSITE_INSERT = 82;
constexpr u32 OP_BITCAST = 124;
constexpr u32 OP_TYPE_COOPERATIVE_MATRIX = 4456;
constexpr u32 OP_COOPERATIVE_MATRIX_LOAD = 4457;
constexpr u32 OP_COOPERATIVE_MATRIX_STORE = 4458;
constexpr u32 OP_COOPERATIVE_MATRIX_MUL_ADD = 4459;
constexpr u32 CAPABILITY_COOPERATIVE_MATRIX = 6022;
constexpr u32 CAPABILITY_FLOAT16 = 9;
constexpr u32 CAPABILITY_VULKAN_MEMORY_MODEL = 5345;
constexpr u32 CAPABILITY_VULKAN_MEMORY_MODEL_DEVICE_SCOPE = 5346;

std::map<u32, u32> Constants(const std::vector<u32>& code) {
    std::map<u32, u32> constants;
    for (const auto& words : Instructions(code, 43)) { // OpConstant
        constants.emplace(words[1], words[2]);
    }
    return constants;
}

void RequireHmmaMatrixTypes(const std::vector<u32>& code, u32 columns = 8) {
    const auto constants = Constants(code);
    const auto types = Instructions(code, OP_TYPE_COOPERATIVE_MATRIX);
    REQUIRE(types.size() == 3);
    std::array<u32, 3> columns_by_use{};
    for (const auto& words : types) {
        REQUIRE(words.size() == 6);
        REQUIRE(constants.at(words[2]) == 3); // Subgroup scope
        REQUIRE(constants.at(words[3]) == 16);
        columns_by_use.at(constants.at(words[5])) = constants.at(words[4]);
    }
    REQUIRE(columns_by_use == std::array<u32, 3>{16, columns, columns}); // A, B, accumulator
}

// SPV_KHR_cooperative_matrix requires the Vulkan memory model, which spirv-val does not check.
void RequireVulkanMemoryModel(const std::vector<u32>& code) {
    REQUIRE(HasCapability(code, CAPABILITY_VULKAN_MEMORY_MODEL));
    REQUIRE(HasCapability(code, CAPABILITY_VULKAN_MEMORY_MODEL_DEVICE_SCOPE));
    const auto models = Instructions(code, 14); // OpMemoryModel
    REQUIRE(models.size() == 1);
    REQUIRE(models[0][1] == 3); // Vulkan
    bool extension = false;
    for (const auto& words : Instructions(code, 10)) { // OpExtension
        extension |= std::string{reinterpret_cast<const char*>(words.data())} ==
                     "SPV_KHR_vulkan_memory_model";
    }
    REQUIRE(extension == (code[1] < 0x00010500));
}

} // namespace

TEST_CASE("Cooperative HMMA replaces the K16 software helper", "[video_core][tensor][ampere]") {
    TensorProgram fixture{3, 16};
    fixture.profile.support_cooperative_hmma = true;
    const auto code = fixture.Emit("cooperative-k16");
    REQUIRE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX));
    REQUIRE(HasCapability(code, CAPABILITY_FLOAT16));
    RequireVulkanMemoryModel(code);
    REQUIRE(fixture.program.info.uses_cooperative_matrix);
    RequireHmmaMatrixTypes(code);
    REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_MUL_ADD).size() == 3);
    REQUIRE(Instructions(code, 246).empty()); // OpLoopMerge
    REQUIRE(Instructions(code, 57).empty());  // OpFunctionCall
    REQUIRE(Instructions(code, 345).empty()); // OpGroupNonUniformShuffle
    for (const auto& words : Instructions(code, 169)) { // OpSelect
        for (const auto& type : Instructions(code, OP_TYPE_COOPERATIVE_MATRIX)) {
            REQUIRE(words[0] != type[0]);
        }
    }
}

TEST_CASE("Software matrix broadcast routing preserves every B source lane",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        TensorProgram fixture{1, depth};
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.support_quad_shuffles = true;
        const auto code = fixture.Emit(depth == 8 ? "software-k8-broadcast" : "software-k16-broadcast");
        std::map<u32, std::vector<u32>> definitions;
        u32 lane_variable{};
        std::vector<u32> gathers;
        for (size_t offset = 5; offset < code.size();) {
            const u32 size = code[offset] >> 16;
            REQUIRE(size != 0);
            REQUIRE(offset + size <= code.size());
            const auto words = std::span{code}.subspan(offset, size);
            const auto opcode = static_cast<spv::Op>(words[0] & 0xffffU);
            bool has_result{}, has_type{};
            spv::HasResultAndType(opcode, &has_result, &has_type);
            if (has_result && has_type) {
                definitions.emplace(words[2], std::vector<u32>{words.begin(), words.end()});
            }
            if (opcode == spv::Op::OpDecorate && words[2] == 11 && words[3] == 41) {
                lane_variable = words[1];
            }
            if (opcode == spv::Op::OpExtInst && words[4] == 62) {
                const auto found = definitions.find(words[5]);
                if (found != definitions.end() &&
                    (found->second[0] & 0xffffU) == static_cast<u32>(spv::Op::OpSelect)) {
                    gathers.push_back(words[5]);
                }
            }
            offset += size;
        }
        REQUIRE(lane_variable != 0);
        REQUIRE(gathers.size() == 8);
        const auto broadcasts = Instructions(code, 337);
        REQUIRE(broadcasts.size() == 32);
        const auto constants = Instructions(code, 43);
        for (u32 i = 0; i < broadcasts.size(); ++i) {
            REQUIRE(broadcasts[i][3] == broadcasts.front()[3]);
            const auto constant = std::ranges::find_if(constants, [&](const auto& words) {
                return words[1] == broadcasts[i][4];
            });
            REQUIRE(constant != constants.end());
            const u32 gather = i / 4;
            CHECK((*constant)[2] == gather / 2 + (gather % 2) * 4 + (i % 4) * 8);
        }
        const auto evaluate = [&](auto&& self, u32 id, u32 lane) -> u32 {
            const auto& words = definitions.at(id);
            const auto arg = [&](size_t index) { return self(self, words[index], lane); };
            switch (static_cast<spv::Op>(words[0] & 0xffffU)) {
            case spv::Op::OpConstant: return words[3];
            case spv::Op::OpLoad:
                REQUIRE(words[3] == lane_variable);
                return lane;
            case spv::Op::OpBitwiseAnd: return arg(3) & arg(4);
            case spv::Op::OpIEqual: return arg(3) == arg(4);
            case spv::Op::OpULessThan: return arg(3) < arg(4);
            case spv::Op::OpSelect: return arg(3) ? arg(4) : arg(5);
            case spv::Op::OpGroupNonUniformBroadcast:
                REQUIRE((definitions.at(words[5])[0] & 0xffffU) ==
                        static_cast<u32>(spv::Op::OpConstant));
                return arg(5);
            default: FAIL("Unexpected B gather opcode " << (words[0] & 0xffffU));
            }
            return 0;
        };
        for (u32 lane = 0; lane < 32; ++lane) {
            for (u32 i = 0; i < gathers.size(); ++i) {
                CAPTURE(depth, lane, i);
                CHECK(evaluate(evaluate, gathers[i], lane) ==
                      8 * (lane & 3) + i / 2 + (i % 2) * 4);
            }
        }
        fixture.program.info.uses_fp16 = true;
        const auto fp16_code = fixture.Emit(depth == 8 ? "software-k8-broadcast-fp16"
                                                      : "software-k16-broadcast-fp16");
        const auto counts = CountHelper(fp16_code);
        REQUIRE(counts.fma == 32);
        REQUIRE(counts.conversions == 8);
        REQUIRE(counts.fma_before_rounding.empty());
        const auto decorations = Instructions(fp16_code, 71);
        for (const u32 id : counts.conversion_ids) {
            REQUIRE(std::ranges::any_of(decorations, [=](const auto& words) {
                return words[0] == id && words[1] == 42;
            }));
        }
        TensorProgram repeated{64, depth};
        repeated.profile = fixture.profile;
        const auto repeated_code = repeated.Emit(depth == 8 ? "software-k8-broadcast-many"
                                                           : "software-k16-broadcast-many");
        REQUIRE(Instructions(repeated_code, 337).size() == 32);
        REQUIRE(Instructions(repeated_code, 54).size() == 2);
        REQUIRE(Instructions(repeated_code, 57).size() == 64);
        REQUIRE(Instructions(repeated_code, 246).size() == (depth == 16 ? 1U : 0U));
        REQUIRE(repeated_code.size() < code.size() + 64 * 40);
    }
}

TEST_CASE("Software matrix compute broadcast policy leaves other stages unchanged",
          "[video_core][tensor][ampere]") {
    TensorProgram fixture{1, 16, false};
    fixture.program.stage = Stage::Fragment;
    fixture.program.info.uses_local_invocation_id = false;
    fixture.profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Fragment);
    fixture.profile.required_compute_subgroup_size = 32;
    fixture.profile.support_subgroup_broadcast = true;
    fixture.profile.prefer_software_matrix_broadcast = true;
    const auto code = fixture.Emit("software-fragment-fallback");
    REQUIRE(Instructions(code, 337).empty());
    REQUIRE(Instructions(code, 345).size() == 16);
}

TEST_CASE("Software matrix broadcast preference preserves subgroup admission checks",
          "[video_core][tensor][ampere]") {
    for (const bool broken : {false, true}) {
        TensorProgram fixture{1, 16};
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.has_broken_spirv_subgroup_shuffle = broken;
        if (!broken) {
            fixture.profile.supported_subgroup_stages = 0;
        }
        REQUIRE_THROWS_AS(fixture.Emit(), NotImplementedException);
    }
}

TEST_CASE("Software matrix shared B defaults to a separate explicit-layout block when supported",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const bool guest_shared : {false, true}) {
            for (const bool explicit_layout : {false, true}) {
                TensorProgram fixture{64, depth};
                fixture.profile.supported_spirv = 0x00010600;
                fixture.profile.required_compute_subgroup_size = 32;
                fixture.profile.support_subgroup_broadcast = true;
                fixture.profile.prefer_software_matrix_broadcast = true;
                fixture.profile.support_quad_shuffles = true;
                fixture.profile.support_explicit_workgroup_layout = explicit_layout;
                fixture.program.shared_memory_size = guest_shared ? 4096 : 0;
                const auto code = fixture.Emit("software-shared-k" + std::to_string(depth) +
                    "-guest" + std::to_string(guest_shared) + "-layout" +
                    std::to_string(explicit_layout));
                const auto barriers = Instructions(code, 224);
                REQUIRE(barriers.size() == (explicit_layout ? 2U : 0U));
                REQUIRE(Instructions(code, 337).size() == (explicit_layout ? 0U : 32U));
                REQUIRE(Instructions(code, 345).empty());
                REQUIRE(Instructions(code, 57).size() == 64);
                REQUIRE(CountHelper(code).fma == 32);
                if (!explicit_layout) continue;
                std::map<u32, u32> constants;
                for (const auto& words : Instructions(code, 43)) constants[words[1]] = words[2];
                for (const auto& words : barriers) {
                    REQUIRE(constants.at(words[0]) == 3);
                    REQUIRE(constants.at(words[1]) == 3);
                    REQUIRE(constants.at(words[2]) == 264);
                }
                const auto variables = Instructions(code, 59);
                const auto decorations = Instructions(code, 71);
                std::map<u32, u32> pointers, offsets;
                for (const auto& p : Instructions(code, 32)) pointers[p[0]] = p[2];
                for (const auto& m : Instructions(code, 72))
                    if (m[1] == 0 && m[2] == 35) offsets[m[0]] = m[3];
                u32 scratch{};
                for (const auto& words : variables) {
                    if (words[2] != 4) continue; // Workgroup
                    const bool aliased = std::ranges::any_of(decorations, [&](const auto& d) {
                        return d[0] == words[1] && d[1] == 20;
                    });
                    REQUIRE(aliased);
                    if (offsets.at(pointers.at(words[0])) == (guest_shared ? 4096U : 0U)) {
                        REQUIRE(scratch == 0);
                        scratch = words[1];
                    }
                }
                REQUIRE(scratch != 0);
                u32 loads{}, stores{};
                for (const auto& chain : Instructions(code, 65)) {
                    if (chain[2] != scratch) continue;
                    REQUIRE(chain.size() == 5); // block member zero, then array index
                    REQUIRE(constants.at(chain[3]) == 0);
                    for (const auto& load : Instructions(code, 61)) loads += load[2] == chain[1];
                    for (const auto& store : Instructions(code, 62)) stores += store[0] == chain[1];
                }
                REQUIRE(loads == 8);
                REQUIRE(stores == 1);
            }
        }
    }
}

TEST_CASE("Software matrix shared B rejects partial subgroups and uncertain call flow",
          "[video_core][tensor][ampere]") {
    for (u32 reason = 0; reason < 13; ++reason) {
        TensorProgram fixture{1, 16};
        fixture.profile.supported_spirv = 0x00010600;
        fixture.profile.required_compute_subgroup_size = 32;
        fixture.profile.support_subgroup_broadcast = true;
        fixture.profile.prefer_software_matrix_broadcast = true;
        fixture.profile.support_explicit_workgroup_layout = true;
        IR::Block preheader{fixture.pool}, merge{fixture.pool};
        switch (reason) {
        case 0: fixture.program.workgroup_size = {48, 1, 1}; break;
        case 1: fixture.profile.required_compute_subgroup_size = 64; break;
        case 2: fixture.profile.support_explicit_workgroup_layout = false; break;
        case 3: fixture.program.shared_memory_size = 16384; break;
        case 4: fixture.profile.prefer_software_matrix_broadcast = false; break;
        case 5: fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(),
                    {{}, IR::AbstractSyntaxNode::Type::Return}); break;
        case 6: fixture.program.syntax_list.insert(fixture.program.syntax_list.begin(),
                    {{}, IR::AbstractSyntaxNode::Type::Unreachable}); break;
        case 7: fixture.program.workgroup_size = {32, 0, 1}; break;
        case 8: fixture.profile.support_subgroup_broadcast = false; break;
        case 9:
            fixture.program.blocks = {&preheader, &fixture.block, &merge};
            fixture.program.post_order_blocks = fixture.program.blocks;
            fixture.program.syntax_list = {
                {{.block = &preheader}, IR::AbstractSyntaxNode::Type::Block},
                {{.if_node = {IR::U1{IR::Value{true}}, &fixture.block, &merge}}, IR::AbstractSyntaxNode::Type::If},
                {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
                {{.end_if = {&merge}}, IR::AbstractSyntaxNode::Type::EndIf},
                {{.block = &merge}, IR::AbstractSyntaxNode::Type::Block},
                {{}, IR::AbstractSyntaxNode::Type::Return}};
            break;
        case 10: fixture.profile.supported_spirv = 0x00010500; break;
        case 11:
            fixture.program.shared_memory_size = 6912;
            fixture.program.info.uses_int16 = true;
            fixture.profile.support_workgroup_layout_16bit_access = false;
            break;
        case 12: fixture.program.workgroup_size = {1056, 1, 1}; break;
        }
        // Invalid syntax/zero-sized workgroups are admission-only checks, not valid modules.
        const auto code = fixture.Emit();
        CAPTURE(reason);
        REQUIRE(Instructions(code, 224).empty());
        REQUIRE(Instructions(code, 337).size() == ((reason == 1 || reason == 4 || reason == 8) ? 0U : 32U));
    }
}

TEST_CASE("Software matrix source GPU fixtures preserve guest shared memory",
          "[video_core][tensor][ampere]") {
    for (const u32 depth : {8U, 16U}) {
        for (const u32 count : {1U, 192U}) {
            for (const bool fp16 : {false, true}) {
                for (const bool shared_b : {false, true}) {
                    TensorProgram fixture{count, depth, true, true};
                    fixture.profile.supported_spirv = 0x00010600;
                    fixture.profile.required_compute_subgroup_size = 32;
                    fixture.profile.support_subgroup_broadcast = true;
                    fixture.profile.prefer_software_matrix_broadcast = true;
                    fixture.profile.support_quad_shuffles = true;
                    fixture.profile.support_explicit_workgroup_layout = shared_b;
                    fixture.profile.support_workgroup_layout_16bit_access = true;
                    fixture.profile.support_int16 = true;
                    fixture.program.info.uses_int16 = true;
                    fixture.program.info.uses_fp16 = fp16;
                    fixture.program.shared_memory_size = 6912; // actual OoT shader guest allocation
                    const auto code = fixture.Emit("gpu-k" + std::to_string(depth) + "-n" +
                        std::to_string(count) + "-fp" + std::to_string(fp16) + "-shared" +
                        std::to_string(shared_b));
                    REQUIRE(Instructions(code, 224).size() == (shared_b ? 2U : 0U));
                    REQUIRE(Instructions(code, 337).size() == (shared_b ? 0U : 32U));
                    REQUIRE(CountHelper(code).shuffles == 0);
                }
            }
        }
    }
}

TEST_CASE("Cooperative HMMA keeps barriers valid under the Vulkan memory model",
          "[video_core][tensor][ampere]") {
    for (const u32 version : {0x00010400U, 0x00010600U}) {
        TensorProgram fixture{1, 16};
        fixture.profile.supported_spirv = version;
        fixture.profile.support_cooperative_hmma = true;
        IR::IREmitter ir{fixture.block};
        ir.Barrier();
        ir.WorkgroupMemoryBarrier();
        ir.DeviceMemoryBarrier();
        const auto code = fixture.Emit("cooperative-k16-barriers-" + std::to_string(version));
        REQUIRE(code[1] == version);
        RequireVulkanMemoryModel(code);
        REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_MUL_ADD).size() == 1);
    }
}

TEST_CASE("Cooperative HMMA keeps the driver's order of A's registers",
          "[video_core][tensor][ampere]") {
    TensorProgram fixture{1, 16};
    fixture.profile.support_cooperative_hmma = true;
    const auto code = fixture.Emit();
    const auto extracts = Instructions(code, OP_COMPOSITE_EXTRACT);
    const auto bitcasts = Instructions(code, OP_BITCAST);
    const auto by_result = [](const auto& instructions, u32 result) {
        const auto it = std::ranges::find_if(
            instructions, [=](const auto& words) { return words[1] == result; });
        REQUIRE(it != instructions.end());
        return *it;
    };
    const auto types = Instructions(code, OP_TYPE_COOPERATIVE_MATRIX);
    const auto constants = Constants(code);
    const auto matrix_a = std::ranges::find_if(
        types, [&](const auto& words) { return constants.at(words[5]) == 0; });
    REQUIRE(matrix_a != types.end());

    std::array<u32, 8> register_of_element{};
    u32 inserts = 0;
    for (const auto& words : Instructions(code, OP_COMPOSITE_INSERT)) {
        if (words[0] != (*matrix_a)[0]) {
            continue;
        }
        const auto half = by_result(extracts, words[2]);
        const auto halves = by_result(bitcasts, half[2]);
        const auto guest_register = by_result(extracts, halves[2]);
        REQUIRE(half[3] == (words[4] & 1));
        register_of_element.at(words[4]) = guest_register[3];
        ++inserts;
    }
    REQUIRE(inserts == 8);
    REQUIRE(register_of_element == std::array<u32, 8>{0, 0, 2, 2, 1, 1, 3, 3});
}

TEST_CASE("Cooperative HMMA falls back to the software helper", "[video_core][tensor][ampere]") {
    const auto require_software = [](TensorProgram& fixture) {
        const auto code = fixture.Emit();
        REQUIRE_FALSE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX));
        REQUIRE(Instructions(code, OP_TYPE_COOPERATIVE_MATRIX).empty());
        REQUIRE(Instructions(code, 57).size() == 1); // OpFunctionCall
        REQUIRE_FALSE(fixture.program.info.uses_cooperative_matrix);
    };
    SECTION("unsupported by the device") {
        TensorProgram fixture{1, 16};
        require_software(fixture);
    }
    SECTION("the K8 form") {
        TensorProgram fixture{1, 8};
        fixture.profile.support_cooperative_hmma = true;
        require_software(fixture);
    }
    SECTION("a workgroup that leaves a warp unfilled") {
        TensorProgram fixture{1, 16};
        fixture.profile.support_cooperative_hmma = true;
        fixture.program.workgroup_size = {48, 1, 1};
        require_software(fixture);
    }
    SECTION("a workgroup row that leaves a warp unfilled") {
        TensorProgram fixture{1, 16};
        fixture.profile.support_cooperative_hmma = true;
        fixture.program.workgroup_size = {16, 2, 1};
        require_software(fixture);
    }
    SECTION("coherent memory, which the Vulkan memory model has no decoration for") {
        TensorProgram fixture{1, 16};
        fixture.profile.support_cooperative_hmma = true;
        fixture.program.info.requires_coherent_global_memory = true;
        require_software(fixture);
    }
}

TEST_CASE("Cooperative HMMA self-test shader runs one lowering on one warp",
          "[video_core][tensor][ampere]") {
    for (const u32 version : {0x00010300U, 0x00010600U}) {
        const auto code = Backend::SPIRV::EmitCooperativeHmmaTest(version);
        REQUIRE(code[1] == version);
        REQUIRE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX));
        RequireVulkanMemoryModel(code);
        RequireHmmaMatrixTypes(code);
        REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_MUL_ADD).size() == 1);
        const auto modes = Instructions(code, 16); // OpExecutionMode
        REQUIRE(modes.size() == 1);
        REQUIRE(modes[0][1] == 17); // LocalSize
        REQUIRE(modes[0][2] == 32);
        if (const char* directory = std::getenv("EDEN_TENSOR_TEST_OUTPUT")) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} /
                                   ("cooperative-self-test-" + std::to_string(version) + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
        }
    }
}

TEST_CASE("Software matrix shared B admits game-like reconverged calls and suffix returns",
          "[video_core][tensor][ampere]") {
    TensorProgram fixture{192, 16};
    fixture.profile.supported_spirv = 0x00010600;
    fixture.profile.required_compute_subgroup_size = 32;
    fixture.profile.support_subgroup_broadcast = true;
    fixture.profile.prefer_software_matrix_broadcast = true;
    fixture.profile.support_explicit_workgroup_layout = true;
    IR::Block preheader{fixture.pool}, optional{fixture.pool}, suffix{fixture.pool}, exit{fixture.pool};
    fixture.program.blocks = {&preheader, &optional, &fixture.block, &suffix, &exit};
    fixture.program.post_order_blocks = fixture.program.blocks;
    fixture.program.syntax_list = {
        {{.block = &preheader}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {IR::U1{IR::Value{true}}, &optional, &fixture.block}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &optional}, IR::AbstractSyntaxNode::Type::Block},
        {{.end_if = {&fixture.block}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &fixture.block}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {IR::U1{IR::Value{true}}, &suffix, &exit}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &suffix}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
        {{.end_if = {&exit}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &exit}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return}};
    const auto code = fixture.Emit("software-shared-game-flow");
    REQUIRE(Instructions(code, 224).size() == 2);
    REQUIRE(Instructions(code, 57).size() == 192);
}

TEST_CASE("16x16x16 cooperative HMMA runs both depths through shared memory",
          "[video_core][tensor][ampere]") {
    for (const auto& [depth, version] : {std::pair{16U, 0x00010500U}, std::pair{16U, 0x00010600U},
                                        std::pair{8U, 0x00010600U}}) {
        TensorProgram fixture{3, depth};
        fixture.profile.supported_spirv = version;
        fixture.profile.support_cooperative_hmma_16x16x16 = true;
        const auto code = fixture.Emit("cooperative-16x16x16-k" + std::to_string(depth) + "-" +
                                       std::to_string(version));
        REQUIRE(code[1] == version);
        REQUIRE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX));
        REQUIRE(HasCapability(code, CAPABILITY_FLOAT16));
        RequireVulkanMemoryModel(code);
        RequireHmmaMatrixTypes(code, 16);
        REQUIRE(fixture.program.info.uses_cooperative_matrix);
        REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_LOAD).size() == 9);
        REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_MUL_ADD).size() == 3);
        REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_STORE).size() == 3);
        REQUIRE(Instructions(code, 202).empty()); // OpBitFieldUExtract
        REQUIRE(Instructions(code, 57).empty());  // OpFunctionCall
        REQUIRE(Instructions(code, 345).empty()); // OpGroupNonUniformShuffle
        REQUIRE(fixture.program.shared_memory_size == 2 * 384 * 4);
    }
}

TEST_CASE("16x16x16 cooperative HMMA falls back to the software helper",
          "[video_core][tensor][ampere]") {
    TensorProgram fixture{1, 16};
    fixture.profile.support_cooperative_hmma_16x16x16 = true;
    SECTION("a workgroup that leaves a warp unfilled") {
        fixture.program.workgroup_size = {48, 1, 1};
    }
    SECTION("a workgroup row that leaves a warp unfilled") {
        fixture.program.workgroup_size = {16, 2, 1};
    }
    SECTION("coherent memory, which the Vulkan memory model has no decoration for") {
        fixture.program.info.requires_coherent_image_memory = true;
    }
    SECTION("scratch past the shared memory limit") {
        fixture.profile.max_compute_shared_memory_size = 2 * 384 * 4 - 4;
    }
    const auto code = fixture.Emit();
    REQUIRE_FALSE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX));
    REQUIRE(Instructions(code, 57).size() == 1); // OpFunctionCall
    REQUIRE(fixture.program.shared_memory_size == 0);
    REQUIRE_FALSE(fixture.program.info.uses_cooperative_matrix);
}

TEST_CASE("Cooperative HMMA and guarded software matrix optimizations coexist",
          "[video_core][tensor][ampere]") {
    for (const bool shared_cooperative : {false, true}) {
        for (const u32 depth : {8U, 16U}) {
            CAPTURE(shared_cooperative, depth);
            TensorProgram fixture{1, depth};
            fixture.profile.supported_spirv = 0x00010600;
            fixture.profile.required_compute_subgroup_size = 32;
            fixture.profile.support_cooperative_hmma = !shared_cooperative;
            fixture.profile.support_cooperative_hmma_16x16x16 = shared_cooperative;
            fixture.profile.support_subgroup_broadcast = true;
            fixture.profile.prefer_software_matrix_broadcast = true;
            fixture.profile.support_quad_shuffles = true;
            fixture.profile.support_explicit_workgroup_layout = true;
            const auto code = fixture.Emit();
            const bool hardware = shared_cooperative || depth == 16;
            REQUIRE(HasCapability(code, CAPABILITY_COOPERATIVE_MATRIX) == hardware);
            REQUIRE(Instructions(code, OP_COOPERATIVE_MATRIX_MUL_ADD).size() ==
                    (hardware ? 1U : 0U));
            REQUIRE(Instructions(code, 57).size() == (hardware ? 0U : 1U));
            REQUIRE(Instructions(code, 337).empty());
            REQUIRE(Instructions(code, 345).empty());
            if (!hardware) {
                REQUIRE(Instructions(code, 224).size() == 2);
                REQUIRE(CountHelper(code).quad_broadcasts == 8);
            }
        }
    }
}

TEST_CASE("Captured Bananza matrix shaders lower without an environment override",
          "[video_core][tensor][ampere]") {
    TensorMode mode{nullptr};
    for (const Ampere::Instruction instruction : {
             Ampere::Instruction{{0x102c723c, 0x30, 0x8ff, 0x04aff000}},
             Ampere::Instruction{{0x1020723c, 0x24, 0x8ff, 0x04aff000}},
             Ampere::Instruction{{0x1418723c, 0x1c, 0x8ff, 0x04aff000}}}) {
        const std::array code{instruction,
                              Ampere::Instruction{{0x794d, 0, 0x03800000, 0}}};
        const auto lowered = Ampere::LowerToMaxwell(code, false, true);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 2);
        REQUIRE(Maxwell::Decode(lowered.instructions[0]) == Maxwell::Opcode::TENSOR_MUL_ADD);
    }
}

TEST_CASE("Native matrix lowering selects compact software helpers without legacy toggles", "[video_core][tensor][ampere]") {
    TensorMode mode{nullptr};
    std::array<Ampere::Instruction, 2> code{{
        {{0x0810723c, 12, 0x8ff, 0}}, {{0x0000794d, 0, 0x03800000, 0}}}};
    const auto supported = Ampere::LowerToMaxwell(code, false, false);
    REQUIRE(static_cast<bool>(supported));
    REQUIRE(supported.instructions.size() > 100);
    const auto compact = Ampere::LowerToMaxwell(code, false, true);
    REQUIRE(compact);
    REQUIRE(compact.instructions.size() == 2);
    REQUIRE(Maxwell::Decode(compact.instructions.front()) == Maxwell::Opcode::TENSOR_MUL_ADD);
    REQUIRE((compact.instructions.front() & (1ULL << 48)) != 0);
    for (const char* legacy : {"fast", "stub", "off"}) {
        TensorMode::Set(legacy);
        REQUIRE(Ampere::LowerToMaxwell(code, false, true).instructions == compact.instructions);
        REQUIRE(Ampere::LowerToMaxwell(code).instructions == supported.instructions);
    }
    code[0].words[0] &= ~0x7000U;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    code[0].words[0] |= 0x7000;
    code[0].words[3] = 1;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
    code[0].words[3] = 0;
    code[0].words[0] += 1U << 24;
    REQUIRE_FALSE(static_cast<bool>(Ampere::LowerToMaxwell(code, false, true)));
}
