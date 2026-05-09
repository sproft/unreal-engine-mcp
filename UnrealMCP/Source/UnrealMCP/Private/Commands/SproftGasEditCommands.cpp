#include "Commands/SproftGasEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Abilities/GameplayAbility.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AttributeSet.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
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

    bool ParseDurationPolicy(const FString& Token, EGameplayEffectDurationType& OutPolicy)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("instant"))
        {
            OutPolicy = EGameplayEffectDurationType::Instant;
            return true;
        }
        if (Lower == TEXT("infinite"))
        {
            OutPolicy = EGameplayEffectDurationType::Infinite;
            return true;
        }
        if (Lower == TEXT("has_duration") || Lower == TEXT("hasduration") || Lower == TEXT("duration"))
        {
            OutPolicy = EGameplayEffectDurationType::HasDuration;
            return true;
        }
        return false;
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

    /** Split a `/Game/Subdir/AssetName` path into directory + asset name. */
    void SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
    {
        FString Trim = InPath;
        Trim.TrimEndInline();
        Trim.RemoveFromEnd(TEXT("/"));

        int32 LastSlash = INDEX_NONE;
        if (Trim.FindLastChar('/', LastSlash))
        {
            OutPackageDir = Trim.Left(LastSlash + 1);
            OutAssetName = Trim.Mid(LastSlash + 1);
        }
        else
        {
            OutPackageDir = TEXT("/Game/");
            OutAssetName = Trim;
        }

        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Resolve a parent class for the create_* ops. Accepts:
     *    - empty string (caller falls back to the canonical default).
     *    - full `/Script/Module.ClassName` path.
     *    - `/Game/...` Blueprint asset path (auto-suffixed with `_C`).
     *    - short class name probed against in-memory classes plus a
     *      `/Script/GameplayAbilities.<Name>` fallback.
     */
    UClass* ResolveCreateParentClass(const FString& Input, UClass* DefaultClass)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return DefaultClass;
        }

        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Trimmed))
            {
                return Loaded;
            }
        }
        if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Trimmed;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                return Loaded;
            }
        }

        if (UClass* Found = FindObject<UClass>(nullptr, *Trimmed))
        {
            return Found;
        }
        const FString GASPath = FString::Printf(TEXT("/Script/GameplayAbilities.%s"), *Trimmed);
        if (UClass* Loaded = LoadClass<UObject>(nullptr, *GASPath))
        {
            return Loaded;
        }
        return nullptr;
    }

    /** Pull a string array out of a JSON object field. Accepts a JSON
     *  array of strings or a single string. Returns true if the field
     *  was present (even when empty). */
    bool TryReadTagArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& OutTags)
    {
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Obj->TryGetArrayField(FieldName, AsArray) && AsArray)
        {
            for (const TSharedPtr<FJsonValue>& Entry : *AsArray)
            {
                if (Entry.IsValid() && Entry->Type == EJson::String)
                {
                    OutTags.Add(Entry->AsString());
                }
            }
            return true;
        }
        FString Single;
        if (Obj->TryGetStringField(FieldName, Single))
        {
            OutTags.Add(Single);
            return true;
        }
        return false;
    }

    /** Resolve a tag string through `UGameplayTagsManager::RequestGameplayTag`
     *  (loose lookup so unregistered tag strings still build a runtime
     *  FGameplayTag through the dynamic path). */
    FGameplayTag ResolveOrRequestTag(const FString& TagString)
    {
        UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
        const FName TagName(*TagString);
        FGameplayTag Tag = Manager.RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);
        if (Tag.IsValid())
        {
            return Tag;
        }
        // Fallback: build a synthetic tag through the static helper so a
        // caller writing a tag that has not been registered yet still
        // produces a useful container for downstream evaluation.
        return FGameplayTag::RequestGameplayTag(TagName, /*ErrorIfNotFound=*/false);
    }

    /** Build a tag container from an array of string entries, surfacing
     *  unresolved entries on `OutSkipped`. */
    FGameplayTagContainer BuildContainer(const TArray<FString>& TagStrings, TArray<TSharedPtr<FJsonValue>>& OutSkipped, const FString& Field)
    {
        FGameplayTagContainer Container;
        for (const FString& Tag : TagStrings)
        {
            if (Tag.IsEmpty())
            {
                continue;
            }
            const FGameplayTag Resolved = ResolveOrRequestTag(Tag);
            if (Resolved.IsValid())
            {
                Container.AddTag(Resolved);
            }
            else
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("field"), Field);
                Skip->SetStringField(TEXT("tag"), Tag);
                Skip->SetStringField(TEXT("reason"), TEXT("unresolved_tag"));
                OutSkipped.Add(MakeShared<FJsonValueObject>(Skip));
            }
        }
        return Container;
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

    /** Common Blueprint-creation back end used by both create ops. */
    UBlueprint* CreateGasBlueprint(const FString& AssetPath, UClass* ParentClass, bool bOverwrite, FString& OutError)
    {
        if (!AssetPath.StartsWith(TEXT("/Game/")))
        {
            OutError = FString::Printf(TEXT("Asset path '%s' must start with /Game/"), *AssetPath);
            return nullptr;
        }
        FString PackageDir;
        FString AssetName;
        SplitPackagePath(AssetPath, PackageDir, AssetName);
        if (AssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Could not derive asset name from '%s'"), *AssetPath);
            return nullptr;
        }
        const FString PackagePath = PackageDir + AssetName;

        if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
        {
            if (!bOverwrite)
            {
                OutError = FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *PackagePath);
                return nullptr;
            }
            // Reuse the existing Blueprint for an in-place edit pass.
            UBlueprint* Existing = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(PackagePath));
            if (!Existing)
            {
                OutError = FString::Printf(TEXT("Existing asset at '%s' is not a UBlueprint"), *PackagePath);
                return nullptr;
            }
            return Existing;
        }

        if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
        {
            OutError = FString::Printf(TEXT("Cannot create a Blueprint subclass of '%s'"), *ParentClass->GetPathName());
            return nullptr;
        }

        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("Failed to create package at '%s'"), *PackagePath);
            return nullptr;
        }
        Package->FullyLoad();

        UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
            ParentClass, Package, FName(*AssetName), BPTYPE_Normal, FName(TEXT("SproftGasEdit")));
        if (!NewBP)
        {
            OutError = FString::Printf(TEXT("FKismetEditorUtilities::CreateBlueprint returned null for '%s'"), *AssetName);
            return nullptr;
        }
        NewBP->SetFlags(RF_Standalone | RF_Public);
        FAssetRegistryModule::AssetCreated(NewBP);
        Package->MarkPackageDirty();
        return NewBP;
    }
}

