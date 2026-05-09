#include "Commands/SproftGasEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Abilities/GameplayAbility.h"
#include "AttributeSet.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    /** Render a gameplay tag container as a sorted JSON array. */
    TArray<TSharedPtr<FJsonValue>> TagContainerToJson(const FGameplayTagContainer& Container)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        TArray<FGameplayTag> Tags;
        Container.GetGameplayTagArray(Tags);
        Tags.Sort([](const FGameplayTag& A, const FGameplayTag& B)
        {
            return A.GetTagName().FastLess(B.GetTagName());
        });
        for (const FGameplayTag& Tag : Tags)
        {
            Out.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        return Out;
    }

    const TCHAR* DurationPolicyToString(EGameplayEffectDurationType Type)
    {
        switch (Type)
        {
        case EGameplayEffectDurationType::Instant:     return TEXT("instant");
        case EGameplayEffectDurationType::Infinite:    return TEXT("infinite");
        case EGameplayEffectDurationType::HasDuration: return TEXT("has_duration");
        default: return TEXT("unknown");
        }
    }

    const TCHAR* ModOpToString(EGameplayModOp::Type Op)
    {
        switch (Op)
        {
        case EGameplayModOp::AddBase:           return TEXT("add_base");
        case EGameplayModOp::MultiplyAdditive:  return TEXT("multiply_additive");
        case EGameplayModOp::DivideAdditive:    return TEXT("divide_additive");
        case EGameplayModOp::MultiplyCompound:  return TEXT("multiply_compound");
        case EGameplayModOp::AddFinal:          return TEXT("add_final");
        case EGameplayModOp::Override:          return TEXT("override");
        default: return TEXT("unknown");
        }
    }

    /** Render a magnitude struct's calculation type as a token. The
     *  underlying enum is private inside the GameplayEffect module so we
     *  fall back to the property's reflected default-text value. */
    FString DescribeMagnitude(const FGameplayEffectModifierMagnitude& Magnitude)
    {
        // Public accessor: returns true and writes the float when the
        // magnitude is a literal Scalable/Float type. Otherwise we tag
        // it as non-literal so the caller knows to query the GE asset
        // for the expression.
        float Resolved = 0.0f;
        if (Magnitude.GetStaticMagnitudeIfPossible(/*Level*/ 1.0f, Resolved))
        {
            return FString::Printf(TEXT("scalable_float:%.6f"), Resolved);
        }
        return TEXT("non_literal");
    }

    UClass* ResolveAssetClass(UObject* Asset)
    {
        if (!Asset)
        {
            return nullptr;
        }
        if (UBlueprint* AsBlueprint = Cast<UBlueprint>(Asset))
        {
            return AsBlueprint->GeneratedClass;
        }
        return Asset->GetClass();
    }

    UObject* ResolveCDO(UObject* Asset)
    {
        if (UClass* Class = ResolveAssetClass(Asset))
        {
            return Class->GetDefaultObject();
        }
        return nullptr;
    }

    void DumpGameplayAbility(UGameplayAbility* Ability, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("kind"), TEXT("gameplay_ability"));
        Out->SetArrayField(TEXT("ability_tags"), TagContainerToJson(Ability->GetAssetTags()));
        Out->SetArrayField(TEXT("cancel_abilities_with_tag"), TagContainerToJson(Ability->CancelAbilitiesWithTag));
        Out->SetArrayField(TEXT("block_abilities_with_tag"), TagContainerToJson(Ability->BlockAbilitiesWithTag));
        Out->SetArrayField(TEXT("activation_owned_tags"), TagContainerToJson(Ability->ActivationOwnedTags));
        Out->SetArrayField(TEXT("activation_required_tags"), TagContainerToJson(Ability->ActivationRequiredTags));
        Out->SetArrayField(TEXT("activation_blocked_tags"), TagContainerToJson(Ability->ActivationBlockedTags));
        Out->SetArrayField(TEXT("source_required_tags"), TagContainerToJson(Ability->SourceRequiredTags));
        Out->SetArrayField(TEXT("source_blocked_tags"), TagContainerToJson(Ability->SourceBlockedTags));
        Out->SetArrayField(TEXT("target_required_tags"), TagContainerToJson(Ability->TargetRequiredTags));
        Out->SetArrayField(TEXT("target_blocked_tags"), TagContainerToJson(Ability->TargetBlockedTags));

        if (UClass* CostClass = Ability->CostGameplayEffectClass.Get())
        {
            Out->SetStringField(TEXT("cost_gameplay_effect_class"), CostClass->GetPathName());
        }
        if (UClass* CooldownClass = Ability->CooldownGameplayEffectClass.Get())
        {
            Out->SetStringField(TEXT("cooldown_gameplay_effect_class"), CooldownClass->GetPathName());
        }

        // Triggers (e.g. on_gameplay_event with a tag).
        TArray<TSharedPtr<FJsonValue>> TriggerArr;
        for (const FAbilityTriggerData& Trigger : Ability->AbilityTriggers)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("trigger_tag"), Trigger.TriggerTag.ToString());
            Row->SetNumberField(TEXT("trigger_source"), static_cast<int32>(Trigger.TriggerSource));
            TriggerArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("triggers"), TriggerArr);
    }

    void DumpGameplayEffect(UGameplayEffect* Effect, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("kind"), TEXT("gameplay_effect"));
        Out->SetStringField(TEXT("duration_policy"), DurationPolicyToString(Effect->DurationPolicy));

        if (Effect->DurationPolicy == EGameplayEffectDurationType::HasDuration)
        {
            Out->SetStringField(TEXT("duration_magnitude"), DescribeMagnitude(Effect->DurationMagnitude));
            Out->SetStringField(TEXT("max_duration_magnitude"), DescribeMagnitude(Effect->MaxDurationMagnitude));
        }

        // The component model migrated tag containers off direct properties
        // in 5.3; the public accessors return the cached snapshot and work
        // for both component-driven and legacy assets.
        Out->SetArrayField(TEXT("asset_tags"), TagContainerToJson(Effect->GetAssetTags()));
        Out->SetArrayField(TEXT("granted_tags"), TagContainerToJson(Effect->GetGrantedTags()));

        FGameplayTagContainer BlockedAbilityTags;
        Effect->GetBlockedAbilityTags(BlockedAbilityTags);
        Out->SetArrayField(TEXT("blocked_ability_tags"), TagContainerToJson(BlockedAbilityTags));

        // Modifier list. Each row carries the attribute it touches and the
        // arithmetic op so a caller can answer "what does this GE actually
        // change" without round-tripping through Python.
        TArray<TSharedPtr<FJsonValue>> ModifierArr;
        for (const FGameplayModifierInfo& Mod : Effect->Modifiers)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("attribute_name"), Mod.Attribute.GetName());
            if (UClass* OwnerClass = (Mod.Attribute.IsValid() ? Mod.Attribute.GetAttributeSetClass() : nullptr))
            {
                Row->SetStringField(TEXT("attribute_set_class"), OwnerClass->GetPathName());
            }
            Row->SetStringField(TEXT("modifier_op"), ModOpToString(Mod.ModifierOp));
            Row->SetStringField(TEXT("magnitude"), DescribeMagnitude(Mod.ModifierMagnitude));
            ModifierArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("modifiers"), ModifierArr);
        Out->SetNumberField(TEXT("modifier_count"), ModifierArr.Num());

        // Executions: each row carries the calculation class + scoped
        // modifier count so a caller can find custom execution logic.
        TArray<TSharedPtr<FJsonValue>> ExecArr;
        for (const FGameplayEffectExecutionDefinition& Exec : Effect->Executions)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            if (Exec.CalculationClass)
            {
                Row->SetStringField(TEXT("calculation_class"), Exec.CalculationClass->GetPathName());
            }
            Row->SetNumberField(TEXT("scoped_modifier_count"), Exec.CalculationModifiers.Num());
            ExecArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("executions"), ExecArr);

        // GameplayCues: trigger tag set + magnitude attribute.
        TArray<TSharedPtr<FJsonValue>> CueArr;
        for (const FGameplayEffectCue& Cue : Effect->GameplayCues)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetArrayField(TEXT("cue_tags"), TagContainerToJson(Cue.GameplayCueTags));
            Row->SetNumberField(TEXT("min_level"), Cue.MinLevel);
            Row->SetNumberField(TEXT("max_level"), Cue.MaxLevel);
            if (Cue.MagnitudeAttribute.IsValid())
            {
                Row->SetStringField(TEXT("magnitude_attribute"), Cue.MagnitudeAttribute.GetName());
            }
            CueArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("gameplay_cues"), CueArr);

        // Stack limit + expiration policy through the public accessors so
        // we ride the component-shim path when the asset has migrated.
        Out->SetNumberField(TEXT("stack_limit_count"), Effect->GetStackLimitCount());
        Out->SetNumberField(TEXT("stack_expiration_policy"), static_cast<int32>(Effect->GetStackExpirationPolicy()));
    }

    void DumpAttributeSet(UAttributeSet* AttributeSet, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("kind"), TEXT("attribute_set"));

        TArray<TSharedPtr<FJsonValue>> AttrArr;
        UClass* SetClass = AttributeSet->GetClass();
        for (TFieldIterator<FProperty> PropIt(SetClass); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop || !FGameplayAttribute::IsSupportedProperty(Prop))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Prop->GetName());
            Row->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());

            // Two storage modes: float (legacy) or FGameplayAttributeData.
            if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
            {
                if (StructProp->Struct == FGameplayAttributeData::StaticStruct()
                    || StructProp->Struct->IsChildOf(FGameplayAttributeData::StaticStruct()))
                {
                    if (FGameplayAttributeData* Data = StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttributeSet))
                    {
                        Row->SetNumberField(TEXT("base_value"), Data->GetBaseValue());
                        Row->SetNumberField(TEXT("current_value"), Data->GetCurrentValue());
                        Row->SetStringField(TEXT("storage"), TEXT("attribute_data"));
                    }
                }
            }
            else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
            {
                if (float* RawValue = FloatProp->ContainerPtrToValuePtr<float>(AttributeSet))
                {
                    Row->SetNumberField(TEXT("base_value"), *RawValue);
                    Row->SetNumberField(TEXT("current_value"), *RawValue);
                    Row->SetStringField(TEXT("storage"), TEXT("float"));
                }
            }

            AttrArr.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("attributes"), AttrArr);
        Out->SetNumberField(TEXT("attribute_count"), AttrArr.Num());
    }
}

