#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: gas_edit (read + small edit slice)
 *
 * Multi-op tool over Gameplay Ability System assets, keyed by `op`.
 * The default op is `inspect` and matches the read-only slice shipped
 * earlier:
 *   - UGameplayAbility (or a Blueprint with a UGameplayAbility CDO):
 *     dumps ability tags, cancel tags, block tags, activation owned /
 *     required / blocked tags, source / target required / blocked tags,
 *     CostGameplayEffectClass, CooldownGameplayEffectClass, instancing
 *     policy, replication policy, and net execution / security policy.
 *   - UGameplayEffect (or a Blueprint with a UGameplayEffect CDO):
 *     dumps DurationPolicy, DurationMagnitude / MaxDurationMagnitude /
 *     Period (when Has-Duration), the modifier list, the cached asset /
 *     granted / blocked-ability tags through the public accessors that
 *     the GE component model migrated to in 5.3+, and the GameplayCues.
 *   - UAttributeSet (or a Blueprint with a UAttributeSet CDO): walks
 *     the CDO's FProperty list filtering on
 *     `FGameplayAttribute::IsSupportedProperty`.
 *
 * Edit ops:
 *   - `create_gameplay_ability`: NewObject's a UBlueprint at a `/Game/...`
 *     path with a UGameplayAbility-derived parent class (default
 *     `/Script/GameplayAbilities.GameplayAbility`). Compiles and saves
 *     by default.
 *   - `create_gameplay_effect`: NewObject's a UBlueprint at a `/Game/...`
 *     path with a UGameplayEffect-derived parent class (default
 *     `/Script/GameplayAbilities.GameplayEffect`). Optional
 *     `duration_policy` (`instant` / `has_duration` / `infinite`) plus an
 *     optional `duration_magnitude` (literal float on a HasDuration
 *     effect's ScalableFloat magnitude) writes through the CDO before
 *     the first compile.
 *   - `set_gameplay_tags`: tag-container mutation on either asset shape.
 *   - `add_modifier`: append an FGameplayModifierInfo to a GE's
 *     `Modifiers` array. `attribute` accepts `<attribute_set_path>:<attr_name>`
 *     or a separate `attribute_set` + `attribute_name` pair; the
 *     resolver falls back to a substring match across loaded
 *     UAttributeSet subclasses for the canonical short-name case.
 *     `modifier_op` accepts `Add` / `Additive` / `add_base` /
 *     `Multiply` / `multiply_additive` / `Override` /
 *     `Division` / `divide_additive` / `multiply_compound` /
 *     `add_final` (case-insensitive). `magnitude` is a literal float
 *     wrapped into an FScalableFloat.
 *   - `remove_modifier_at`: remove an FGameplayModifierInfo at index.
 *   - `set_attribute_default`: write the base value of an attribute
 *     on a UAttributeSet (or its Blueprint CDO).
 *
 * Inputs (op-dependent):
 *   - asset:           short asset name or full `/Game/...` path (inspect, set_gameplay_tags).
 *   - path:            target /Game/... package path (create_*).
 *   - parent_class:    UClass path / short name (create_*; defaults to the canonical base).
 *   - duration_policy: `instant` / `has_duration` / `infinite` (create_gameplay_effect).
 *   - duration_magnitude: literal float (create_gameplay_effect; HasDuration only).
 *   - tags:            object dict with named tag containers (set_gameplay_tags).
 *
 * Heavier ops (modifier add / remove, cost / cooldown rebind, attribute
 * default override, GameplayCue authoring) remain on the backlog.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UGameplayAbility property surface (CancelAbilitiesWithTag etc.).
 *   - UGameplayEffect public AddComponent / FindOrAddComponent template
 *     plus the asset / target / block-tag GE-component public mutators.
 *   - UAttributeSet + FGameplayAttribute::IsSupportedProperty.
 *   - FKismetEditorUtilities::CreateBlueprint for the BP factory side.
 *
 * No code from the proprietary FlopAI plugin is used.
 */
class UNREALMCP_API FSproftGasEditCommands
{
public:
    FSproftGasEditCommands();

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleGasEdit(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateGameplayAbility(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateGameplayEffect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetGameplayTags(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddModifier(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveModifierAt(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAttributeDefault(const TSharedPtr<FJsonObject>& Params);
};
