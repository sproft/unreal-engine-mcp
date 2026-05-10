#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: sequencer_edit (read + edit slice)
 *
 * Multi-op tool keyed by `op`:
 *   - `inspect` (default): read-only structured dump of a
 *     ULevelSequence (or any UMovieSceneSequence subclass). Returns
 *     master tracks, sections, possessables, spawnables, plus tick /
 *     display frame rates and the playback range.
 *   - `create_level_sequence`: create a new ULevelSequence asset at a
 *     `/Game/...` package path with default tick / display rates and
 *     an empty MovieScene through ULevelSequence::Initialize.
 *   - `add_possessable`: bind a named editor-world actor to an
 *     existing ULevelSequence.
 *   - `add_track`: add a UMovieSceneTrack of a chosen subclass. Master
 *     tracks (e.g. UMovieSceneCameraCutTrack) attach via
 *     `UMovieScene::AddMasterTrack`; binding-scoped tracks (e.g.
 *     UMovieScene3DTransformTrack) require a target binding GUID or
 *     possessable name and attach via `UMovieScene::AddTrack`.
 *   - `add_section`: append a section to a chosen track at an explicit
 *     start frame + duration. Uses `UMovieSceneTrack::CreateNewSection` +
 *     `AddSection` so the track decides its native section subclass.
 *   - `move_section`: move an existing section on a chosen track to a
 *     new (start frame, duration) pair through `UMovieSceneSection::SetRange`
 *     plus `MarkAsChanged`. The target section is resolved either by
 *     `section_index` (into the track's `GetAllSections()` array) or
 *     by track / binding scoping plus the index.
 *
 * Inputs (inspect):
 *   - sequence: short asset name or full `/Game/...` Level Sequence
 *     path. Resolves through `UEditorAssetLibrary::LoadAsset` and
 *     accepts any UMovieSceneSequence subclass.
 *   - include_tracks / include_camera_cut_track / include_sections /
 *     include_possessables / include_spawnables: see the read slice.
 *   - max_sections_per_track: cap on per-track section emission.
 *
 * Inputs (create_level_sequence):
 *   - sequence: target `/Game/...` package path. Required.
 *   - overwrite: replace an existing asset at the path. Default
 *     False.
 *   - save: save the new package after creation. Default True.
 *
 * Inputs (add_possessable):
 *   - sequence: short asset name or full `/Game/...` Level Sequence
 *     path. Required.
 *   - actor: actor name (matched against GetName() first and
 *     GetActorLabel() second) for the editor-world actor to bind.
 *     Required.
 *   - binding_name: friendly name for the FMovieScenePossessable.
 *     Optional; falls back to the actor's GetActorLabel() when
 *     omitted.
 *   - save: save the asset after the edit. Default True.
 *
 * Inputs (add_track):
 *   - sequence: BT asset path / short name. Required.
 *   - track_class: UMovieSceneTrack subclass. Accepts a short token
 *     (`transform`, `camera_cut`, `skeletal_animation`, `audio`,
 *     `event`, `float`, `subscene`, `bool`, `byte`), a full
 *     `/Script/Module.ClassName` path, or `MovieScene...Track` style
 *     class name. Required.
 *   - binding: optional binding GUID (string) for a binding-scoped
 *     track. Mutually exclusive with `actor` / `possessable`.
 *   - actor: optional actor name; the call resolves the actor's
 *     binding by `FMovieScenePossessable::GetName()` first, then
 *     binding-name match.
 *   - possessable: optional possessable name (matched against
 *     `FMovieScenePossessable::GetName()`).
 *   - save: save the asset after the edit. Default True.
 *
 * Inputs (add_section):
 *   - sequence: required.
 *   - track: track FName / display name (case-insensitive substring
 *     match). Required. The first match wins; pass a more specific
 *     name when ambiguous.
 *   - binding: optional binding GUID to disambiguate when a track of
 *     the same class lives under a possessable.
 *   - start_frame: integer tick-resolution start frame. Required.
 *   - duration_frames: integer tick-resolution duration. Required.
 *   - save: save the asset after the edit. Default True.
 *
 * Inputs (move_section):
 *   - sequence: required.
 *   - track: required. Same matching rules as `add_section`.
 *   - binding: optional binding GUID disambiguator.
 *   - section_index: integer index into the track's
 *     `GetAllSections()` array. Required.
 *   - start_frame: integer tick-resolution start frame. Required.
 *   - duration_frames: integer tick-resolution duration. Required.
 *   - save: save the asset after the edit. Default True.
 *
 * Read-only `inspect` does not mutate the asset; the two edit ops
 * touch the package and dirty it for save.
 *
 * Clean-room implementation derived from the public UE5 Sequencer API:
 *   - UMovieSceneSequence::GetMovieScene / BindPossessableObject.
 *   - ULevelSequence::Initialize for the default-rates empty
 *     MovieScene that Sequencer expects.
 *   - UMovieScene::GetTracks / GetCameraCutTrack /
 *     GetTickResolution / GetDisplayRate / GetPlaybackRange (read).
 *   - UMovieScene::AddPossessable for the binding.
 *   - UMovieSceneTrack::GetAllSections / GetDisplayName (read).
 *   - UMovieSceneSection::GetRange / HasStartFrame / HasEndFrame /
 *     GetInclusiveStartFrame / GetExclusiveEndFrame (read).
 *   - UMovieScene::GetPossessableCount / GetPossessable /
 *     GetSpawnableCount / GetSpawnable (read).
 *   - FMovieScenePossessable::GetGuid / GetName /
 *     GetPossessedObjectClass / GetParent (read).
 *   - FMovieSceneSpawnable::GetGuid / GetName / GetObjectTemplate
 *     (read).
 *   - CreatePackage / NewObject / FAssetRegistryModule::AssetCreated /
 *     UEditorAssetLibrary::SaveAsset for the create / save side.
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
    TSharedPtr<FJsonObject> HandleCreateLevelSequence(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddPossessable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddTrack(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSection(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMoveSection(const TSharedPtr<FJsonObject>& Params);
};
