#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: animation_inspect (read-only)
 *
 * Read-only structured dump for animation assets. Resolves the asset
 * by `/Game/...` path or short name and branches on the loaded class:
 *   - USkeletalMesh: skeleton path + LOD count + bone list +
 *     socket list (mesh-level + skeleton-level).
 *   - UAnimSequence: sampling frame rate (numerator / denominator /
 *     approx_fps), play length, additive animation type, rate scale,
 *     sampled key count, and the notify list.
 *   - UAnimMontage: composite section list with per-section name +
 *     start time + next section, plus the slot-track list.
 *   - UBlendSpace (and 1D variants): per-axis FBlendParameter
 *     (display name + min + max + grid divisions + snap / wrap
 *     flags) and the sample count.
 *   - UAnimBlueprint: parent class + target skeleton + state-machine
 *     name list (read off the cached UAnimBlueprintGeneratedClass
 *     when the BP has compiled at least once) + variable count.
 *
 * Operation: single op (`inspect`, default).
 *
 * Required input:
 *   - `asset`: short asset name or full `/Game/...` path. Resolves
 *     through `UEditorAssetLibrary::LoadAsset`.
 *
 * Optional inputs:
 *   - `include_bones`: emit per-bone records on USkeletalMesh.
 *     Default True.
 *   - `include_sockets`: emit the socket list on USkeletalMesh.
 *     Default True.
 *   - `include_notifies`: emit the notify list on UAnimSequence /
 *     UAnimMontage. Default True.
 *   - `include_sections`: emit the composite section list on
 *     UAnimMontage. Default True.
 *   - `include_slot_tracks`: emit the slot-track list on
 *     UAnimMontage. Default True.
 *   - `include_state_machines`: emit the state-machine name list
 *     on UAnimBlueprint. Default True.
 *   - `max_bones`: cap on per-skeleton bone emission. Default 4096.
 *   - `max_notifies`: cap on per-asset notify emission. Default 1024.
 *
 * Returns a class-keyed payload. The top-level `kind` field is one
 * of `skeletal_mesh` / `anim_sequence` / `anim_montage` /
 * `blend_space` / `anim_blueprint` / `unknown`. Common fields:
 *   - `name`, `path`, `class`, `class_path`.
 *
 * Per kind:
 *   - skeletal_mesh: `skeleton_path`, `lod_count`, `bone_count`,
 *     optional `bones` array (each `{name, parent_index,
 *     parent_name}`), optional `sockets` array (each `{name,
 *     bone_name, relative_location, relative_rotation,
 *     relative_scale, source}`).
 *   - anim_sequence: `play_length`, `rate_scale`,
 *     `sampling_frame_rate` (numerator / denominator / approx_fps),
 *     `sampled_key_count`, `additive_anim_type` (string token),
 *     optional `notifies` array.
 *   - anim_montage: `play_length`, `rate_scale`, optional
 *     `composite_sections` array (each `{name, time, next_section}`),
 *     optional `slot_tracks` array (each `{slot_name,
 *     animation_count}`), optional `notifies` array.
 *   - blend_space: `sample_count`, `axis_count` (1 for BlendSpace1D,
 *     2 otherwise), `axes` array (each `{display_name, min, max,
 *     grid_num, snap_to_grid, wrap_input}`).
 *   - anim_blueprint: `parent_class`, `parent_class_path`,
 *     `target_skeleton_path`, `is_template`, `variable_count`,
 *     optional `state_machines` array (each
 *     `{name, state_count, transition_count, initial_state}`).
 *
 * Read-only. We do not mutate the asset.
 *
 * Clean-room implementation derived from the public UE5 Animation
 * API:
 *   - USkeletalMesh::GetSkeleton / GetLODNum /
 *     GetActiveSocketList / GetRefSkeleton.
 *   - USkeleton::GetReferenceSkeleton / Sockets.
 *   - FReferenceSkeleton::GetRefBoneInfo.
 *   - UAnimSequenceBase::GetPlayLength / RateScale / Notifies.
 *   - UAnimSequence::GetSamplingFrameRate / GetNumberOfSampledKeys /
 *     GetAdditiveAnimType.
 *   - UAnimMontage::CompositeSections / SlotAnimTracks.
 *   - UBlendSpace::GetBlendSamples / BlendParameters.
 *   - UAnimBlueprint::TargetSkeleton / GetAnimBlueprintGeneratedClass.
 *   - UAnimBlueprintGeneratedClass::GetBakedStateMachines.
 *   - FAnimNotifyEvent::GetTime / NotifyName /
 *     Notify / NotifyStateClass / Duration / TrackIndex.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftAnimationInspectCommands
{
public:
    FSproftAnimationInspectCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAnimationInspect(const TSharedPtr<FJsonObject>& Params);
};
