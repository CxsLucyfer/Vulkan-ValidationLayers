/* Copyright (c) 2015-2023 The Khronos Group Inc.
 * Copyright (c) 2015-2023 Valve Corporation
 * Copyright (c) 2015-2023 LunarG, Inc.
 * Copyright (C) 2015-2023 Google Inc.
 * Modifications Copyright (C) 2020 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

#include "generated/vk_enum_string_helper.h"
#include "core_validation.h"
#include "generated/spirv_grammar_helper.h"
#include "utils/shader_utils.h"

bool CoreChecks::ValidateInterfaceVertexInput(const PIPELINE_STATE &pipeline, const SHADER_MODULE_STATE &module_state,
                                              const EntryPoint &entrypoint) const {
    bool skip = false;
    safe_VkPipelineVertexInputStateCreateInfo const *vi = pipeline.vertex_input_state->input_state;

    struct AttribInputPair {
        const VkFormat *attribute_input = nullptr;
        const Instruction *shader_input = nullptr;
    };
    // For vertex input, we only need to care about Location.
    // You are not allowed to offset into the Component words
    // or have 2 variables in a location
    std::map<uint32_t, AttribInputPair> location_map;

    if (vi) {
        for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
            // Vertex input attributes use VkFormat, but only to make use of how they define sizes, things such as
            // depth/multi-plane/compressed will never be used here because they would mean nothing. So we can ensure these are
            // "standard" color formats being used
            const VkFormat format = vi->pVertexAttributeDescriptions[i].format;
            const uint32_t format_size = FormatElementSize(format);
            // Vulkan Spec: Location is made up of 16 bytes, never can have 0 Locations
            const uint32_t bytes_in_location = 16;
            const uint32_t num_locations = ((format_size - 1) / bytes_in_location) + 1;
            for (uint32_t j = 0; j < num_locations; ++j) {
                location_map[vi->pVertexAttributeDescriptions[i].location + j].attribute_input =
                    &(vi->pVertexAttributeDescriptions[i].format);
            }
        }
    }

    for (const auto *variable_ptr : entrypoint.user_defined_interface_variables) {
        const auto &variable = *variable_ptr;
        if ((variable.storage_class != spv::StorageClassInput)) {
            continue;  // not an input interface
        }
        // It is possible to have a struct block for the vertex input.
        // All members of struct must all have Locations or none of them will.
        // If the interface variable doesn't have the Locations, find them inside the struct members
        if (!variable.type_struct_info) {
            for (const auto &slot : variable.interface_slots) {
                location_map[slot.Location()].shader_input = &variable.base_type;
            }
        } else if (variable.decorations.location != DecorationSet::kInvalidValue) {
            // Variable is decorated with Location
            uint32_t location = variable.decorations.location;
            for (uint32_t i = 0; i < variable.type_struct_info->members.size(); i++) {
                const auto &member = variable.type_struct_info->members[i];
                // can be 64-bit formats in the struct
                const uint32_t num_locations = module_state.GetLocationsConsumedByType(member.id);
                for (uint32_t j = 0; j < num_locations; ++j) {
                    location_map[location + j].shader_input = member.insn;
                }
                location += num_locations;
            }
        } else {
            // Can't be nested so only need to look at first level of members
            for (const auto &member : variable.type_struct_info->members) {
                location_map[member.decorations->location].shader_input = member.insn;
            }
        }
    }

    for (const auto &location_it : location_map) {
        const auto location = location_it.first;
        const auto attribute_input = location_it.second.attribute_input;
        const auto shader_input = location_it.second.shader_input;

        if (attribute_input && !shader_input) {
            skip |= LogPerformanceWarning(module_state.vk_shader_module(), kVUID_Core_Shader_OutputNotConsumed,
                                          "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                          "] Vertex attribute at location %" PRIu32 " not consumed by vertex shader",
                                          pipeline.create_index, location);
        } else if (!attribute_input && shader_input) {
            skip |= LogError(module_state.vk_shader_module(), "VUID-VkGraphicsPipelineCreateInfo-Input-07904",
                             "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                             "] Vertex shader consumes input at location %" PRIu32 " but not provided",
                             pipeline.create_index, location);
        } else if (attribute_input && shader_input) {
            const VkFormat attribute_format = *attribute_input;
            const auto attribute_type = GetFormatType(attribute_format);
            const uint32_t var_base_type_id = shader_input->ResultId();
            const auto var_numeric_type = module_state.GetNumericType(var_base_type_id);

            // Type checking
            if (!(attribute_type & var_numeric_type)) {
                skip |= LogError(module_state.vk_shader_module(), "VUID-VkGraphicsPipelineCreateInfo-Input-08733",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                 "] Attribute type of `%s` at location %" PRIu32 " does not match vertex shader input type of `%s`",
                                 pipeline.create_index, string_VkFormat(attribute_format), location,
                                 module_state.DescribeType(var_base_type_id).c_str());
            } else {
                // 64-bit can't be used if both the Vertex Attribute AND Shader Input Variable are both not 64-bit.
                const bool attribute64 = FormatIs64bit(attribute_format);
                const bool shader64 = module_state.GetBaseTypeInstruction(var_base_type_id)->GetBitWidth() == 64;
                if (attribute64 && !shader64) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-VkGraphicsPipelineCreateInfo-pVertexInputState-08929",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] Attribute at location %" PRIu32
                                     " is a 64-bit format (%s) but vertex shader input is 32-bit type (%s)",
                                     pipeline.create_index, location, string_VkFormat(attribute_format),
                                     module_state.DescribeType(var_base_type_id).c_str());
                } else if (!attribute64 && shader64) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-VkGraphicsPipelineCreateInfo-pVertexInputState-08930",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] Attribute at location %" PRIu32
                                     " is a not a 64-bit format (%s) but vertex shader input is 64-bit type (%s)",
                                     pipeline.create_index, location, string_VkFormat(attribute_format),
                                     module_state.DescribeType(var_base_type_id).c_str());
                } else if (attribute64 && shader64) {
                    // Unlike 32-bit, the components for 64-bit inputs have to match exactly
                    const uint32_t attribute_components = FormatComponentCount(attribute_format);
                    const uint32_t input_components = module_state.GetNumComponentsInBaseType(shader_input);
                    if (attribute_components != input_components) {
                        skip |= LogError(module_state.vk_shader_module(), "UNASSIGNED-VkGraphicsPipelineCreateInfo-Component-64bit",
                                         "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] Attribute at location %" PRIu32
                                         " is a %" PRIu32 "-wide 64-bit format (%s) but vertex shader input is %" PRIu32
                                         "-wide 64-bit type (%s), 64-bit vertex input don't have default values and require "
                                         "matching components.",
                                         pipeline.create_index, location, attribute_components, string_VkFormat(attribute_format),
                                         input_components, module_state.DescribeType(var_base_type_id).c_str());
                    }
                }
            }
        } else {            // !attrib && !input
            assert(false);  // at least one exists in the map
        }
    }

    return skip;
}

bool CoreChecks::ValidateInterfaceFragmentOutput(const PIPELINE_STATE &pipeline, const SHADER_MODULE_STATE &module_state,
                                                 const EntryPoint &entrypoint) const {
    bool skip = false;
    const auto *ms_state = pipeline.MultisampleState();
    if (!pipeline.IsDynamic(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT) && ms_state && ms_state->alphaToCoverageEnable) {
        // TODO - DualSource blend has two outputs at location zero, so Index == 0 is the one that's required.
        // Currently lack support to test each index.
        if (!entrypoint.has_alpha_to_coverage_variable && !pipeline.DualSourceBlending()) {
            skip |= LogError(module_state.vk_shader_module(), "VUID-VkGraphicsPipelineCreateInfo-alphaToCoverageEnable-08891",
                             "vkCreateGraphicsPipelines(): alphaToCoverageEnable is set, but pCreateInfos[%" PRIu32
                             "] fragment shader doesn't declare a variable that covers Location 0, Component 3.",
                             pipeline.create_index);
        }
    }
    return skip;
}

bool CoreChecks::ValidateBuiltinLimits(const SHADER_MODULE_STATE &module_state, const EntryPoint &entrypoint,
                                       const PIPELINE_STATE &pipeline) const {
    bool skip = false;

    // Currently all builtin tested are only found in fragment shaders
    if (entrypoint.execution_model != spv::ExecutionModelFragment) {
        return skip;
    }

    for (const auto *variable : entrypoint.built_in_variables) {
        // Currently don't need to search in structs
        // Handles both the input and output sampleMask
        if (variable->decorations.builtin == spv::BuiltInSampleMask &&
            variable->array_size > phys_dev_props.limits.maxSampleMaskWords) {
            skip |= LogError(module_state.vk_shader_module(), "VUID-VkPipelineShaderStageCreateInfo-maxSampleMaskWords-00711",
                             "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                             "] The BuiltIns SampleMask array sizes is %u which exceeds "
                             "maxSampleMaskWords of %u in %s.",
                             pipeline.create_index, variable->array_size, phys_dev_props.limits.maxSampleMaskWords,
                             report_data->FormatHandle(module_state.vk_shader_module()).c_str());
            break;
        }
    }

    return skip;
}

bool CoreChecks::ValidateShaderStageInputOutputLimits(const SHADER_MODULE_STATE &module_state, VkShaderStageFlagBits stage,
                                                      const PIPELINE_STATE &pipeline, const EntryPoint &entrypoint) const {
    if (stage == VK_SHADER_STAGE_COMPUTE_BIT || stage == VK_SHADER_STAGE_ALL_GRAPHICS || stage == VK_SHADER_STAGE_ALL) {
        return false;
    }

    bool skip = false;
    auto const &limits = phys_dev_props.limits;

    const uint32_t num_vertices = entrypoint.execution_mode.output_vertices;
    const uint32_t num_primitives = entrypoint.execution_mode.output_primitives;
    const bool is_iso_lines = entrypoint.execution_mode.Has(ExecutionModeSet::iso_lines_bit);
    const bool is_point_mode = entrypoint.execution_mode.Has(ExecutionModeSet::point_mode_bit);
    const bool is_xfb_execution_mode = entrypoint.execution_mode.Has(ExecutionModeSet::xfb_bit);

    if (is_xfb_execution_mode &&
        ((pipeline.create_info_shaders & (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT)) != 0)) {
        skip |= LogError(pipeline.pipeline(), "VUID-VkGraphicsPipelineCreateInfo-None-02322",
                         "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                         "] If the pipeline is being created with pre-rasterization shader state, and there are any mesh shader "
                         "stages in the pipeline there must not be any shader stage in the pipeline with a Xfb execution mode",
                         pipeline.create_index);
    }

    // The max is a combiniation of both the user defined variables largest values
    // and
    // The total components used by built ins
    const auto max_input_slot =
        (entrypoint.max_input_slot_variable && entrypoint.max_input_slot) ? *entrypoint.max_input_slot : InterfaceSlot(0, 0, 0, 0);
    const auto max_output_slot = (entrypoint.max_output_slot_variable && entrypoint.max_output_slot) ? *entrypoint.max_output_slot
                                                                                                     : InterfaceSlot(0, 0, 0, 0);

    const uint32_t total_input_components = max_input_slot.slot + entrypoint.builtin_input_components;
    const uint32_t total_output_components = max_output_slot.slot + entrypoint.builtin_output_components;

    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            if (total_output_components >= limits.maxVertexOutputComponents) {
                skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                 "] Vertex shader output variable (%s) along with %" PRIu32
                                 " built-in components,  "
                                 "exceeds component limit VkPhysicalDeviceLimits::maxVertexOutputComponents (%" PRIu32 ")",
                                 pipeline.create_index, max_output_slot.Describe().c_str(), entrypoint.builtin_output_components,
                                 limits.maxVertexOutputComponents);
            }
            break;

        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            if (total_input_components >= limits.maxTessellationControlPerVertexInputComponents) {
                skip |= LogError(
                    module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                    "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                    "] Tessellation control shader input variable (%s) along with %" PRIu32
                    " built-in components,  "
                    "exceeds component limit VkPhysicalDeviceLimits::maxTessellationControlPerVertexInputComponents (%" PRIu32 ")",
                    pipeline.create_index, max_input_slot.Describe().c_str(), entrypoint.builtin_input_components,
                    limits.maxTessellationControlPerVertexInputComponents);
            }
            if (entrypoint.max_input_slot_variable) {
                if (entrypoint.max_input_slot_variable->is_patch &&
                    total_output_components >= limits.maxTessellationControlPerPatchOutputComponents) {
                    skip |= LogError(
                        module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                        "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                        "] Tessellation control shader output variable (%s) along with %" PRIu32
                        " built-in components,  "
                        "exceeds component limit VkPhysicalDeviceLimits::maxTessellationControlPerPatchOutputComponents (%" PRIu32
                        ")",
                        pipeline.create_index, max_output_slot.Describe().c_str(), entrypoint.builtin_output_components,
                        limits.maxTessellationControlPerPatchOutputComponents);
                }
                if (!entrypoint.max_input_slot_variable->is_patch &&
                    total_output_components >= limits.maxTessellationControlPerVertexOutputComponents) {
                    skip |= LogError(
                        module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                        "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                        "] Tessellation control shader output variable (%s) along with %" PRIu32
                        " built-in components,  "
                        "exceeds component limit VkPhysicalDeviceLimits::maxTessellationControlPerVertexOutputComponents (%" PRIu32
                        ")",
                        pipeline.create_index, max_output_slot.Describe().c_str(), entrypoint.builtin_output_components,
                        limits.maxTessellationControlPerVertexOutputComponents);
                }
            }
            break;

        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            if (total_input_components >= limits.maxTessellationEvaluationInputComponents) {
                skip |= LogError(
                    module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                    "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                    "] Tessellation evaluation shader input variable (%s) along with %" PRIu32
                    " built-in components,  "
                    "exceeds component limit VkPhysicalDeviceLimits::maxTessellationEvaluationInputComponents (%" PRIu32 ")",
                    pipeline.create_index, max_input_slot.Describe().c_str(), entrypoint.builtin_input_components,
                    limits.maxTessellationEvaluationInputComponents);
            }
            if (total_output_components >= limits.maxTessellationEvaluationOutputComponents) {
                skip |= LogError(
                    module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                    "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                    "] Tessellation evaluation shader output variable (%s) along with %" PRIu32
                    " built-in components,  "
                    "exceeds component limit VkPhysicalDeviceLimits::maxTessellationEvaluationOutputComponents (%" PRIu32 ")",
                    pipeline.create_index, max_output_slot.Describe().c_str(), entrypoint.builtin_output_components,
                    limits.maxTessellationEvaluationOutputComponents);
            }
            // Portability validation
            if (IsExtEnabled(device_extensions.vk_khr_portability_subset)) {
                if (is_iso_lines && (VK_FALSE == enabled_features.portability_subset_features.tessellationIsolines)) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-tessellationShader-06326",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "](portability error): Tessellation evaluation shader"
                                     " is using abstract patch type IsoLines, but this is not supported on this platform",
                                     pipeline.create_index);
                }
                if (is_point_mode && (VK_FALSE == enabled_features.portability_subset_features.tessellationPointMode)) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-tessellationShader-06327",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "](portability error): Tessellation evaluation shader"
                                     " is using abstract patch type PointMode, but this is not supported on this platform",
                                     pipeline.create_index);
                }
            }
            break;

        case VK_SHADER_STAGE_GEOMETRY_BIT:
            if (total_input_components >= limits.maxGeometryInputComponents) {
                skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                 "] Geometry shader input variable (%s) along with %" PRIu32
                                 " built-in components,  "
                                 "exceeds component limit VkPhysicalDeviceLimits::maxGeometryInputComponents (%" PRIu32 ")",
                                 pipeline.create_index, max_input_slot.Describe().c_str(), entrypoint.builtin_input_components,
                                 limits.maxGeometryInputComponents);
            }
            if (total_output_components >= limits.maxGeometryOutputComponents) {
                skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                 "] Geometry shader output variable (%s) along with %" PRIu32
                                 " built-in components,  "
                                 "exceeds component limit VkPhysicalDeviceLimits::maxGeometryOutputComponents (%" PRIu32 ")",
                                 pipeline.create_index, max_output_slot.Describe().c_str(), entrypoint.builtin_output_components,
                                 limits.maxGeometryOutputComponents);
            }
            break;

        case VK_SHADER_STAGE_FRAGMENT_BIT:
            if (total_input_components >= limits.maxFragmentInputComponents) {
                skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-Location-06272",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                 "] Fragment shader input variable (%s) along with %" PRIu32
                                 " built-in components,  "
                                 "exceeds component limit VkPhysicalDeviceLimits::maxFragmentInputComponents (%" PRIu32 ")",
                                 pipeline.create_index, max_input_slot.Describe().c_str(), entrypoint.builtin_input_components,
                                 limits.maxFragmentInputComponents);
            }
            break;

        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
        case VK_SHADER_STAGE_MISS_BIT_KHR:
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            break;

        // Shader stage is an alias, but the ExecutionModel is not
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            if (entrypoint.execution_model == spv::ExecutionModelMeshNV) {
                if (num_vertices > phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputVertices) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-MeshNV-07113",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "] Mesh shader output vertices count exceeds the "
                                     "maxMeshOutputVertices of %" PRIu32 " by %" PRIu32,
                                     pipeline.create_index, phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputVertices,
                                     num_vertices - phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputVertices);
                }
                if (num_primitives > phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputPrimitives) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-MeshNV-07114",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "] Mesh shader output primitives count exceeds the "
                                     "maxMeshOutputPrimitives of %" PRIu32 " by %" PRIu32,
                                     pipeline.create_index, phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputPrimitives,
                                     num_primitives - phys_dev_ext_props.mesh_shader_props_nv.maxMeshOutputPrimitives);
                }
            } else if (entrypoint.execution_model == spv::ExecutionModelMeshEXT) {
                if (num_vertices > phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputVertices) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-MeshEXT-07115",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "] Mesh shader output vertices count exceeds the "
                                     "maxMeshOutputVertices of %" PRIu32 " by %" PRIu32,
                                     pipeline.create_index, phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputVertices,
                                     num_vertices - phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputVertices);
                }
                if (num_primitives > phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputPrimitives) {
                    skip |= LogError(module_state.vk_shader_module(), "VUID-RuntimeSpirv-MeshEXT-07116",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                     "] Mesh shader output primitives count exceeds the "
                                     "maxMeshOutputPrimitives of %u by %u ",
                                     pipeline.create_index, phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputPrimitives,
                                     num_primitives - phys_dev_ext_props.mesh_shader_props_ext.maxMeshOutputPrimitives);
                }
            }
            break;

        default:
            assert(false);  // This should never happen
    }
    return skip;
}

bool CoreChecks::ValidateInterfaceBetweenStages(const SHADER_MODULE_STATE &producer, const EntryPoint &producer_entrypoint,
                                                const SHADER_MODULE_STATE &consumer, const EntryPoint &consumer_entrypoint,
                                                uint32_t pipe_index) const {
    bool skip = false;

    if (producer_entrypoint.has_passthrough) {
        return skip;  // PassthroughNV doesn't have to do Location matching
    }

    const VkShaderStageFlagBits producer_stage = producer_entrypoint.stage;
    const VkShaderStageFlagBits consumer_stage = consumer_entrypoint.stage;

    // build up a mapping of which slots are used and then go through it and look for gaps
    struct ComponentInfo {
        const StageInteraceVariable *output = nullptr;
        uint32_t output_type = 0;
        uint32_t output_width = 0;
        const StageInteraceVariable *input = nullptr;
        uint32_t input_type = 0;
        uint32_t input_width = 0;
    };
    // <Location, Components[4]> (only 4 components in a Location)
    vvl::unordered_map<uint32_t, std::array<ComponentInfo, 4>> slot_map;

    for (const auto &interface_slot : producer_entrypoint.output_interface_slots) {
        auto &slot = slot_map[interface_slot.first.Location()][interface_slot.first.Component()];
        if (interface_slot.second->nested_struct || interface_slot.second->physical_storage_buffer) {
            return skip;  // TODO workaround
        }
        slot.output = interface_slot.second;
        slot.output_type = interface_slot.first.type;
        slot.output_width = interface_slot.first.bit_width;
    }
    for (const auto &interface_slot : consumer_entrypoint.input_interface_slots) {
        auto &slot = slot_map[interface_slot.first.Location()][interface_slot.first.Component()];
        if (interface_slot.second->nested_struct || interface_slot.second->physical_storage_buffer) {
            return skip;  // TODO workaround
        }
        slot.input = interface_slot.second;
        slot.input_type = interface_slot.first.type;
        slot.input_width = interface_slot.first.bit_width;
    }

    for (const auto &slot : slot_map) {
        // Found that sometimes there is a big mismatch and printing out EVERY slot adds a lot of noise
        if (skip) break;

        const uint32_t location = slot.first;
        for (uint32_t component = 0; component < 4; component++) {
            const auto &component_info = slot.second[component];
            const auto *input_var = component_info.input;
            const auto *output_var = component_info.output;

            if ((input_var == nullptr) && (output_var == nullptr)) {
                continue;  // both empty
            } else if ((input_var != nullptr) && (output_var != nullptr)) {
                // if matched, need to check type
                // Only the OpType has to match, signed vs unsigned in not important
                if ((component_info.output_type != component_info.input_type) ||
                    (component_info.output_width != component_info.input_width)) {
                    const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
                    skip |=
                        LogError(objlist, "VUID-RuntimeSpirv-OpEntryPoint-07754",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] Type mismatch on Location %" PRIu32
                                 " Component %" PRIu32 ", between\n%s stage:\n%s\n%s stage:\n%s\n",
                                 pipe_index, location, component, string_VkShaderStageFlagBits(producer_stage),
                                 producer.DescribeType(output_var->type_id).c_str(), string_VkShaderStageFlagBits(consumer_stage),
                                 consumer.DescribeType(input_var->type_id).c_str());
                }

                // Tessellation needs to match Patch vs Vertex
                if ((producer_stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) &&
                    (consumer_stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) &&
                    (input_var->is_patch != output_var->is_patch)) {
                    const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
                    skip |= LogError(objlist, "VUID-RuntimeSpirv-OpVariable-08746",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] at Location %" PRIu32
                                     " Component %" PRIu32 " Tessellation Control is %s while Tessellation Evaluation is %s",
                                     pipe_index, location, component, input_var->is_patch ? "patch" : "vertex",
                                     output_var->is_patch ? "patch" : "vertex");
                }

                // If using maintenance4 need to check Vectors incase different sizes
                if (!enabled_features.core13.maintenance4 && (output_var->base_type.Opcode() == spv::OpTypeVector) &&
                    (input_var->base_type.Opcode() == spv::OpTypeVector)) {
                    // Note the "Component Count" in the VU refers to OpTypeVector's operand and NOT the "Component slot"
                    const uint32_t output_vec_size = output_var->base_type.Word(3);
                    const uint32_t input_vec_size = input_var->base_type.Word(3);
                    if (output_vec_size > input_vec_size) {
                        const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
                        skip |=
                            LogError(objlist, "VUID-RuntimeSpirv-maintenance4-06817",
                                     "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] starting at Location %" PRIu32
                                     " Component %" PRIu32 " the Output (%s) has a Vec%" PRIu32 " while Input (%s) as a Vec%" PRIu32
                                     ". Enable VK_KHR_maintenance4 device extension to allow relaxed interface matching "
                                     "between input and output vectors.",
                                     pipe_index, location, component, string_VkShaderStageFlagBits(producer_stage), output_vec_size,
                                     string_VkShaderStageFlagBits(consumer_stage), input_vec_size);
                        break;  // Only need to report for the first component found
                    }
                }
            } else if ((input_var == nullptr) && (output_var != nullptr)) {
                // Missing input slot
                // It is not an error if a stage does not consume all outputs from the previous stage
                // The values will be undefined, but still legal
                // Don't give any warning if maintenance4 with vectors
                if (!enabled_features.core13.maintenance4 && (output_var->base_type.Opcode() != spv::OpTypeVector)) {
                    const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
                    skip |= LogPerformanceWarning(objlist, kVUID_Core_Shader_OutputNotConsumed,
                                                  "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32
                                                  "] %s declared to output location %" PRIu32 " Component %" PRIu32
                                                  " but is not an Input declared by %s.",
                                                  pipe_index, string_VkShaderStageFlagBits(producer_stage), location, component,
                                                  string_VkShaderStageFlagBits(consumer_stage));
                }
            } else if ((input_var != nullptr) && (output_var == nullptr)) {
                // Missing output slot
                if ((consumer_stage & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) &&
                    (input_var->base_type.Opcode() == spv::OpTypeArray)) {
                    break;  // When going inbetween Tessellation or Geometry, array size can be different
                }
                const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
                skip |= LogError(objlist, "VUID-RuntimeSpirv-OpEntryPoint-08743",
                                 "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] %s declared input at Location %" PRIu32
                                 " Component %" PRIu32 " but it is not an Output declared in %s",
                                 pipe_index, string_VkShaderStageFlagBits(consumer_stage), location, component,
                                 string_VkShaderStageFlagBits(producer_stage));
                break;  // Only need to report for the first component found
            }
        }
    }

    // Need to check the BuiltIn interface (if not going into Fragment)
    if (consumer_stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
        return skip;
    }

    std::vector<uint32_t> input_builtins_block;
    std::vector<uint32_t> output_builtins_block;
    for (const auto *variable : producer_entrypoint.built_in_variables) {
        if (variable->storage_class == spv::StorageClassOutput && !variable->builtin_block.empty()) {
            output_builtins_block = variable->builtin_block;
            break;
        }
    }
    for (const auto *variable : consumer_entrypoint.built_in_variables) {
        if (variable->storage_class == spv::StorageClassInput && !variable->builtin_block.empty()) {
            input_builtins_block = variable->builtin_block;
            break;
        }
    }

    bool mismatch = false;
    if (input_builtins_block.empty() || output_builtins_block.empty()) {
        // TODO - Nothing about this in spec, need to add language to confirm this is correct
        return skip;
    } else if (input_builtins_block.size() != output_builtins_block.size()) {
        mismatch = true;
    } else {
        for (size_t i = 0; i < input_builtins_block.size(); i++) {
            const uint32_t input_builtin = input_builtins_block[i];
            const uint32_t output_builtin = output_builtins_block[i];
            if (input_builtin == DecorationSet::kInvalidValue || output_builtin == DecorationSet::kInvalidValue) {
                continue;  // some stages (TessControl -> TessEval) can have legal block vs non-block mistmatch
            } else if (input_builtin != output_builtin) {
                mismatch = true;
            }
        }
    }

    if (mismatch) {
        std::stringstream msg;
        msg << string_VkShaderStageFlagBits(producer_stage) << " Output Block {\n";
        for (size_t i = 0; i < output_builtins_block.size(); i++) {
            msg << "\t" << i << ": " << string_SpvBuiltIn(output_builtins_block[i]) << "\n";
        }
        msg << "}\n";
        msg << string_VkShaderStageFlagBits(consumer_stage) << " Input Block {\n";
        for (size_t i = 0; i < input_builtins_block.size(); i++) {
            msg << "\t" << i << ": " << string_SpvBuiltIn(input_builtins_block[i]) << "\n";
        }
        msg << "}\n";
        const LogObjectList objlist(producer.vk_shader_module(), consumer.vk_shader_module());
        skip |= LogError(objlist, "VUID-RuntimeSpirv-OpVariable-08746",
                         "vkCreateGraphicsPipelines(): pCreateInfos[%" PRIu32 "] Mistmatch in BuiltIn blocks:\n %s", pipe_index,
                         msg.str().c_str());
    }
    return skip;
}

// Validate that the shaders used by the given pipeline and store the active_slots
//  that are actually used by the pipeline into pPipeline->active_slots
bool CoreChecks::ValidateGraphicsPipelineShaderState(const PIPELINE_STATE &pipeline) const {
    bool skip = false;

    if (!(pipeline.pre_raster_state || pipeline.fragment_shader_state)) {
        // Only validate pipelines that contain shader stages
        return skip;
    }

    const PipelineStageState *vertex_stage = nullptr, *fragment_stage = nullptr;
    for (auto &stage_state : pipeline.stage_states) {
        const VkShaderStageFlagBits stage = stage_state.create_info->stage;
        // Only validate the shader state once when added, not again when linked
        if ((stage & pipeline.linking_shaders) == 0) {
            skip |= ValidatePipelineShaderStage(pipeline, stage_state);
        }
        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            vertex_stage = &stage_state;
        }
        if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            fragment_stage = &stage_state;
        }
    }

    // if the shader stages are no good individually, cross-stage validation is pointless.
    if (skip) return true;

    if (pipeline.vertex_input_state && vertex_stage && vertex_stage->entrypoint && vertex_stage->module_state->has_valid_spirv &&
        !pipeline.IsDynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
        skip |= ValidateInterfaceVertexInput(pipeline, *vertex_stage->module_state.get(), *vertex_stage->entrypoint);
    }

    if (pipeline.fragment_shader_state && fragment_stage && fragment_stage->entrypoint &&
        fragment_stage->module_state->has_valid_spirv) {
        skip |= ValidateInterfaceFragmentOutput(pipeline, *fragment_stage->module_state.get(), *fragment_stage->entrypoint);
    }

    for (size_t i = 1; i < pipeline.stage_states.size(); i++) {
        const auto &producer = pipeline.stage_states[i - 1];
        const auto &consumer = pipeline.stage_states[i];
        assert(producer.module_state);
        if (&producer == fragment_stage) {
            break;
        }
        if (consumer.module_state) {
            if (consumer.module_state->has_valid_spirv && producer.module_state->has_valid_spirv && consumer.entrypoint &&
                producer.entrypoint) {
                skip |= ValidateInterfaceBetweenStages(*producer.module_state.get(), *producer.entrypoint,
                                                       *consumer.module_state.get(), *consumer.entrypoint, pipeline.create_index);
            }
        }
    }
    return skip;
}
