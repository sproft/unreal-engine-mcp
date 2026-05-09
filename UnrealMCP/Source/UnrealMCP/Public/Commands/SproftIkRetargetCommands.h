#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: ik_retarget (read + edit slice)
 *
 * Inspect or mutate a `UIKRetargeter` asset. The 5.6 retargeter refactor
 * moved chain mapping / root settings / global settings into a polymorphic
 * op stack (each op is an `FInstancedStruct` of a subclass of
 * `FIKRetargetOpBase`). The read-only inspect slice walks the asset's
 * public surface plus the op stack and reports:
 *   - asset path / name / class
 *   - source IK Rig path + has_source_ik_rig flag
 *   - target IK Rig path + has_target_ik_rig flag
 *   - current source / target retarget pose names plus the per-skeleton
 *     pose lists (each with bone-rotation-offset count + has_root_offset
 *     flag)
 *   - the retarget op stack: per-op name, parent op name, struct type
 *     short-name + path, enabled flag, and any chain pairs the op
 *     exposes via `FIKRetargetOpBase::GetChainMapping()`
 *   - aggregate counts (op_count, chain_pair_count)
 *
 * Operations:
 *   - `inspect` (default): read-only walk above.
 *   - `set_source_ik_rig`: rebind the retargeter's source IK Rig
 *     through `UIKRetargeterController::SetIKRig(Source, Rig)`. Pass
 *     `null` / `clear=true` to clear the binding.
 *   - `set_target_ik_rig`: rebind the retargeter's target IK Rig
 *     through `UIKRetargeterController::SetIKRig(Target, Rig)`. Same
 *     clear flow.
 *   - `set_retarget_pose`: switch the current retarget pose for either
 *     the source or target side through
 *     `UIKRetargeterController::SetCurrentRetargetPose(PoseName, Side)`.
 *     The named pose must already exist on that side; create it
 *     through the editor or `CreateRetargetPose` first.
 *
 * Inputs (inspect):
 *   - retargeter / path / asset / asset_path: required. Accepts a
 *     `/Game/...` UIKRetargeter path or a short asset name (resolved
 *     via the asset registry).
 *   - include_op_chain_mappings: when true (default), each op record
 *     carries its `chain_mapping` array. False keeps the per-op
 *     metadata but skips the chain pair walk for big rigs.
 *   - max_ops: cap on the op stack walk. Default 64.
 *
 * Inputs (set_source_ik_rig / set_target_ik_rig):
 *   - retargeter: required.
 *   - rig / ik_rig / ik_rig_path: `/Game/...` UIKRigDefinition path or
 *     short asset name. Required unless `clear=true`.
 *   - clear: pass true to unbind the side. Default false.
 *   - save: save the asset after the edit. Default true.
 *
 * Inputs (set_retarget_pose):
 *   - retargeter: required.
 *   - side: `source` or `target`. Required (no default; the caller
 *     must pick the side they intend to edit).
 *   - pose_name / pose: required. The named pose must already exist
 *     on that side.
 *   - save: save the asset after the edit. Default true.
 *
 * Returns the inspect structure for the inspect op, or a per-op dict
 * carrying `operation`, `retargeter`, op-specific fields, and a
 * `saved` flag.
 *
 * Edit-side ops still on the BACKLOG: append op / remove op (the
 * polymorphic FInstancedStruct array on the asset), set chain mapping
 * pair on a chosen op, override per-bone retarget pose offsets, and
 * profile management through `UIKRetargeter::GetProfileByName`.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UIKRetargeter` from
 *     `Plugins/Animation/IKRig/Source/IKRig/Public/Retargeter/IKRetargeter.h`.
 *   - `UIKRetargeter::GetIKRig` /
 *     `GetCurrentRetargetPoseName` / `GetCurrentRetargetPose` /
 *     `GetRetargetOps` / `HasSourceIKRig` / `HasTargetIKRig`.
 *   - `FIKRetargetPose::GetAllDeltaRotations` /
 *     `GetRootTranslationDelta`.
 *   - `FInstancedStruct::GetScriptStruct` / `GetPtr<FIKRetargetOpBase>`.
 *   - `FIKRetargetOpBase::GetName` / `GetParentOpName` / `IsEnabled` /
 *     `GetChainMapping`.
 *   - `FRetargetChainMapping::GetChainPairs`.
 *   - `UIKRetargeterController::GetController` / `SetIKRig` /
 *     `SetCurrentRetargetPose` (editor-only, in `IKRigEditor`).
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftIkRetargetCommands
{
public:
    FSproftIkRetargetCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleInspect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetSourceIkRig(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetTargetIkRig(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRetargetPose(const TSharedPtr<FJsonObject>& Params);
};
