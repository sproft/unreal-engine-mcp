#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: sound_asset_edit (small variant)
 *
 * Three ops on Sound Cue assets, keyed by `op`:
 *   - `create_sound_cue`: NewObject's a `USoundCue` at a `/Game/...`
 *     package path. Optional `sound_wave` parameter loads the named
 *     `USoundWave` and wires a single `USoundNodeWavePlayer` into the
 *     cue's `FirstNode` slot, mirroring the editor's
 *     "right-click sound wave -> Create Cue" shortcut. Without
 *     `sound_wave` the asset ships with `FirstNode = nullptr` and a
 *     blank graph.
 *   - `add_sound_node_wave_player`: resolves an existing
 *     `USoundCue` and a target `USoundWave`, calls the
 *     `USoundCue::ConstructSoundNode<USoundNodeWavePlayer>` factory,
 *     binds the wave through `USoundNodeWavePlayer::SetSoundWave`,
 *     and (when `connect_to_root=true`, the default) writes the new
 *     node into the cue's `FirstNode` slot. `LinkGraphNodesFromSoundNodes`
 *     refreshes the editor graph so the SoundCue editor opens cleanly.
 *   - `set_attenuation`: writes `AttenuationSettings` (the
 *     `USoundAttenuation` ref on USoundBase) on a target `USoundCue`.
 *     `attenuation` accepts a `/Game/...` USoundAttenuation path or
 *     null / empty string to clear the override.
 *
 * The full SoundCue node-graph authoring surface (mixer, modulator,
 * delay / loop / branch composites, attenuation node, distance
 * crossfade) stays on the BACKLOG.
 *
 * Inputs (op-dependent):
 *   - path:           target /Game/... package path. Required.
 *   - sound_wave:     `/Game/...` path to a USoundWave or short name.
 *                     Optional for `create_sound_cue`; required for
 *                     `add_sound_node_wave_player`.
 *   - attenuation:    `/Game/...` path to a USoundAttenuation, or
 *                     null / empty to clear. `set_attenuation` only.
 *   - connect_to_root: when true (default), the new wave player is
 *                     wired into FirstNode. When false the node is
 *                     constructed but left detached.
 *   - overwrite:      reuse an existing asset at the path on
 *                     `create_sound_cue`. Default false.
 *   - save:           save after the edit. Default true.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `USoundCue` from Sound/SoundCue.h.
 *   - `USoundCue::ConstructSoundNode<T>` template factory.
 *   - `USoundNodeWavePlayer::SetSoundWave` from
 *     Sound/SoundNodeWavePlayer.h.
 *   - `USoundCue::LinkGraphNodesFromSoundNodes` for the editor
 *     graph refresh.
 *   - `USoundBase::AttenuationSettings` UPROPERTY for the
 *     attenuation rebind.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftSoundAssetEditCommands
{
public:
    FSproftSoundAssetEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCreateSoundCue(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSoundNodeWavePlayer(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAttenuation(const TSharedPtr<FJsonObject>& Params);
};
