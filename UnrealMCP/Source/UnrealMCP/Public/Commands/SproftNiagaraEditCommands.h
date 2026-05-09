#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: niagara_edit (ultra-minimum cut)
 *
 * One op: `create_niagara_system`. Spawns a `UNiagaraSystem` asset
 * at a `/Game/...` path through `UNiagaraSystemFactoryNew::FactoryCreateNew`'s
 * "no source / no emitters" branch (`InitializeSystem(System, /*bCreateDefaultNodes=*/false)`).
 * No emitters, no parameter store mutations, no module / sim-stage
 * authoring. The bar this slice clears is "the persistent four-skip
 * is broken"; the broader Niagara authoring surface stays on the
 * BACKLOG.
 *
 * Inputs:
 *   - path: `/Game/...` package path. Required.
 *   - overwrite: replace an existing asset at the path. Default false.
 *   - save: save the new asset to disk. Default true.
 *
 * Returns asset name + path + class plus a `saved` flag.
 *
 * Editor-side warning: a Niagara System with no emitters opens
 * cleanly in the Niagara editor but produces a "no emitter"
 * warning in the asset's status banner. That is by design for this
 * minimum-cut slice; the emitter-authoring surface is the next
 * follow-on.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - `UNiagaraSystem` from
 *     `Plugins/FX/Niagara/Source/Niagara/Public/NiagaraSystem.h`.
 *   - `UNiagaraSystemFactoryNew::InitializeSystem(System, false)`
 *     from
 *     `Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraSystemFactoryNew.h`.
 *     The function is `NIAGARAEDITOR_API` so we link it through the
 *     editor-only `NiagaraEditor` module dep we add to .Build.cs.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftNiagaraEditCommands
{
public:
    FSproftNiagaraEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleCreateSystem(const TSharedPtr<FJsonObject>& Params);
};
