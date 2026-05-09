#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: sequencer_edit (read + small edit slice)
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
 *     existing ULevelSequence. Wraps UMovieScene::AddPossessable +
 *     UMovieSceneSequence::BindPossessableObject so the Sequencer UI
 *     picks the binding up the next time the asset opens.
 *
 * Edit-slice future work (track add, section add, section move,
 * spawnable creation, camera-cut creation) stays on BACKLOG.md.
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
};
