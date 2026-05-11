#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: pie_test_scene (assertion kinds)
 *
 * Scene-state assertion harness. Runs against the editor world without
 * driving Play in Editor. Four assertion kinds are supported:
 *
 *   - `actor_exists`: target = actor name (matched against GetName()
 *     first and then GetActorLabel() second). Pass = an actor with
 *     the given name or label is present in the current editor world.
 *   - `actor_at_location`: target = actor name, expected = `[x, y, z]`
 *     world-space location, optional `tolerance` = number (defaults
 *     to 1.0 cm). Pass = the resolved actor's GetActorLocation is
 *     within `tolerance` of the expected vector.
 *   - `actor_overlapping_tag`: target = actor name, expected = an
 *     FName tag string. Pass = the resolved actor's `Tags` array
 *     contains that FName. Despite the historical "PIE-only" framing,
 *     `AActor::Tags` is set in the editor world too, so this kind
 *     answers statically against the loaded level.
 *   - `var_equals`: target = actor name, expected = either a flat
 *     `{var, value}` dict or a `{var, value}` row. Pass = the
 *     resolved actor's UPROPERTY (looked up by FName) ImportText-
 *     matches the canonicalized representation of `value`. Works
 *     against any read-only actor property (transform fields,
 *     Blueprint-exposed variables, gameplay tags, FString fields)
 *     in the editor world without needing PIE.
 *   - `actor_has_class`: target = actor name, expected = a class
 *     path (`/Script/Module.ClassName`, a `/Game/...` Blueprint
 *     class path optionally suffixed `_C`, or a bare short class
 *     name probed against the loaded class set). Pass = the
 *     resolved actor's `GetClass()` matches the expected class or
 *     is a subclass. Editor-world only; no PIE drive.
 *   - `actor_tag_count`: target = actor name, expected = an
 *     integer. Pass = the resolved actor's `Tags.Num()` equals the
 *     expected count. Useful for verifying that an editor-world
 *     mutation pass left exactly the right tag set.
 *   - `level_actor_count`: target = a class path (same shape as
 *     `actor_has_class`), expected = an integer. Pass = the
 *     editor world contains exactly that many actors of the
 *     resolved class (subclasses included; mirrors
 *     `UGameplayStatics::GetAllActorsOfClass`).
 *   - `actor_distance`: target = first actor name, `target_b`
 *     (or `other`) = second actor name, `max_distance` (or
 *     `expected`) = upper-bound distance in cm, optional
 *     `tolerance` (defaults 0.0). Pass = `FVector::Dist(A.Loc,
 *     B.Loc) <= max_distance + tolerance`. Editor-world only;
 *     no PIE drive. Useful for proximity assertions in level
 *     setup tests (the player start sits within N cm of the
 *     spawn marker, the patrol target lives within range of
 *     the AI's home).
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
 *   - AActor::Tags array contains() check for the tag overlap.
 *   - FProperty::FindPropertyByName + ImportText / ExportText for the
 *     var_equals path.
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