FSproftGasEditCommands::FSproftGasEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("gas_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown gas_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("inspect"))
    {
        return HandleGasEdit(Params);
    }
    if (Op == TEXT("create_gameplay_ability"))
    {
        return HandleCreateGameplayAbility(Params);
    }
    if (Op == TEXT("create_gameplay_effect"))
    {
        return HandleCreateGameplayEffect(Params);
    }
    if (Op == TEXT("set_gameplay_tags"))
    {
        return HandleSetGameplayTags(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("gas_edit: unsupported op '%s'"), *Op));
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

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleCreateGameplayAbility(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }

    FString ParentInput;
    Params->TryGetStringField(TEXT("parent_class"), ParentInput);
    if (ParentInput.IsEmpty())
    {
        Params->TryGetStringField(TEXT("parent"), ParentInput);
    }

    UClass* DefaultParent = LoadClass<UObject>(nullptr, TEXT("/Script/GameplayAbilities.GameplayAbility"));
    if (!DefaultParent)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not load /Script/GameplayAbilities.GameplayAbility (is the GameplayAbilities plugin enabled?)"));
    }
    UClass* ParentClass = ResolveCreateParentClass(ParentInput, DefaultParent);
    if (!ParentClass || !ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class '%s' is not a UGameplayAbility subclass"),
                ParentClass ? *ParentClass->GetPathName() : *ParentInput));
    }

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FString Error;
    UBlueprint* NewBP = CreateGasBlueprint(PackagePath, ParentClass, bOverwrite, Error);
    if (!NewBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(NewBP);
    }
    const FString FinalPath = NewBP->GetPathName();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(FinalPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_gameplay_ability"));
    Result->SetStringField(TEXT("name"), NewBP->GetName());
    Result->SetStringField(TEXT("path"), FinalPath);
    Result->SetStringField(TEXT("class"), NewBP->GetClass()->GetName());
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    Result->SetStringField(TEXT("parent_class_short"), ParentClass->GetName());
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleCreateGameplayEffect(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }

    FString ParentInput;
    Params->TryGetStringField(TEXT("parent_class"), ParentInput);
    if (ParentInput.IsEmpty())
    {
        Params->TryGetStringField(TEXT("parent"), ParentInput);
    }

    UClass* DefaultParent = LoadClass<UObject>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
    if (!DefaultParent)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not load /Script/GameplayAbilities.GameplayEffect (is the GameplayAbilities plugin enabled?)"));
    }
    UClass* ParentClass = ResolveCreateParentClass(ParentInput, DefaultParent);
    if (!ParentClass || !ParentClass->IsChildOf(UGameplayEffect::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class '%s' is not a UGameplayEffect subclass"),
                ParentClass ? *ParentClass->GetPathName() : *ParentInput));
    }

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FString Error;
    UBlueprint* NewBP = CreateGasBlueprint(PackagePath, ParentClass, bOverwrite, Error);
    if (!NewBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    // Optional duration policy / magnitude on the CDO before the first compile.
    bool bWroteDurationPolicy = false;
    bool bWroteDurationMagnitude = false;
    FString PolicyToken;
    if (Params->TryGetStringField(TEXT("duration_policy"), PolicyToken))
    {
        EGameplayEffectDurationType Policy = EGameplayEffectDurationType::Instant;
        if (!ParseDurationPolicy(PolicyToken, Policy))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unrecognised duration_policy '%s'. Use 'instant' / 'has_duration' / 'infinite'."),
                    *PolicyToken));
        }
        UClass* GenClass = NewBP->GeneratedClass ? NewBP->GeneratedClass : NewBP->ParentClass;
        UGameplayEffect* CDO = GenClass ? Cast<UGameplayEffect>(GenClass->GetDefaultObject(/*bCreateIfNeeded=*/true)) : nullptr;
        if (!CDO)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Could not access UGameplayEffect CDO on the new Blueprint"));
        }
        CDO->DurationPolicy = Policy;
        bWroteDurationPolicy = true;

        double Magnitude = 0.0;
        if (Params->TryGetNumberField(TEXT("duration_magnitude"), Magnitude))
        {
            if (Policy != EGameplayEffectDurationType::HasDuration)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    TEXT("'duration_magnitude' is only meaningful when duration_policy is 'has_duration'"));
            }
            FScalableFloat Scale;
            Scale.Value = static_cast<float>(Magnitude);
            CDO->DurationMagnitude = FGameplayEffectModifierMagnitude(Scale);
            bWroteDurationMagnitude = true;
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(NewBP);
    }

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(NewBP);
    }
    const FString FinalPath = NewBP->GetPathName();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(FinalPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_gameplay_effect"));
    Result->SetStringField(TEXT("name"), NewBP->GetName());
    Result->SetStringField(TEXT("path"), FinalPath);
    Result->SetStringField(TEXT("class"), NewBP->GetClass()->GetName());
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    Result->SetStringField(TEXT("parent_class_short"), ParentClass->GetName());
    Result->SetBoolField(TEXT("duration_policy_written"), bWroteDurationPolicy);
    Result->SetBoolField(TEXT("duration_magnitude_written"), bWroteDurationMagnitude);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleSetGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
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

    const TSharedPtr<FJsonObject>* TagsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("tags"), TagsObj) || !TagsObj || !TagsObj->IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'tags' object. Expected a dict of named tag containers."));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    TArray<TSharedPtr<FJsonValue>> SkippedArr;

    auto AppliedRecord = [&AppliedArr](const FString& Field, const FGameplayTagContainer& Container)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("field"), Field);
        Row->SetNumberField(TEXT("count"), Container.Num());
        Row->SetArrayField(TEXT("tags"), TagContainerToJson(Container));
        AppliedArr.Add(MakeShared<FJsonValueObject>(Row));
    };

    if (UGameplayAbility* AsAbility = Cast<UGameplayAbility>(CDO))
    {
        // We route every ability tag write through FProperty reflection
        // so the access path is identical for the public AbilityTags
        // field and the protected CancelAbilitiesWithTag etc. fields.
        // The reflected name matches the C++ field name; the JSON shape
        // mirrors snake_case for designer ergonomics.
        struct FAbilityField
        {
            const TCHAR* JsonName;
            const TCHAR* PropertyName;
        };
        static const FAbilityField Fields[] = {
            { TEXT("ability_tags"),                TEXT("AbilityTags") },
            { TEXT("cancel_abilities_with_tag"),   TEXT("CancelAbilitiesWithTag") },
            { TEXT("block_abilities_with_tag"),    TEXT("BlockAbilitiesWithTag") },
            { TEXT("activation_owned_tags"),       TEXT("ActivationOwnedTags") },
            { TEXT("activation_required_tags"),    TEXT("ActivationRequiredTags") },
            { TEXT("activation_blocked_tags"),     TEXT("ActivationBlockedTags") },
            { TEXT("source_required_tags"),        TEXT("SourceRequiredTags") },
            { TEXT("source_blocked_tags"),         TEXT("SourceBlockedTags") },
            { TEXT("target_required_tags"),        TEXT("TargetRequiredTags") },
            { TEXT("target_blocked_tags"),         TEXT("TargetBlockedTags") },
        };

        UClass* AbilityClass = AsAbility->GetClass();
        for (const FAbilityField& Entry : Fields)
        {
            TArray<FString> Tags;
            if (!TryReadTagArray(*TagsObj, Entry.JsonName, Tags))
            {
                continue;
            }
            FStructProperty* Prop = CastField<FStructProperty>(
                FindFProperty<FProperty>(AbilityClass, FName(Entry.PropertyName)));
            if (!Prop || Prop->Struct != TBaseStructure<FGameplayTagContainer>::Get())
            {
                TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
                Skip->SetStringField(TEXT("field"), Entry.JsonName);
                Skip->SetStringField(TEXT("reason"), TEXT("uproperty_not_resolved"));
                SkippedArr.Add(MakeShared<FJsonValueObject>(Skip));
                continue;
            }
            FGameplayTagContainer Container = BuildContainer(Tags, SkippedArr, FString(Entry.JsonName));
            FGameplayTagContainer* TargetPtr =
                Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(AsAbility);
            if (TargetPtr)
            {
                *TargetPtr = Container;
                AppliedRecord(FString(Entry.JsonName), Container);
            }
        }
    }
    else if (UGameplayEffect* AsEffect = Cast<UGameplayEffect>(CDO))
    {
        // The component model exposes inheritable containers through
        // dedicated GameplayEffectComponent subclasses. We add or find
        // each component, set its `Added` container to the requested
        // tags, and route through the public Apply mutator so the
        // cached snapshot on the GE refreshes.
        TArray<FString> AssetTags;
        if (TryReadTagArray(*TagsObj, TEXT("asset_tags"), AssetTags))
        {
            UAssetTagsGameplayEffectComponent& Component =
                AsEffect->FindOrAddComponent<UAssetTagsGameplayEffectComponent>();
            FInheritedTagContainer Inherited;
            Inherited.Added = BuildContainer(AssetTags, SkippedArr, TEXT("asset_tags"));
            Component.SetAndApplyAssetTagChanges(Inherited);
            AppliedRecord(TEXT("asset_tags"), Inherited.Added);
        }
        TArray<FString> GrantedTags;
        if (TryReadTagArray(*TagsObj, TEXT("granted_tags"), GrantedTags))
        {
            UTargetTagsGameplayEffectComponent& Component =
                AsEffect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();
            FInheritedTagContainer Inherited;
            Inherited.Added = BuildContainer(GrantedTags, SkippedArr, TEXT("granted_tags"));
            Component.SetAndApplyTargetTagChanges(Inherited);
            AppliedRecord(TEXT("granted_tags"), Inherited.Added);
        }
        TArray<FString> BlockedTags;
        if (TryReadTagArray(*TagsObj, TEXT("blocked_ability_tags"), BlockedTags))
        {
            UBlockAbilityTagsGameplayEffectComponent& Component =
                AsEffect->FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>();
            FInheritedTagContainer Inherited;
            Inherited.Added = BuildContainer(BlockedTags, SkippedArr, TEXT("blocked_ability_tags"));
            Component.SetAndApplyBlockedAbilityTagChanges(Inherited);
            AppliedRecord(TEXT("blocked_ability_tags"), Inherited.Added);
        }
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayAbility / UGameplayEffect (resolved class: %s)"), *AssetPath, *AssetClass->GetName()));
    }

    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_gameplay_tags"));
    Result->SetStringField(TEXT("name"), Asset->GetName());
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("resolved_class"), AssetClass->GetName());
    Result->SetArrayField(TEXT("applied"), AppliedArr);
    Result->SetArrayField(TEXT("skipped"), SkippedArr);
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