FSproftGasEditCommands::FSproftGasEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("gas_edit"))
    {
        return HandleGasEdit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown gas_edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleGasEdit(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath)
        && !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' parameter"));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }

    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    if (!AssetClass || !CDO)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' has no resolved class / CDO"), *AssetPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Asset->GetName());
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
    Result->SetStringField(TEXT("resolved_class"), AssetClass->GetName());
    Result->SetStringField(TEXT("resolved_class_path"), AssetClass->GetPathName());
    Result->SetBoolField(TEXT("is_blueprint"), Cast<UBlueprint>(Asset) != nullptr);

    if (UGameplayAbility* AsAbility = Cast<UGameplayAbility>(CDO))
    {
        DumpGameplayAbility(AsAbility, Result);
        return Result;
    }
    if (UGameplayEffect* AsEffect = Cast<UGameplayEffect>(CDO))
    {
        DumpGameplayEffect(AsEffect, Result);
        return Result;
    }
    if (UAttributeSet* AsAttributeSet = Cast<UAttributeSet>(CDO))
    {
        DumpAttributeSet(AsAttributeSet, Result);
        return Result;
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Asset at '%s' is not a UGameplayAbility / UGameplayEffect / UAttributeSet (resolved class: %s)"), *AssetPath, *AssetClass->GetName()));
}
