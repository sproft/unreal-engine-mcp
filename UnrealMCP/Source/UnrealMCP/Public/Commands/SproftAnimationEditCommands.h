#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: animation_edit (small variant)
 *
 * Targeted UAnimSequence (and UAnimMontage where the field exists)
 * mutations through a multi-op tool keyed by `op`. The hosted Flop
 * `animation_edit` covers a much wider surface (timeline / curves /
 * key frames / sync markers); the small variant ships the three ops
 * that come up most often when rigging up a project's notify pipeline:
 *
 *   - `set_rate_scale`: write `RateScale` (float) on UAnimSequenceBase.
 *     Applies to UAnimSequence and UAnimMontage (the field lives on
 *     the base class). Saves on success.
 *   - `set_additive`: toggle the additive shape on UAnimSequence.
 *     Writes `AdditiveAnimType` (none / local_space /
 *     rotation_offset_mesh_space) plus optional `RefPoseType`
 *     (none / ref_pose / anim_scaled / anim_frame). The optional
 *     `ref_pose_seq` is a `/Game/...` UAnimSequence path applied
 *     when RefPoseType is anim_scaled / anim_frame; `ref_frame_index`
 *     is the integer frame index used when RefPoseType is anim_frame.
 *   - `add_notify`: append an FAnimNotifyEvent to a notify track on
 *     UAnimSequenceBase. Goes through
 *     `UAnimationBlueprintLibrary::AddAnimationNotifyEvent` /
 *     `AddAnimationNotifyStateEvent` / `AddAnimationNotifyEventByName`
 *     which is the documented public surface for notify authoring
 *     and handles the `AnimNotifyTracks` re-link on the base class.
 *     The notify track is auto-added through `AddAnimationNotifyTrack`
 *     when the named track does not yet exist.
 *   - `add_curve`: declare a typed animation curve (`Float` / `Vector`
 *     / `Transform`) on UAnimSequenceBase through
 *     `UAnimationBlueprintLibrary::AddCurve`, then optionally seed it
 *     with `[time, value]` keyframes through `AddFloatCurveKeys` /
 *     `AddVectorCurveKeys` / `AddTransformationCurveKeys`. The
 *     keyframe shape per `curve_type` is: `Float` -> `[time]` (number)
 *     plus a parallel `values` array, or per-row `[time, value]`;
 *     `Vector` -> `[time, [x, y, z]]`; `Transform` -> `[time, location,
 *     rotation, scale]` triples (each as `[x, y, z]`).
 *   - `add_sync_marker`: append an `FAnimSyncMarker` to a UAnimSequence's
 *     notify track through
 *     `UAnimationBlueprintLibrary::AddAnimationSyncMarker`. The named
 *     notify track is auto-created through `AddAnimationNotifyTrack`
 *     when missing, mirroring the `add_notify` shape.
 *   - `add_blendspace_sample`: append a sample to a UBlendSpace or
 *     UBlendSpace1D through `UBlendSpace::AddSample(AnimSequence,
 *     SampleValue)`. `blendspace` is the target blendspace asset path;
 *     `animation` is a UAnimSequence path; `sample_value` is a 1D
 *     `[x]` array for UBlendSpace1D or a 2D `[x, y]` array for
 *     UBlendSpace. Refuses values outside the axis range and refuses
 *     animations whose additive type does not match the existing
 *     samples.
 *   - `replace_blendspace_sample`: swap the UAnimSequence on an
 *     existing sample at `sample_index` through
 *     `UBlendSpace::ReplaceSampleAnimation`. Pass `animation` (a
 *     `/Game/...` UAnimSequence path) to bind a new sequence; pass
 *     `clear=true` or an empty / `none` animation string to unbind.
 *     The replacement sequence's skeleton must match the blendspace's
 *     target skeleton (UBlendSpace::IsAnimationCompatibleWithSkeleton)
 *     and its additive type must match the existing samples
 *     (UBlendSpace::IsAnimationCompatible).
 *   - `delete_blendspace_sample`: remove the sample at `sample_index`
 *     through `UBlendSpace::DeleteSample`. Bounds-checked against the
 *     blendspace's current sample count.
 *   - `set_root_motion`: write the four canonical root-motion
 *     UPROPERTYs on a UAnimSequence: `bEnableRootMotion` (bool,
 *     required), `RootMotionRootLock` (ERootMotionRootLock token:
 *     `RefPose` / `AnimFirstFrame` / `Zero`; optional), `bForceRootLock`
 *     (optional), `bUseNormalizedRootMotionScale` (optional). The
 *     four fields are public UPROPERTYs on UAnimSequence under
 *     Category=RootMotion. PostEditChange + MarkPackageDirty fire
 *     after the writes so any open editor refreshes; saves on
 *     success unless `save=false`.
 *   - `add_notify_state`: append an FAnimNotifyEvent backed by a
 *     UAnimNotifyState subclass on a notify track of a
 *     UAnimSequenceBase. Pairs with `add_notify` which only handles
 *     the point-notify shape (UAnimNotify). Routes through the public
 *     `UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent`
 *     entry point with start time and duration converted from frame
 *     counts through the asset's sampling frame rate (matches
 *     `add_notify`'s frame-vs-time precedence). The named notify track
 *     auto-creates through `AddAnimationNotifyTrack` when missing.
 *     The resolved class must derive from UAnimNotifyState (the
 *     UAnimNotify branch belongs on `add_notify`).
 *   - `set_compression_scheme`: writes the compression slot on a
 *     UAnimSequence. The modern UE5 compression pipeline keys off
 *     `UAnimSequence::BoneCompressionSettings` (a
 *     UAnimBoneCompressionSettings DataAsset whose `Codecs` array
 *     holds the UAnimBoneCompressionCodec subclass instances the
 *     engine runs in turn). Callers pass one of two shapes:
 *       - `compression_settings`: a `/Game/...` path to an existing
 *         UAnimBoneCompressionSettings DataAsset, written into the
 *         BoneCompressionSettings slot directly.
 *       - `compression_codec` (alias `compression_scheme` /
 *         `scheme`): a class path or short class name for a
 *         UAnimBoneCompressionCodec subclass (the legacy
 *         `UAnimCompress_BitwiseCompressOnly` /
 *         `UAnimCompress_RemoveLinearKeys` /
 *         `UAnimCompress_RemoveTrivialKeys` codecs derive from this
 *         interface). We NewObject a UAnimBoneCompressionSettings as
 *         a per-sequence subobject (outered to the sequence's
 *         package) and assign one codec subobject of the requested
 *         class into the `Codecs` array. The per-sequence settings
 *         object isolates the codec choice from any other sequence
 *         on the project so flipping the slot does not change every
 *         other animation.
 *     The optional `request_compile` flag (alias `recompile`)
 *     triggers `UAnimSequence::RequestAnimCompression` with the
 *     default FRequestAnimCompressionParams (synchronous) so the
 *     saved asset reflects the new scheme; without it the engine
 *     resamples lazily through the derived data cache. Saves the
 *     sequence on success unless `save=false`.
 *
 * Inputs (set_rate_scale):
 *   - asset: short asset name or `/Game/...` UAnimSequenceBase path.
 *     Required.
 *   - rate_scale: float. Required. Negative values are accepted (the
 *     engine plays the sequence in reverse for negative scales).
 *   - save: persist the asset on success. Default True.
 *
 * Inputs (set_additive):
 *   - asset: short asset name or `/Game/...` UAnimSequence path.
 *     Required. Refuses non-UAnimSequence assets.
 *   - additive_type: one of `none` / `local_space` /
 *     `rotation_offset_mesh_space` (case-insensitive). Required.
 *   - ref_pose_type: one of `none` / `ref_pose` / `anim_scaled` /
 *     `anim_frame`. Optional; defaults to leaving the existing
 *     setting alone.
 *   - ref_pose_seq: `/Game/...` UAnimSequence path. Optional;
 *     applied when ref_pose_type is anim_scaled / anim_frame.
 *   - ref_frame_index: integer frame index. Optional; applied when
 *     ref_pose_type is anim_frame.
 *   - save: persist the asset on success. Default True.
 *
 * Inputs (add_notify):
 *   - asset: short asset name or `/Game/...` UAnimSequenceBase path.
 *     Required. Accepts UAnimSequence and UAnimMontage.
 *   - track: notify track FName. Required. Auto-created when missing.
 *   - frame: integer frame index, or
 *   - time: float seconds. One of `frame` / `time` is required.
 *     `frame` is converted to seconds through the asset's sampling
 *     frame rate when both are present we prefer `frame`.
 *   - notify_class: short name (`UAnimNotify_PlaySound`,
 *     `AnimNotify_PlayMontageNotify`, etc.) or full
 *     `/Script/Module.ClassName` / `/Game/...` Blueprint class path.
 *     Resolves to either a UAnimNotify or UAnimNotifyState subclass.
 *     When omitted we treat the entry as a custom-event notify and
 *     write only the FAnimNotifyEvent.NotifyName (`event_name`).
 *   - event_name: FName for the custom-event notify. Optional;
 *     defaults to the resolved notify class' short name when a class
 *     is given.
 *   - duration: float seconds. Required for UAnimNotifyState
 *     subclasses; ignored otherwise.
 *   - save: persist the asset on success. Default True.
 *
 * Returns a payload with at least `op`, `asset`, `path`, `class`,
 * and op-specific fields. `set_rate_scale` returns the previous and
 * new values; `set_additive` returns the resolved enum tokens;
 * `add_notify` returns the new notify's `name`, `track_name`,
 * `notify_class`, `notify_kind` (instant / state / event), and the
 * resolved `time` in seconds.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UAnimSequenceBase::RateScale / SortNotifies.
 *   - UAnimSequence::AdditiveAnimType / RefPoseType / RefPoseSeq /
 *     RefFrameIndex (UPROPERTY direct write on the asset).
 *   - UAnimationBlueprintLibrary::AddAnimationNotifyEvent /
 *     AddAnimationNotifyStateEvent / AddAnimationNotifyEventByName /
 *     AddAnimationNotifyTrack / IsValidAnimNotifyTrackName for the
 *     notify path.
 *   - UEditorAssetLibrary::LoadAsset / SaveAsset for the asset
 *     resolve / persist cycle.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftAnimationEditCommands
{
public:
    FSproftAnimationEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSetRateScale(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAdditive(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNotify(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddCurve(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSyncMarker(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReplaceBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddMetadataCurve(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetRootMotion(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddNotifyState(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetCompressionScheme(const TSharedPtr<FJsonObject>& Params);
};
