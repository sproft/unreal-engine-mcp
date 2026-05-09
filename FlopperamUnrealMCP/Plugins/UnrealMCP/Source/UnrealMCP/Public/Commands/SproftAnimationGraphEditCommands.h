#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: animation_graph_edit (read-only first slice)
 *
 * Inspect a `UAnimBlueprint` asset's compiled state machines plus
 * the flat anim-graph node list. Pairs with `animation_inspect`,
 * which already returns a state-machine summary keyed off
 * `UAnimBlueprintGeneratedClass::BakedStateMachines`; this slice
 * goes one level deeper:
 *
 * - per state machine: `name`, `initial_state`, `state_count`,
 *   `transition_count`, plus a `states` array (each state with its
 *   FName + state-root-node index + per-state `transitions` rows
 *   pointing at the per-machine `transitions` table) and a
 *   `transitions` array (each row with `index`, `previous_state`,
 *   `next_state`, `previous_state_name`, `next_state_name`,
 *   `crossfade_duration`, `min_time_before_reentry`, blend mode
 *   token, and the alpha-blend logic-type token).
 * - the AnimGraph node-property list: a flat array of
 *   `{index, struct_type, struct_path}` entries off the cached
 *   `UAnimBlueprintGeneratedClass::AnimNodeProperties`. The struct
 *   property's `Struct` field gives the anim node's UScriptStruct,
 *   which is what the editor uses to identify each AnimGraph node
 *   (e.g. `FAnimNode_StateMachine`, `FAnimNode_BlendListByEnum`,
 *   `FAnimNode_SequencePlayer`).
 *
 * The caller must pass a UAnimBlueprint that has compiled at least
 * once; the cached generated class is what carries the baked state
 * machines and AnimGraph node table. Uncompiled AnimBPs return a
 * `not_compiled` flag with empty arrays.
 *
 * Operation: single op (`inspect`, default). Edit-side ops (state
 * machine create / mutate, anim-graph node add / connect, link a
 * Linked Anim Graph by tag) stay on the BACKLOG.
 *
 * Required input:
 *   - `anim_bp`: short asset name or `/Game/...` UAnimBlueprint
 *     path.
 *
 * Optional inputs:
 *   - `include_state_machines`: default true.
 *   - `include_states`:         default true. Each state entry
 *                               carries `name`, `state_root_node_index`,
 *                               and per-state `transitions` rows.
 *   - `include_transitions`:    default true. Adds the per-machine
 *                               `transitions` array.
 *   - `include_anim_nodes`:     default true. Adds the flat
 *                               anim-graph node-property list.
 *   - `max_state_machines`:     default 64.
 *   - `max_states_per_machine`: default 256.
 *   - `max_transitions_per_machine`: default 1024.
 *   - `max_anim_nodes`:         default 2048.
 *
 * Returns a structured payload:
 *   - `name`, `path`, `class`, `parent_class` / `parent_class_path`,
 *     `target_skeleton_path`, `is_template`, `compiled` flag,
 *     aggregate counts (`state_machine_count` / `anim_node_count`).
 *   - `state_machines`: array of per-machine dicts.
 *   - `anim_nodes`: flat array of `{index, struct_type,
 *     struct_path}` rows.
 *
 * Read-only. We do not mutate the asset.
 *
 * Clean-room implementation derived from the public UE5 Anim API:
 *   - `UAnimBlueprint::GetAnimBlueprintGeneratedClass`.
 *   - `UAnimBlueprintGeneratedClass::BakedStateMachines` /
 *     `AnimNodeProperties`.
 *   - `FBakedAnimationStateMachine` / `FBakedAnimationState` /
 *     `FBakedStateExitTransition` /
 *     `FAnimationTransitionBetweenStates` from
 *     `Animation/AnimStateMachineTypes.h`.
 *   - `FStructProperty::Struct` for each anim node's UScriptStruct.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftAnimationGraphEditCommands
{
public:
    FSproftAnimationGraphEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAnimationGraphInspect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddState(const TSharedPtr<FJsonObject>& Params);
};
