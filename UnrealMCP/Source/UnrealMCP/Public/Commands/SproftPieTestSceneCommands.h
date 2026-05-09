#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: pie_test_scene (minimum cut)
 *
 * Scene-state assertion harness. The hosted Flop tool surface promised
 * a full PIE-driven scene assertion runner. The minimum cut we ship
 * here runs against the editor world directly, without driving Play in
 * Editor, and supports two assertion kinds that can both be answered
 * statically:
 *
 *   - `actor_exists`: target = actor name (matched against GetName()
 *     first and then GetActorLabel() second). Pass = an actor with
 *     the given name or label is present in the current editor world.
 *   - `actor_at_location`: target = actor name, expected = `[x, y, z]`
 *     world-space location, optional `tolerance` = number (defaults
 *     to 1.0 cm). Pass = the resolved actor's GetActorLocation is
 *     within `tolerance` of the expected vector.
 *
 * The kinds that need a running PIE world (`var_equals` against a
 * Blueprint instance variable, `actor_overlapping_tag` for overlap-
 * driven gameplay assertions, etc.) are listed on BACKLOG.md and ship
 * in a later pass.
 *
 * Inputs:
 *   - `assertions`: array of assertion specs. Each entry is a dict
 *     `{kind, target, expected?, tolerance?}` where `kind` is one of
 *     the supported tokens above. Required, must be non-empty.
 *
 * Returns a structured payload with:
 *   - `total`: total assertion count.
 *   - `passed`: count of passing assertions.
 *   - `failed`: count of failing assertions.
 *   - `unsupported`: count of assertions whose `kind` we do not
 *     handle in this slice.
 *   - `results`: per-assertion record carrying `index`, `kind`,
 *     `target`, `passed` flag, optional `actual` value, optional
 *     `expected` value, optional `delta` for distance-based kinds,
 *     and `message` describing the outcome.
 *
 * Read-only against the editor world. We do not spawn, mutate, or
 * delete actors; we do not drive Play in Editor; we do not save.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - GEditor->GetEditorWorldContext().World() for the active world.
 *   - AActor::GetName / GetActorLabel for the resolution path used by
 *     `scene_compose` / `actor_inspect`.
 *   - AActor::GetActorLocation for the location read.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftPieTestSceneCommands
{
public:
    FSproftPieTestSceneCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePieTestScene(const TSharedPtr<FJsonObject>& Params);
};
