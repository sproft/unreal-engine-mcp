#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Sproft fork addition: gas_edit (read-only slice)
 *
 * Read-only counterpart to `tag_registry_edit` for the Gameplay Ability
 * System side. Resolves a target asset path to one of:
 *   - UGameplayAbility (or a Blueprint with a UGameplayAbility CDO):
 *     dumps ability tags, cancel tags, block tags, activation owned /
 *     required / blocked tags, source / target required / blocked tags,
 *     CostGameplayEffectClass, CooldownGameplayEffectClass, instancing
 *     policy, replication policy, and net execution / security policy.
 *   - UGameplayEffect (or a Blueprint with a UGameplayEffect CDO):
 *     dumps DurationPolicy, DurationMagnitude / MaxDurationMagnitude /
 *     Period (when Has-Duration), the modifier list (each with
 *     Attribute, ModifierOp, magnitude calculation type), the cached
 *     asset / granted / blocked-ability tags through the public
 *     accessors that the GE component model migrated to in 5.3+, and
 *     the GameplayCues array.
 *   - UAttributeSet (or a Blueprint with a UAttributeSet CDO): walks
 *     the CDO's FProperty list filtering on
 *     FGameplayAttribute::IsSupportedProperty and dumps each attribute's
 *     name, base value, and current value.
 *
 * Inputs:
 *   - asset:           short asset name or full `/Game/...` path.
 *
 * Edit-side ops (tag mutation, modifier add / remove, cost / cooldown
 * rebind, attribute default override) remain on the backlog.
 *
 * Clean-room implementation derived from the public UE5 API:
 *   - UGameplayAbility property surface (CancelAbilitiesWithTag etc.).
 *   - UGameplayEffect public accessors GetAssetTags / GetGrantedTags /
 *     GetBlockedAbilityTags plus the still-public DurationPolicy /
 *     Modifiers / Period / GameplayCues fields.
 *   - UAttributeSet + FGameplayAttribute::IsSupportedProperty.
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
};
