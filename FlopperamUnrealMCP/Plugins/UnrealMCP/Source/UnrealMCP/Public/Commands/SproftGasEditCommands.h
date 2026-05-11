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
 *   - `set_modifier_magnitude`: rewrite the magnitude on an existing
 *     `FGameplayModifierInfo`. The previous `add_modifier` op only
 *     emitted the scalable-float variant of
 *     `FGameplayEffectModifierMagnitude`; this op extends the
 *     magnitude surface to the other documented variants. The
 *     `magnitude_type` token selects the variant
 *     (`scalable` / `scalable_float`, `attribute_based`,
 *     `set_by_caller`, `custom_calculation_class`). Per-variant
 *     payload fields:
 *       - scalable_float: `value` (literal float written into
 *         `FScalableFloat::Value`).
 *       - attribute_based: `attribute` / `attribute_set` +
 *         `attribute_name` (same resolver as `add_modifier`),
 *         optional `source` (`source` / `target`, default
 *         source), optional `snapshot` (default false), optional
 *         `coefficient` (literal float, written into
 *         `Coefficient.Value`), optional `pre_multiply` /
 *         `post_multiply` (literal floats), optional
 *         `calculation_type` token
 *         (`magnitude` / `base_value` / `bonus_magnitude` /
 *         `magnitude_evaluated_up_to_channel`).
 *       - set_by_caller: `data_name` (FName for `DataName`) and / or
 *         `data_tag` (FGameplayTag string for `DataTag`).
 *       - custom_calculation_class: `calculation_class`
 *         (`/Script/Module.ClassName` path or `/Game/...` BP class
 *         path for a `UGameplayModMagnitudeCalculation` subclass),
 *         optional `coefficient` / `pre_multiply` / `post_multiply`
 *         literal floats.
 *   - `set_attribute_default`: write the base value of an attribute
 *     on a UAttributeSet (or its Blueprint CDO).
 *   - `set_ability_cost`: rebind a UGameplayAbility's
 *     `CostGameplayEffectClass` to a chosen UGameplayEffect-derived
 *     class (or `none` / empty / explicit `clear=true` to clear the
 *     binding). Routes through reflection (`FClassProperty` +
 *     `ContainerPtrToValuePtr<TSubclassOf<UObject>>`) so the write
 *     stays compatible with the 5.7 visibility tightening that
 *     demoted the field from public to protected.
 *   - `set_ability_cooldown`: same shape against
 *     `CooldownGameplayEffectClass`.
 *   - `create_cue_notify`: NewObject's a UGameplayCueNotify_Static or
 *     AGameplayCueNotify_Actor (parent class chosen via the
 *     `parent_class` field, default
 *     `/Script/GameplayAbilities.GameplayCueNotify_Static`) at a
 *     `/Game/...` path. Optional `cue_tag` writes the asset's
 *     `GameplayCueTag` UPROPERTY through reflection so the asset
 *     opens with a tag already wired (the engine's editor falls back
 *     to `DeriveGameplayCueTagFromAssetName` if the field stays
 *     empty, but the explicit write is the more predictable path).
 *
 * Inputs (op-dependent):
 *   - asset:           short asset name or full `/Game/...` path (inspect, set_gameplay_tags).
 *   - path:            target /Game/... package path (create_*).
 *   - parent_class:    UClass path / short name (create_*; defaults to the canonical base).
 *   - duration_policy: `instant` / `has_duration` / `infinite` (create_gameplay_effect).
 *   - duration_magnitude: literal float (create_gameplay_effect; HasDuration only).
 *   - tags:            object dict with named tag containers (set_gameplay_tags).
 *   - cue_tag:         FGameplayTag string for create_cue_notify (e.g. `GameplayCue.Combat.Hit`).
 *
 * The `set_ability_cue_tag` op ships in this slice as a graph-side
 * authoring path: we spawn a `UK2Node_CallFunction` for
 * `UGameplayAbility::K2_ExecuteGameplayCue` in the ability's event
 * graph and pre-fill the `GameplayCueTag` literal pin. Reuses an
 * existing matching call node if one is already wired for the
 * resolved tag so the op is idempotent.
 *
 * The `add_execution` op appends an `FGameplayEffectExecutionDefinition`
 * to a UGameplayEffect's `Executions` array on the GE CDO.
 * `calculation_class` resolves to a `UGameplayEffectExecutionCalculation`
 * subclass through a `/Script/Module.ClassName` path, a `/Game/...`
 * BP class path, or a bare class name lookup. Optional
 * `passed_in_tags` lands on the entry's `PassedInTags`
 * `FGameplayTagContainer` (each tag added through
 * `RequestGameplayTag(..., /*bErrorIfNotFound=*/false)` so unknown
 * tags surface a warning and skip rather than crash). Recompiles +
 * saves on success.
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
    TSharedPtr<FJsonObject> HandleSetModifierMagnitude(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAttributeDefault(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAbilityCostOrCooldown(const TSharedPtr<FJsonObject>& Params, bool bIsCost);
    TSharedPtr<FJsonObject> HandleCreateCueNotify(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAbilityCueTag(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddExecution(const TSharedPtr<FJsonObject>& Params);
};
