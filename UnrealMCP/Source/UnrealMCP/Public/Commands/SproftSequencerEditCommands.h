#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: sequencer_edit (read-only first slice)
 *
 * Read-only structured dump of a ULevelSequence (or any
 * UMovieSceneSequence subclass). The hosted Flop tool surface promised
 * a sequencer authoring tool with track / section / camera-cut / event
 * handling. The first slice we ship here is the read side: the master
 * track list with class + name + section count, per-section
 * start / end / duration, the possessables list (one entry per
 * FMovieScenePossessable with binding GUID + name + class) and the
 * spawnables list (one entry per FMovieSceneSpawnable with binding
 * GUID + name + spawn-template class). The edit-side ops (track add,
 * section move, possessable / spawnable swap) remain on the backlog.
 *
 * Operation: single op (`inspect`, default).
 *
 * Required input:
 *   - `sequence`: short asset name or full `/Game/...` Level Sequence
 *     path. Resolves through `UEditorAssetLibrary::LoadAsset` and
 *     accepts any UMovieSceneSequence subclass, not just ULevelSequence.
 *
 * Optional inputs:
 *   - `include_tracks`: emit the master tracks array. Default True.
 *   - `include_camera_cut_track`: emit the camera-cut track stub when
 *     present. Default True.
 *   - `include_sections`: emit per-section start / end / duration on
 *     each track. Default True.
 *   - `include_possessables`: emit the possessables array. Default
 *     True.
 *   - `include_spawnables`: emit the spawnables array. Default True.
 *   - `max_sections_per_track`: cap on per-track section emission.
 *     Default 64; setting `section_truncated: true` on the offending
 *     track when the cap fires.
 *
 * Returns a structured payload with:
 *   - `name`, `path`, `class` for the resolved sequence asset.
 *   - `tick_resolution`, `display_rate` (each as `numerator` /
 *     `denominator` plus a numeric helper).
 *   - `playback_start_frame`, `playback_end_frame`,
 *     `playback_duration_frames` when the sequence has a bounded
 *     playback range.
 *   - `tracks`: array of master tracks. Each track carries `name`
 *     (FName), `display_name`, `class`, `class_path`,
 *     `section_count`, and an optional `sections` array. Each section
 *     dict reports `class`, `class_path`,
 *     `has_start_frame` / `has_end_frame`, the inclusive-start /
 *     exclusive-end frame numbers when bounded, and a
 *     `duration_frames` helper that always lands at zero for
 *     unbounded ranges.
 *   - `camera_cut_track` when present, with the same shape as a
 *     master track.
 *   - `possessables`: array of `{guid, name, class, class_path,
 *     parent_guid}`.
 *   - `spawnables`: array of `{guid, name, template_class,
 *     template_class_path}`.
 *
 * Read-only. We do not mutate the asset and we do not save anything.
 *
 * Clean-room implementation derived from the public UE5 Sequencer API:
 *   - UMovieSceneSequence::GetMovieScene.
 *   - UMovieScene::GetTracks / GetCameraCutTrack /
 *     GetTickResolution / GetDisplayRate / GetPlaybackRange.
 *   - UMovieSceneTrack::GetAllSections / GetDisplayName.
 *   - UMovieSceneSection::GetRange / HasStartFrame / HasEndFrame /
 *     GetInclusiveStartFrame / GetExclusiveEndFrame.
 *   - UMovieScene::GetPossessableCount / GetPossessable /
 *     GetSpawnableCount / GetSpawnable.
 *   - FMovieScenePossessable::GetGuid / GetName /
 *     GetPossessedObjectClass / GetParent.
 *   - FMovieSceneSpawnable::GetGuid / GetName / GetObjectTemplate.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSequencerEditCommands
{
public:
    FSproftSequencerEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleSequencerInspect(const TSharedPtr<FJsonObject>& Params);
};
