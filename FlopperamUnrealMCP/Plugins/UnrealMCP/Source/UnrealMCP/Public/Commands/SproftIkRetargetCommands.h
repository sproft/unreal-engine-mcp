#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: ik_retarget (read-only first slice)
 *
 * Inspect a `UIKRetargeter` asset. The 5.6 retargeter refactor moved
 * chain mapping / root settings / global settings into a polymorphic
 * op stack (each op is an `FInstancedStruct` of a subclass of
 * `FIKRetargetOpBase`). The read-only slice walks the asset's public
 * surface plus the op stack and reports:
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
 * One op (`inspect`, default).
 *
 * Inputs:
 *   - retargeter / path / asset / asset_path: required. Accepts a
 *     `/Game/...` UIKRetargeter path or a short asset name (resolved
 *     via the asset registry).
 *   - include_op_chain_mappings: when true (default), each op record
 *     carries its `chain_mapping` array. False keeps the per-op
 *     metadata but skips the chain pair walk for big rigs.
 *   - max_ops: cap on the op stack walk. Default 64.
 *
 * Returns the structure described above plus an `op_count_total`
 * and `ops_truncated` flag when the cap fires.
 *
 * Read-only. We never mutate the asset.
 *
 * Edit-side ops (rebind source / target IK Rig, append op,
 * remove op, set chain mapping pair, override retarget pose,
 * profile management) remain on the BACKLOG.
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
};
