#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: ik_rig_edit (read + edit slice)
 *
 * Inspect or mutate a `UIKRigDefinition` asset. Pairs with
 * `ik_retarget` for the rig side of the retargeting pipeline. The
 * 5.6 IK Rig refactor moved the solver list onto a polymorphic op
 * stack (`FInstancedStruct` of `FIKRigSolverBase`-derived structs);
 * the read-only inspect slice walks the asset's public surface plus
 * the solver stack and reports:
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
 * Operations:
 *   - `inspect` (default): the read-only walk above.
 *   - `set_retarget_root`: writes the retarget root bone through
 *     `UIKRigController::SetRetargetRoot`. The asset's existing
 *     skeleton must already contain the named bone.
 *   - `add_retarget_chain`: appends a new retarget chain through
 *     `UIKRigController::AddRetargetChain(ChainName, StartBone,
 *     EndBone, OptionalGoalName)`. Editor-only API. The IK Rig
 *     duplicate-name guard returns NAME_None on collision; we
 *     surface that as an error response.
 *   - `add_ik_goal`: appends a new IK goal through
 *     `UIKRigController::AddNewGoal(GoalName, BoneName)`. Editor-
 *     only API. Same NAME_None guard surfaces as an error.
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
 * Op-specific inputs:
 *   - set_retarget_root: `bone` (FName) required.
 *   - add_retarget_chain: `chain_name` + `start_bone` + `end_bone`
 *     required, optional `ik_goal_name`.
 *   - add_ik_goal: `goal_name` + `bone` required.
 *
 * Optional inputs (mutating ops):
 *   - `save`: default true. False keeps the edit transient.
 *
 * Returns the inspect structure for the inspect op, or a per-op
 * dict carrying `operation`, `rig`, op-specific fields, and a
 * `saved` flag.
 *
 * Edit-side ops still on the BACKLOG: rebind preview mesh, append /
 * remove solver, remove chain, override goal transform, mutate
 * bone settings.
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
    TSharedPtr<FJsonObject> HandleSetRetargetRoot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddRetargetChain(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddIkGoal(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSolver(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveSolverAt(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSolverSettings(const TSharedPtr<FJsonObject>& Params);
};
