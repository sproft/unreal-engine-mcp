#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: ik_rig_edit (read-only first slice)
 *
 * Inspect a `UIKRigDefinition` asset. Pairs with `ik_retarget` for
 * the rig side of the retargeting pipeline. The 5.6 IK Rig refactor
 * moved the solver list onto a polymorphic op stack
 * (`FInstancedStruct` of `FIKRigSolverBase`-derived structs); the
 * read-only slice walks the asset's public surface plus the solver
 * stack and reports:
 *
 *   - asset path / name / class
 *   - preview skeletal mesh path (when set)
 *   - retarget root bone (`Pelvis`)
 *   - retarget chain list. Each chain reports `chain_name` /
 *     `start_bone` / `end_bone` / `ik_goal_name`.
 *   - IK goal list. Each goal reports `goal_name` / `bone_name` /
 *     transform / position alpha / rotation alpha.
 *   - solver stack. Each solver reports `index` / `struct_type`
 *     short-name + path / `enabled` / `start_bone` (when the solver
 *     uses one) / `end_bone` (when the solver uses one) and a
 *     reflection-driven property dump for the solver's
 *     `GetSolverSettings()` block.
 *   - bone settings list (per-solver per-bone settings rows aggregated
 *     across the solver stack).
 *   - aggregate counts (`chain_count`, `goal_count`, `solver_count`,
 *     `bone_setting_count`).
 *
 * One op (`inspect`, default).
 *
 * Inputs:
 *   - rig / path / asset / asset_path: required. Accepts a
 *     `/Game/...` UIKRigDefinition path or a short asset name
 *     (resolved via the asset registry).
 *   - include_solver_settings: default true. When true each solver
 *     row carries a `settings` dict reflected off
 *     `GetSolverSettings()` plus the solver settings struct type.
 *   - include_bone_settings: default true. When true each solver
 *     row also carries a `bone_settings` array (one entry per bone
 *     that the solver has settings on).
 *   - max_chains / max_goals / max_solvers: per-list caps.
 *
 * Returns the structure described above.
 *
 * Read-only. We never mutate the asset.
 *
 * Edit-side ops (rebind preview mesh, append / remove solver,
 * append / remove chain, rename retarget root, override goal
 * transform, mutate bone settings) remain on the BACKLOG.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UIKRigDefinition` from
 *     `Plugins/Animation/IKRig/Source/IKRig/Public/Rig/IKRigDefinition.h`.
 *   - `UIKRigDefinition::GetGoalArray` /
 *     `GetSolverStructs` / `GetRetargetChains` / `GetPelvis` /
 *     `GetPreviewMesh`.
 *   - `FIKRigSolverBase::IsEnabled` / `GetSolverSettings` /
 *     `GetSolverSettingsType` / `UsesStartBone` / `GetStartBone` /
 *     `UsesEndBone` / `GetEndBone` / `UsesCustomBoneSettings` /
 *     `GetBonesWithSettings` / `GetBoneSettings`.
 *   - `FInstancedStruct::GetScriptStruct` / `GetPtr<FIKRigSolverBase>`.
 *   - Property reflection through `TFieldIterator<FProperty>` and
 *     `FProperty::ExportText_Direct` (same approach the existing
 *     read-only inspectors use).
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftIkRigEditCommands
{
public:
    FSproftIkRigEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleInspect(const TSharedPtr<FJsonObject>& Params);
};
