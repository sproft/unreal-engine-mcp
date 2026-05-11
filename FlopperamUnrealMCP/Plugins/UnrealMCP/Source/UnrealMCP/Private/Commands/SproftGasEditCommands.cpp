#include "Commands/SproftGasEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Abilities/GameplayAbility.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AttributeSet.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayEffect.h"
#include "GameplayEffectAttributeCaptureDefinition.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayModMagnitudeCalculation.h"
#include "GameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
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
    void GasEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

        // UE 5.7 tightened the visibility on every reflected tag-
        // container field on UGameplayAbility from public to
        // protected. The fields stay reflected (UPROPERTY-decorated)
        // so we go through the reflection accessor here instead of
        // the direct member; this stays clean-room (we are reading
        // through the public reflection database, not friending the
        // class) and keeps the code compatible with 5.5 / 5.6 builds
        // where the same members were public.
        UClass* AbilityClass = Ability->GetClass();
        auto ReadTagContainerByName = [&](const TCHAR* PropName) -> FGameplayTagContainer
        {
            FStructProperty* StructProp = FindFProperty<FStructProperty>(AbilityClass, FName(PropName));
            if (!StructProp || StructProp->Struct != TBaseStructure<FGameplayTagContainer>::Get())
            {
                return FGameplayTagContainer();
            }
            const FGameplayTagContainer* Ptr = StructProp->ContainerPtrToValuePtr<FGameplayTagContainer>(Ability);
            return Ptr ? *Ptr : FGameplayTagContainer();
        };
        Out->SetArrayField(TEXT("cancel_abilities_with_tag"),
            TagContainerToJson(ReadTagContainerByName(TEXT("CancelAbilitiesWithTag"))));
        Out->SetArrayField(TEXT("block_abilities_with_tag"),
            TagContainerToJson(ReadTagContainerByName(TEXT("BlockAbilitiesWithTag"))));
        Out->SetArrayField(TEXT("activation_owned_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("ActivationOwnedTags"))));
        Out->SetArrayField(TEXT("activation_required_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("ActivationRequiredTags"))));
        Out->SetArrayField(TEXT("activation_blocked_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("ActivationBlockedTags"))));
        Out->SetArrayField(TEXT("source_required_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("SourceRequiredTags"))));
        Out->SetArrayField(TEXT("source_blocked_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("SourceBlockedTags"))));
        Out->SetArrayField(TEXT("target_required_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("TargetRequiredTags"))));
        Out->SetArrayField(TEXT("target_blocked_tags"),
            TagContainerToJson(ReadTagContainerByName(TEXT("TargetBlockedTags"))));

        auto ReadClassByName = [&](const TCHAR* PropName) -> UClass*
        {
            FClassProperty* ClassProp = FindFProperty<FClassProperty>(AbilityClass, FName(PropName));
            if (!ClassProp)
            {
                return nullptr;
            }
            const TSubclassOf<UObject>* Ptr = ClassProp->ContainerPtrToValuePtr<TSubclassOf<UObject>>(Ability);
            return Ptr ? Ptr->Get() : nullptr;
        };
        if (UClass* CostClass = ReadClassByName(TEXT("CostGameplayEffectClass")))
        {
            Out->SetStringField(TEXT("cost_gameplay_effect_class"), CostClass->GetPathName());
        }
        if (UClass* CooldownClass = ReadClassByName(TEXT("CooldownGameplayEffectClass")))
        {
            Out->SetStringField(TEXT("cooldown_gameplay_effect_class"), CooldownClass->GetPathName());
        }

        // Triggers (e.g. on_gameplay_event with a tag). Same visibility
        // tightening on `AbilityTriggers`; reflection-walk the array.
        TArray<TSharedPtr<FJsonValue>> TriggerArr;
        if (FArrayProperty* TriggersProp = FindFProperty<FArrayProperty>(AbilityClass, TEXT("AbilityTriggers")))
        {
            FScriptArrayHelper Helper(TriggersProp, TriggersProp->ContainerPtrToValuePtr<void>(Ability));
            FStructProperty* InnerStructProp = CastField<FStructProperty>(TriggersProp->Inner);
            if (InnerStructProp && InnerStructProp->Struct == FAbilityTriggerData::StaticStruct())
            {
                for (int32 I = 0; I < Helper.Num(); ++I)
                {
                    const FAbilityTriggerData* Trigger =
                        reinterpret_cast<const FAbilityTriggerData*>(Helper.GetRawPtr(I));
                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("trigger_tag"), Trigger->TriggerTag.ToString());
                    Row->SetNumberField(TEXT("trigger_source"), static_cast<int32>(Trigger->TriggerSource));
                    TriggerArr.Add(MakeShared<FJsonValueObject>(Row));
                }
            }
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

    /** Map a user-facing modifier-op token onto EGameplayModOp. Accepts
     *  the canonical 5.x names (`add_base` / `multiply_additive` /
     *  `divide_additive` / `override` / `multiply_compound` / `add_final`)
     *  plus the short tokens the backlog spec calls out (`Add` /
     *  `Multiply` / `Override` / `Division`). */
    bool ParseModOp(const FString& Token, EGameplayModOp::Type& Out)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("add") || Lower == TEXT("additive") || Lower == TEXT("add_base"))
        {
            Out = EGameplayModOp::AddBase;
            return true;
        }
        if (Lower == TEXT("multiply") || Lower == TEXT("multiply_additive")
            || Lower == TEXT("multiplyadditive") || Lower == TEXT("multiplicitive"))
        {
            Out = EGameplayModOp::MultiplyAdditive;
            return true;
        }
        if (Lower == TEXT("divide") || Lower == TEXT("division") || Lower == TEXT("divide_additive")
            || Lower == TEXT("divideadditive"))
        {
            Out = EGameplayModOp::DivideAdditive;
            return true;
        }
        if (Lower == TEXT("override"))
        {
            Out = EGameplayModOp::Override;
            return true;
        }
        if (Lower == TEXT("multiply_compound") || Lower == TEXT("multiplycompound"))
        {
            Out = EGameplayModOp::MultiplyCompound;
            return true;
        }
        if (Lower == TEXT("add_final") || Lower == TEXT("addfinal"))
        {
            Out = EGameplayModOp::AddFinal;
            return true;
        }
        return false;
    }

    /** Resolve a UAttributeSet subclass + attribute name from an input
     *  expressed as either:
     *    - `<set_path>:<attribute_name>` colon-separated single string,
     *    - separate `attribute_set` + `attribute_name` parameters.
     *  Returns nullptr / empty name if either side is unresolvable. */
    UClass* ResolveAttributeSetClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UAttributeSet>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            if (UClass* Loaded = LoadClass<UAttributeSet>(nullptr, *WithSuffix))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UAttributeSet::StaticClass()))
            {
                return Found;
            }
        }
        return nullptr;
    }

    /** Build an FGameplayAttribute from "Set:Name" or "/Path/Set_C:Name"
     *  tokens, falling back to a substring match across loaded
     *  UAttributeSet subclasses for short-name shapes. */
    bool ResolveAttribute(const FString& AttributeToken, const FString& SetTokenOpt,
                          const FString& NameTokenOpt, FGameplayAttribute& Out, FString& OutError)
    {
        FString SetToken = SetTokenOpt;
        FString NameToken = NameTokenOpt;
        if (!AttributeToken.IsEmpty())
        {
            int32 ColonIdx = INDEX_NONE;
            if (AttributeToken.FindLastChar(':', ColonIdx))
            {
                SetToken = AttributeToken.Left(ColonIdx);
                NameToken = AttributeToken.Mid(ColonIdx + 1);
            }
            else if (NameToken.IsEmpty())
            {
                NameToken = AttributeToken;
            }
        }
        if (NameToken.IsEmpty())
        {
            OutError = TEXT("attribute name was empty (use 'attribute_name' or '<set_path>:<name>')");
            return false;
        }

        UClass* SetClass = ResolveAttributeSetClass(SetToken);
        if (!SetClass && !SetToken.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Could not resolve UAttributeSet class '%s'"), *SetToken);
            return false;
        }

        // Without a set hint, sweep loaded UAttributeSet subclasses for
        // the named attribute. The first matching FProperty wins; this
        // is the "I just typed Health" case.
        if (!SetClass)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (!Candidate || !Candidate->IsChildOf(UAttributeSet::StaticClass())
                    || Candidate == UAttributeSet::StaticClass())
                {
                    continue;
                }
                if (FProperty* Prop = FindFProperty<FProperty>(Candidate, FName(*NameToken)))
                {
                    if (FGameplayAttribute::IsSupportedProperty(Prop))
                    {
                        Out.SetUProperty(Prop);
                        return true;
                    }
                }
            }
            OutError = FString::Printf(TEXT("No loaded UAttributeSet has attribute '%s'"), *NameToken);
            return false;
        }
        FProperty* Prop = FindFProperty<FProperty>(SetClass, FName(*NameToken));
        if (!Prop || !FGameplayAttribute::IsSupportedProperty(Prop))
        {
            OutError = FString::Printf(TEXT("Attribute '%s' on '%s' is not a supported FGameplayAttribute property"),
                *NameToken, *SetClass->GetPathName());
            return false;
        }
        Out.SetUProperty(Prop);
        return true;
    }

    /** Token mirror of `ModOpToString` used in the inspect side, but
     *  dropping the legacy underscore in favour of the canonical spelling
     *  the new tokens map onto. */
    const TCHAR* ModOpToCanonical(EGameplayModOp::Type Op)
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
        GasEdit_SplitPackagePath(AssetPath, PackageDir, AssetName);
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
    if (Op == TEXT("add_modifier"))
    {
        return HandleAddModifier(Params);
    }
    if (Op == TEXT("remove_modifier_at") || Op == TEXT("remove_modifier"))
    {
        return HandleRemoveModifierAt(Params);
    }
    if (Op == TEXT("set_modifier_magnitude") || Op == TEXT("set_magnitude"))
    {
        return HandleSetModifierMagnitude(Params);
    }
    if (Op == TEXT("set_attribute_default"))
    {
        return HandleSetAttributeDefault(Params);
    }
    if (Op == TEXT("set_ability_cost") || Op == TEXT("set_cost"))
    {
        return HandleSetAbilityCostOrCooldown(Params, /*bIsCost=*/true);
    }
    if (Op == TEXT("set_ability_cooldown") || Op == TEXT("set_cooldown"))
    {
        return HandleSetAbilityCostOrCooldown(Params, /*bIsCost=*/false);
    }
    if (Op == TEXT("create_cue_notify") || Op == TEXT("create_gameplay_cue")
        || Op == TEXT("create_cue"))
    {
        return HandleCreateCueNotify(Params);
    }
    if (Op == TEXT("set_ability_cue_tag") || Op == TEXT("set_cue_tag")
        || Op == TEXT("add_cue_tag") || Op == TEXT("bind_cue_tag"))
    {
        return HandleSetAbilityCueTag(Params);
    }
    if (Op == TEXT("add_execution") || Op == TEXT("add_execution_calculation")
        || Op == TEXT("add_exec"))
    {
        return HandleAddExecution(Params);
    }
    if (Op == TEXT("add_calculation_modifier") || Op == TEXT("add_scoped_modifier")
        || Op == TEXT("add_calc_mod"))
    {
        return HandleAddCalculationModifier(Params);
    }
    if (Op == TEXT("add_conditional_effect") || Op == TEXT("add_conditional_gameplay_effect")
        || Op == TEXT("add_conditional"))
    {
        return HandleAddConditionalEffect(Params);
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

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleAddModifier(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' / 'effect' parameter"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    FString AttributeToken;
    Params->TryGetStringField(TEXT("attribute"), AttributeToken);
    FString AttributeSetToken;
    Params->TryGetStringField(TEXT("attribute_set"), AttributeSetToken);
    FString AttributeNameToken;
    Params->TryGetStringField(TEXT("attribute_name"), AttributeNameToken);

    FGameplayAttribute Attribute;
    FString ResolveError;
    if (!ResolveAttribute(AttributeToken, AttributeSetToken, AttributeNameToken, Attribute, ResolveError))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    FString ModOpToken;
    if (!Params->TryGetStringField(TEXT("modifier_op"), ModOpToken)
        && !Params->TryGetStringField(TEXT("op"), ModOpToken))
    {
        ModOpToken = TEXT("Add");
    }
    EGameplayModOp::Type ModOp;
    if (!ParseModOp(ModOpToken, ModOp))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unrecognised modifier_op '%s' (use add / multiply / override / division / add_final / multiply_compound)"),
                *ModOpToken));
    }

    double MagnitudeRaw = 0.0;
    if (!Params->TryGetNumberField(TEXT("magnitude"), MagnitudeRaw))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'magnitude' (literal float for the FScalableFloat magnitude)"));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FGameplayModifierInfo NewMod;
    NewMod.Attribute = Attribute;
    NewMod.ModifierOp = ModOp;
    FScalableFloat Scale;
    Scale.Value = static_cast<float>(MagnitudeRaw);
    NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(Scale);

    Effect->Modifiers.Add(NewMod);
    const int32 NewIndex = Effect->Modifiers.Num() - 1;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_modifier"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("attribute_name"), Attribute.GetName());
    if (UClass* OwnerClass = Attribute.IsValid() ? Attribute.GetAttributeSetClass() : nullptr)
    {
        Result->SetStringField(TEXT("attribute_set_class"), OwnerClass->GetPathName());
    }
    Result->SetStringField(TEXT("modifier_op"), ModOpToCanonical(ModOp));
    Result->SetNumberField(TEXT("magnitude"), MagnitudeRaw);
    Result->SetNumberField(TEXT("modifier_index"), NewIndex);
    Result->SetNumberField(TEXT("modifier_count"), Effect->Modifiers.Num());
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleRemoveModifierAt(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' / 'effect' parameter"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect"), *AssetPath));
    }

    int32 Index = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("index"), Index))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'index' (integer; the entry in the GE's Modifiers array)"));
    }
    if (!Effect->Modifiers.IsValidIndex(Index))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Index %d out of bounds (GE has %d modifiers)"),
                Index, Effect->Modifiers.Num()));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const FGameplayModifierInfo Removed = Effect->Modifiers[Index];
    Effect->Modifiers.RemoveAt(Index);

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("remove_modifier_at"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetNumberField(TEXT("removed_index"), Index);
    Result->SetStringField(TEXT("removed_attribute"), Removed.Attribute.GetName());
    Result->SetStringField(TEXT("removed_modifier_op"), ModOpToCanonical(Removed.ModifierOp));
    Result->SetNumberField(TEXT("modifier_count"), Effect->Modifiers.Num());
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleSetAttributeDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("attribute_set"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset' / 'attribute_set' parameter"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UAttributeSet* AttributeSet = CDO ? Cast<UAttributeSet>(CDO) : nullptr;
    if (!AttributeSet || !AssetClass || !AssetClass->IsChildOf(UAttributeSet::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UAttributeSet (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    FString AttributeName;
    if (!Params->TryGetStringField(TEXT("attribute_name"), AttributeName)
        && !Params->TryGetStringField(TEXT("attribute"), AttributeName)
        && !Params->TryGetStringField(TEXT("name"), AttributeName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'attribute_name' parameter"));
    }
    double BaseValueRaw = 0.0;
    if (!Params->TryGetNumberField(TEXT("base_value"), BaseValueRaw)
        && !Params->TryGetNumberField(TEXT("value"), BaseValueRaw))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'base_value' (literal float)"));
    }

    FProperty* Prop = FindFProperty<FProperty>(AssetClass, FName(*AttributeName));
    if (!Prop || !FGameplayAttribute::IsSupportedProperty(Prop))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Attribute '%s' on '%s' is not a supported FGameplayAttribute property"),
                *AttributeName, *AssetClass->GetName()));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    const float NewValue = static_cast<float>(BaseValueRaw);
    FString StorageKind;
    if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
    {
        if (StructProp->Struct == FGameplayAttributeData::StaticStruct()
            || StructProp->Struct->IsChildOf(FGameplayAttributeData::StaticStruct()))
        {
            FGameplayAttributeData* Data =
                StructProp->ContainerPtrToValuePtr<FGameplayAttributeData>(AttributeSet);
            if (!Data)
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Could not access FGameplayAttributeData on '%s'"), *AttributeName));
            }
            Data->SetBaseValue(NewValue);
            Data->SetCurrentValue(NewValue);
            StorageKind = TEXT("attribute_data");
        }
    }
    else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
    {
        if (float* Slot = FloatProp->ContainerPtrToValuePtr<float>(AttributeSet))
        {
            *Slot = NewValue;
            StorageKind = TEXT("float");
        }
    }
    if (StorageKind.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Attribute '%s' has unsupported storage type '%s'"),
                *AttributeName, *Prop->GetCPPType()));
    }

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Asset->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_attribute_default"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("attribute_name"), AttributeName);
    Result->SetStringField(TEXT("storage"), StorageKind);
    Result->SetNumberField(TEXT("base_value"), NewValue);
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleSetAbilityCostOrCooldown(const TSharedPtr<FJsonObject>& Params, bool bIsCost)
{
    const TCHAR* OpToken = bIsCost ? TEXT("set_ability_cost") : TEXT("set_ability_cooldown");
    const TCHAR* PropName = bIsCost ? TEXT("CostGameplayEffectClass") : TEXT("CooldownGameplayEffectClass");
    const TCHAR* OutKey   = bIsCost ? TEXT("cost_gameplay_effect_class") : TEXT("cooldown_gameplay_effect_class");

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("ability"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: missing 'asset' / 'ability' parameter"), OpToken));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: could not load asset at '%s'"), OpToken, *AssetPath));
    }

    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayAbility* Ability = CDO ? Cast<UGameplayAbility>(CDO) : nullptr;
    if (!Ability || !AssetClass || !AssetClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: asset at '%s' is not a UGameplayAbility (resolved class: %s)"),
                OpToken, *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    // Read the new effect class. Three shapes accepted: an explicit
    // `clear=true` flag, an empty / null / "none" string in the
    // `effect` (or class / cost / cooldown) field, or a class path
    // pointing at a UGameplayEffect-derived UBlueprint or
    // UBlueprintGeneratedClass. Mirrors the convention shipped on
    // `behavior_tree set_blackboard` and `ik_retarget set_*_ik_rig`.
    bool bClear = false;
    Params->TryGetBoolField(TEXT("clear"), bClear);

    FString EffectInput;
    Params->TryGetStringField(TEXT("effect"), EffectInput);
    if (EffectInput.IsEmpty())
    {
        Params->TryGetStringField(TEXT("class"), EffectInput);
    }
    if (EffectInput.IsEmpty())
    {
        Params->TryGetStringField(bIsCost ? TEXT("cost") : TEXT("cooldown"), EffectInput);
    }
    if (EffectInput.IsEmpty())
    {
        Params->TryGetStringField(TEXT("path"), EffectInput);
    }

    const FString Trimmed = EffectInput.TrimStartAndEnd();
    if (!bClear && (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("none"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("null"), ESearchCase::IgnoreCase)))
    {
        bClear = true;
    }

    UClass* NewEffectClass = nullptr;
    if (!bClear)
    {
        // Resolve the input as a UGameplayEffect-derived UClass. Two
        // accepted shapes: a full `/Script/Module.ClassName` path and
        // a `/Game/...` Blueprint class path (auto-suffixed with `_C`
        // when the caller forgot it).
        UObject* Loaded = nullptr;
        if (Trimmed.StartsWith(TEXT("/Script/")))
        {
            NewEffectClass = LoadClass<UObject>(nullptr, *Trimmed);
        }
        else if (Trimmed.StartsWith(TEXT("/Game/")))
        {
            // Try the class path directly first (so `/Game/Foo/BP_Cost.BP_Cost_C`
            // rounds to a UClass), then fall back to LoadAsset on the
            // BP and pull GeneratedClass off it.
            FString ClassPath = Trimmed;
            if (!ClassPath.EndsWith(TEXT("_C")))
            {
                ClassPath += TEXT("_C");
            }
            NewEffectClass = LoadClass<UObject>(nullptr, *ClassPath);
            if (!NewEffectClass)
            {
                Loaded = UEditorAssetLibrary::LoadAsset(Trimmed);
                if (UBlueprint* AsBP = Cast<UBlueprint>(Loaded))
                {
                    NewEffectClass = AsBP->GeneratedClass;
                }
                else if (UClass* AsClass = Cast<UClass>(Loaded))
                {
                    NewEffectClass = AsClass;
                }
            }
        }
        if (!NewEffectClass)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("%s: could not resolve UGameplayEffect class at '%s'"), OpToken, *Trimmed));
        }
        if (!NewEffectClass->IsChildOf(UGameplayEffect::StaticClass()))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("%s: '%s' is not a UGameplayEffect subclass (resolved: %s)"),
                    OpToken, *Trimmed, *NewEffectClass->GetPathName()));
        }
    }

    // 5.7 demoted CostGameplayEffectClass / CooldownGameplayEffectClass
    // from public to protected. The fields stay reflected, so we go
    // through FClassProperty + ContainerPtrToValuePtr against the
    // ability's UClass; this stays clean-room (we read / write through
    // the public reflection database, not by friending the class).
    FClassProperty* ClassProp = FindFProperty<FClassProperty>(AssetClass, FName(PropName));
    if (!ClassProp)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: '%s' has no reflected FClassProperty named '%s'"),
                OpToken, *AssetClass->GetName(), PropName));
    }
    TSubclassOf<UObject>* Slot = ClassProp->ContainerPtrToValuePtr<TSubclassOf<UObject>>(Ability);
    if (!Slot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: failed to resolve property slot for '%s'"), OpToken, PropName));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UClass* PreviousClass = Slot->Get();
    *Slot = bClear ? nullptr : NewEffectClass;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Asset->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), OpToken);
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetBoolField(TEXT("cleared"), bClear);
    if (PreviousClass)
    {
        Result->SetStringField(TEXT("previous_class"), PreviousClass->GetPathName());
    }
    if (!bClear && NewEffectClass)
    {
        Result->SetStringField(OutKey, NewEffectClass->GetPathName());
    }
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

namespace
{
    /** Resolve a UGameplayCueNotify_Static / AGameplayCueNotify_Actor
     *  parent class from a short token, full path, or bare class name.
     *  Defaults to the static notify (the pattern designers reach for
     *  most often: a one-off burst notify with no scene presence). */
    UClass* GasEdit_ResolveCueNotifyParent(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return UGameplayCueNotify_Static::StaticClass();
        }
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("static") || Lower == TEXT("notify_static")
            || Lower == TEXT("gameplay_cue_notify_static")
            || Lower == TEXT("gameplaycuenotify_static"))
        {
            return UGameplayCueNotify_Static::StaticClass();
        }
        if (Lower == TEXT("actor") || Lower == TEXT("notify_actor")
            || Lower == TEXT("gameplay_cue_notify_actor")
            || Lower == TEXT("gameplaycuenotify_actor"))
        {
            return AGameplayCueNotify_Actor::StaticClass();
        }
        if (Token.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Token))
            {
                return Loaded;
            }
            // Caller may have passed a /Game/... BP class without the _C suffix.
            const FString WithSuffix = Token + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<UObject>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
            return nullptr;
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            return Found;
        }
        const FString GASPath = FString::Printf(TEXT("/Script/GameplayAbilities.%s"), *Token);
        if (UClass* Loaded = LoadClass<UObject>(nullptr, *GASPath))
        {
            return Loaded;
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleCreateCueNotify(const TSharedPtr<FJsonObject>& Params)
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
    UClass* ParentClass = GasEdit_ResolveCueNotifyParent(ParentInput);
    if (!ParentClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent_class '%s'. Pass 'static' / 'actor' or a UGameplayCueNotify_* subclass path."), *ParentInput));
    }
    const bool bIsStatic = ParentClass->IsChildOf(UGameplayCueNotify_Static::StaticClass());
    const bool bIsActor = ParentClass->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
    if (!bIsStatic && !bIsActor)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class '%s' is neither UGameplayCueNotify_Static nor AGameplayCueNotify_Actor"),
                *ParentClass->GetPathName()));
    }

    FString CueTagText;
    Params->TryGetStringField(TEXT("cue_tag"), CueTagText);
    if (CueTagText.IsEmpty())
    {
        Params->TryGetStringField(TEXT("tag"), CueTagText);
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

    bool bAppliedTag = false;
    if (!CueTagText.IsEmpty())
    {
        // The tag must already live in the project's Gameplay Tag
        // registry. RequestGameplayTag with bErrorIfNotFound=false
        // returns an empty tag rather than crashing if the tag is
        // unknown, mirroring the editor's "the tag picker accepts
        // typed text but only highlights registered ones" behaviour.
        const FGameplayTag CueTag = UGameplayTagsManager::Get().RequestGameplayTag(
            FName(*CueTagText), /*ErrorIfNotFound=*/false);
        UClass* GenClass = NewBP->GeneratedClass ? NewBP->GeneratedClass.Get() : NewBP->ParentClass.Get();
        if (GenClass)
        {
            UObject* CDO = GenClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
            if (CDO)
            {
                if (FProperty* TagProperty = FindFProperty<FProperty>(GenClass, FName(TEXT("GameplayCueTag"))))
                {
                    if (FStructProperty* AsStruct = CastField<FStructProperty>(TagProperty))
                    {
                        if (AsStruct->Struct == FGameplayTag::StaticStruct())
                        {
                            void* Container = AsStruct->ContainerPtrToValuePtr<void>(CDO);
                            if (Container)
                            {
                                *(FGameplayTag*)Container = CueTag;
                                bAppliedTag = CueTag.IsValid();
                            }
                        }
                    }
                }
                // Mirror onto the searchable FName for the asset
                // registry; the engine does this in
                // PostEditChangeProperty when the tag changes.
                if (FNameProperty* AsName = FindFProperty<FNameProperty>(GenClass, FName(TEXT("GameplayCueName"))))
                {
                    AsName->SetPropertyValue_InContainer(CDO, FName(*CueTagText));
                }
            }
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
    Result->SetStringField(TEXT("operation"), TEXT("create_cue_notify"));
    Result->SetStringField(TEXT("name"), NewBP->GetName());
    Result->SetStringField(TEXT("path"), FinalPath);
    Result->SetStringField(TEXT("class"), NewBP->GetClass()->GetName());
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    Result->SetStringField(TEXT("parent_class_short"), ParentClass->GetName());
    Result->SetStringField(TEXT("notify_kind"), bIsStatic ? TEXT("static") : TEXT("actor"));
    if (!CueTagText.IsEmpty())
    {
        Result->SetStringField(TEXT("cue_tag"), CueTagText);
        Result->SetBoolField(TEXT("cue_tag_applied"), bAppliedTag);
        if (!bAppliedTag)
        {
            Result->SetStringField(TEXT("cue_tag_warning"),
                TEXT("Tag is not registered in the project's Gameplay Tag database; field stays empty until the tag is added through tag_registry_edit add_tag."));
        }
    }
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleSetAbilityCueTag(const TSharedPtr<FJsonObject>& Params)
{
    // Graph-side authoring slice for the "bind a cue tag to an ability"
    // surface. Spawns a UK2Node_CallFunction for
    //   UGameplayAbility::K2_ExecuteGameplayCue(FGameplayTag, FGameplayEffectContextHandle)
    // in the ability Blueprint's event graph and pre-fills the
    // `GameplayCueTag` literal pin. Reuses an existing matching call
    // node when one is already wired for the same tag so the op is
    // idempotent. The cue invocation exec pin stays unwired by default;
    // a follow-on bp_wire call can chain it from an event node (e.g.
    // ActivateAbility) when the caller is ready.
    const TCHAR* OpToken = TEXT("set_ability_cue_tag");

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("ability"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: missing 'asset' / 'ability' parameter"), OpToken));
    }

    FString CueTagText;
    if (!Params->TryGetStringField(TEXT("cue_tag"), CueTagText)
        && !Params->TryGetStringField(TEXT("tag"), CueTagText)
        && !Params->TryGetStringField(TEXT("gameplay_cue_tag"), CueTagText))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: missing 'cue_tag' parameter (e.g. 'GameplayCue.Combat.Hit')"), OpToken));
    }
    CueTagText.TrimStartAndEndInline();
    if (CueTagText.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: 'cue_tag' parameter is empty"), OpToken));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: asset at '%s' is not a Blueprint"), OpToken, *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    if (!AssetClass || !AssetClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: Blueprint at '%s' is not a UGameplayAbility subclass (resolved class: %s)"),
                OpToken, *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Resolve the gameplay tag. We pass bErrorIfNotFound=false so a
    // missing-tag case surfaces a clean warning rather than an engine
    // assert; the pin literal still picks up the requested text so the
    // node is in shape once the project registers the tag through
    // tag_registry_edit add_tag.
    UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
    const FGameplayTag ResolvedTag = TagManager.RequestGameplayTag(FName(*CueTagText), /*ErrorIfNotFound=*/false);
    const bool bTagRegistered = ResolvedTag.IsValid();

    // Resolve the K2_ExecuteGameplayCue UFunction on UGameplayAbility.
    UFunction* TargetFunction = UGameplayAbility::StaticClass()->FindFunctionByName(FName(TEXT("K2_ExecuteGameplayCue")));
    if (!TargetFunction)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: UGameplayAbility::K2_ExecuteGameplayCue not found on the engine's UFunction table"), OpToken));
    }

    UEdGraph* EventGraph = FEpicUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    if (!EventGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("%s: failed to find or create the Blueprint's event graph"), OpToken));
    }

    // The literal pin's default value string for FGameplayTag uses the
    // canonical struct ExportText form `(TagName="Foo.Bar")`. We build
    // it explicitly here so the pin matches what the editor's literal
    // makes when the user types the tag in the pin picker.
    const FString TagPinDefault = FString::Printf(TEXT("(TagName=\"%s\")"), *CueTagText);

    // Walk the event graph looking for an existing
    // K2_ExecuteGameplayCue call node already pre-filled for this tag,
    // so a second invocation of the op stays idempotent.
    UK2Node_CallFunction* MatchedNode = nullptr;
    for (UEdGraphNode* ExistingNode : EventGraph->Nodes)
    {
        UK2Node_CallFunction* AsCall = Cast<UK2Node_CallFunction>(ExistingNode);
        if (!AsCall)
        {
            continue;
        }
        if (AsCall->GetTargetFunction() != TargetFunction)
        {
            continue;
        }
        UEdGraphPin* TagPin = AsCall->FindPin(FName(TEXT("GameplayCueTag")), EGPD_Input);
        if (TagPin && TagPin->DefaultValue == TagPinDefault)
        {
            MatchedNode = AsCall;
            break;
        }
    }

    bool bReusedExisting = (MatchedNode != nullptr);
    UK2Node_CallFunction* CallNode = MatchedNode;

    if (!CallNode)
    {
        // Spawn a new K2Node_CallFunction wired to K2_ExecuteGameplayCue
        // and pre-fill the tag literal pin.
        CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
        if (!CallNode)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("%s: failed to construct UK2Node_CallFunction"), OpToken));
        }
        CallNode->SetFromFunction(TargetFunction);

        // Lay the node out a little below + right of the graph's
        // existing nodes so it does not stack on top of an event node.
        int32 NodeX = 320;
        int32 NodeY = 64;
        {
            int32 BottomMostY = 0;
            int32 RightMostX = 0;
            for (UEdGraphNode* Existing : EventGraph->Nodes)
            {
                if (Existing)
                {
                    BottomMostY = FMath::Max(BottomMostY, Existing->NodePosY);
                    RightMostX = FMath::Max(RightMostX, Existing->NodePosX);
                }
            }
            NodeX = FMath::Max(NodeX, RightMostX + 320);
            NodeY = FMath::Max(NodeY, BottomMostY);
        }
        CallNode->NodePosX = NodeX;
        CallNode->NodePosY = NodeY;

        EventGraph->AddNode(CallNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
        CallNode->CreateNewGuid();
        CallNode->PostPlacedNewNode();
        CallNode->AllocateDefaultPins();

        UEdGraphPin* TagPin = CallNode->FindPin(FName(TEXT("GameplayCueTag")), EGPD_Input);
        if (!TagPin)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("%s: spawned K2_ExecuteGameplayCue call node has no 'GameplayCueTag' input pin"), OpToken));
        }
        TagPin->DefaultValue = TagPinDefault;
        TagPin->AutogeneratedDefaultValue = TagPinDefault;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("operation"), OpToken);
    Out->SetStringField(TEXT("ability"), Blueprint->GetPathName());
    Out->SetStringField(TEXT("cue_tag"), CueTagText);
    Out->SetBoolField(TEXT("tag_registered"), bTagRegistered);
    Out->SetBoolField(TEXT("node_reused"), bReusedExisting);
    Out->SetStringField(TEXT("graph"), EventGraph->GetName());
    Out->SetStringField(TEXT("node"), CallNode->GetName());
    Out->SetStringField(TEXT("node_guid"), CallNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Out->SetStringField(TEXT("target_function"), TargetFunction->GetName());
    Out->SetStringField(TEXT("tag_pin_default"), TagPinDefault);
    if (!bTagRegistered)
    {
        Out->SetStringField(TEXT("cue_tag_warning"),
            TEXT("Tag is not registered in the project's Gameplay Tag database; the call node is laid down with the requested literal anyway. Run tag_registry_edit add_tag to register it."));
    }
    Out->SetBoolField(TEXT("compiled"), bCompile);
    Out->SetBoolField(TEXT("saved"), bSave);
    return Out;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleSetModifierMagnitude(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' / 'effect' parameter"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    int32 Index = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("index"), Index)
        && !Params->TryGetNumberField(TEXT("modifier_index"), Index))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'index' (integer; the entry in the GE's Modifiers array)"));
    }
    if (!Effect->Modifiers.IsValidIndex(Index))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Index %d out of bounds (GE has %d modifiers)"),
                Index, Effect->Modifiers.Num()));
    }

    FString MagnitudeTypeToken;
    if (!Params->TryGetStringField(TEXT("magnitude_type"), MagnitudeTypeToken)
        && !Params->TryGetStringField(TEXT("type"), MagnitudeTypeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'magnitude_type' (try scalable_float / attribute_based / set_by_caller / custom_calculation_class)"));
    }
    const FString TypeLower = MagnitudeTypeToken.ToLower();

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    auto ReadFloatField = [&Params](const TCHAR* Field, float Default) -> float
    {
        double Out = static_cast<double>(Default);
        Params->TryGetNumberField(Field, Out);
        return static_cast<float>(Out);
    };

    FGameplayEffectModifierMagnitude NewMagnitude;
    FString CanonicalType;
    TSharedPtr<FJsonObject> VariantOut = MakeShared<FJsonObject>();

    if (TypeLower == TEXT("scalable_float") || TypeLower == TEXT("scalable")
        || TypeLower == TEXT("float") || TypeLower == TEXT("literal"))
    {
        double ValueRaw = 0.0;
        if (!Params->TryGetNumberField(TEXT("value"), ValueRaw)
            && !Params->TryGetNumberField(TEXT("magnitude"), ValueRaw))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'value' (literal float for the FScalableFloat magnitude)"));
        }
        FScalableFloat Scale;
        Scale.Value = static_cast<float>(ValueRaw);
        NewMagnitude = FGameplayEffectModifierMagnitude(Scale);
        CanonicalType = TEXT("ScalableFloat");
        VariantOut->SetNumberField(TEXT("value"), ValueRaw);
    }
    else if (TypeLower == TEXT("attribute_based") || TypeLower == TEXT("attribute")
        || TypeLower == TEXT("attribute_capture"))
    {
        FString AttributeToken;
        Params->TryGetStringField(TEXT("attribute"), AttributeToken);
        FString AttributeSetToken;
        Params->TryGetStringField(TEXT("attribute_set"), AttributeSetToken);
        FString AttributeNameToken;
        Params->TryGetStringField(TEXT("attribute_name"), AttributeNameToken);

        FGameplayAttribute BackingAttr;
        FString ResolveError;
        if (!ResolveAttribute(AttributeToken, AttributeSetToken, AttributeNameToken, BackingAttr, ResolveError))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
        }

        FString SourceToken;
        Params->TryGetStringField(TEXT("source"), SourceToken);
        EGameplayEffectAttributeCaptureSource CaptureSource = EGameplayEffectAttributeCaptureSource::Source;
        const FString SourceLower = SourceToken.ToLower();
        if (SourceLower == TEXT("target"))
        {
            CaptureSource = EGameplayEffectAttributeCaptureSource::Target;
        }
        else if (!SourceLower.IsEmpty() && SourceLower != TEXT("source"))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unrecognised 'source' '%s' (use source / target)"), *SourceToken));
        }

        bool bSnapshot = false;
        Params->TryGetBoolField(TEXT("snapshot"), bSnapshot);

        FAttributeBasedFloat Payload;
        Payload.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(BackingAttr, CaptureSource, bSnapshot);
        Payload.Coefficient.Value = ReadFloatField(TEXT("coefficient"), 1.0f);
        Payload.PreMultiplyAdditiveValue.Value = ReadFloatField(TEXT("pre_multiply"), 0.0f);
        Payload.PostMultiplyAdditiveValue.Value = ReadFloatField(TEXT("post_multiply"), 0.0f);

        FString CalcTypeToken;
        Params->TryGetStringField(TEXT("calculation_type"), CalcTypeToken);
        if (!CalcTypeToken.IsEmpty())
        {
            const FString CT = CalcTypeToken.ToLower();
            if (CT == TEXT("magnitude") || CT == TEXT("attribute_magnitude"))
            {
                Payload.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeMagnitude;
            }
            else if (CT == TEXT("base_value") || CT == TEXT("attribute_base_value"))
            {
                Payload.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeBaseValue;
            }
            else if (CT == TEXT("bonus_magnitude") || CT == TEXT("attribute_bonus_magnitude"))
            {
                Payload.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeBonusMagnitude;
            }
            else if (CT == TEXT("magnitude_evaluated_up_to_channel")
                || CT == TEXT("attribute_magnitude_evaluated_up_to_channel"))
            {
                Payload.AttributeCalculationType = EAttributeBasedFloatCalculationType::AttributeMagnitudeEvaluatedUpToChannel;
            }
            else
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unrecognised 'calculation_type' '%s' (magnitude / base_value / bonus_magnitude / magnitude_evaluated_up_to_channel)"),
                        *CalcTypeToken));
            }
        }

        NewMagnitude = FGameplayEffectModifierMagnitude(Payload);
        CanonicalType = TEXT("AttributeBased");
        VariantOut->SetStringField(TEXT("attribute_name"), BackingAttr.GetName());
        if (UClass* OwnerClass = BackingAttr.IsValid() ? BackingAttr.GetAttributeSetClass() : nullptr)
        {
            VariantOut->SetStringField(TEXT("attribute_set_class"), OwnerClass->GetPathName());
        }
        VariantOut->SetStringField(TEXT("source"),
            (CaptureSource == EGameplayEffectAttributeCaptureSource::Target) ? TEXT("target") : TEXT("source"));
        VariantOut->SetBoolField(TEXT("snapshot"), bSnapshot);
        VariantOut->SetNumberField(TEXT("coefficient"), Payload.Coefficient.Value);
        VariantOut->SetNumberField(TEXT("pre_multiply"), Payload.PreMultiplyAdditiveValue.Value);
        VariantOut->SetNumberField(TEXT("post_multiply"), Payload.PostMultiplyAdditiveValue.Value);
    }
    else if (TypeLower == TEXT("set_by_caller") || TypeLower == TEXT("setbycaller")
        || TypeLower == TEXT("caller"))
    {
        FSetByCallerFloat Payload;
        FString DataNameStr;
        if (Params->TryGetStringField(TEXT("data_name"), DataNameStr) && !DataNameStr.IsEmpty())
        {
            Payload.DataName = FName(*DataNameStr);
        }

        FString DataTagStr;
        if (Params->TryGetStringField(TEXT("data_tag"), DataTagStr) && !DataTagStr.IsEmpty())
        {
            const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*DataTagStr),
                /*ErrorIfNotFound=*/false);
            if (Tag.IsValid())
            {
                Payload.DataTag = Tag;
                VariantOut->SetBoolField(TEXT("data_tag_registered"), true);
            }
            else
            {
                // Unknown tags surface a clean warning; the runtime still
                // resolves the magnitude through DataName when DataTag
                // misses, matching the engine's fallback path.
                VariantOut->SetBoolField(TEXT("data_tag_registered"), false);
                VariantOut->SetStringField(TEXT("data_tag_warning"),
                    TEXT("Tag is not registered in the project's Gameplay Tag database; the runtime will fall back to DataName when the tag misses."));
            }
            VariantOut->SetStringField(TEXT("data_tag"), DataTagStr);
        }

        if (Payload.DataName.IsNone() && !Payload.DataTag.IsValid())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("set_by_caller magnitude needs at least one of 'data_name' / 'data_tag'"));
        }

        NewMagnitude = FGameplayEffectModifierMagnitude(Payload);
        CanonicalType = TEXT("SetByCaller");
        VariantOut->SetStringField(TEXT("data_name"), Payload.DataName.ToString());
    }
    else if (TypeLower == TEXT("custom_calculation_class") || TypeLower == TEXT("custom")
        || TypeLower == TEXT("custom_calculation") || TypeLower == TEXT("calculation_class"))
    {
        FString CalculationClassToken;
        if (!Params->TryGetStringField(TEXT("calculation_class"), CalculationClassToken)
            && !Params->TryGetStringField(TEXT("class"), CalculationClassToken)
            && !Params->TryGetStringField(TEXT("magnitude_class"), CalculationClassToken))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'calculation_class' (UGameplayModMagnitudeCalculation subclass path)"));
        }
        UClass* Resolved = ResolveCreateParentClass(CalculationClassToken, nullptr);
        if (!Resolved)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not resolve calculation class '%s'"), *CalculationClassToken));
        }
        if (!Resolved->IsChildOf(UGameplayModMagnitudeCalculation::StaticClass()))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Class '%s' is not a UGameplayModMagnitudeCalculation subclass"),
                    *Resolved->GetPathName()));
        }

        FCustomCalculationBasedFloat Payload;
        Payload.CalculationClassMagnitude = Resolved;
        Payload.Coefficient.Value = ReadFloatField(TEXT("coefficient"), 1.0f);
        Payload.PreMultiplyAdditiveValue.Value = ReadFloatField(TEXT("pre_multiply"), 0.0f);
        Payload.PostMultiplyAdditiveValue.Value = ReadFloatField(TEXT("post_multiply"), 0.0f);

        NewMagnitude = FGameplayEffectModifierMagnitude(Payload);
        CanonicalType = TEXT("CustomCalculationClass");
        VariantOut->SetStringField(TEXT("calculation_class"), Resolved->GetPathName());
        VariantOut->SetNumberField(TEXT("coefficient"), Payload.Coefficient.Value);
        VariantOut->SetNumberField(TEXT("pre_multiply"), Payload.PreMultiplyAdditiveValue.Value);
        VariantOut->SetNumberField(TEXT("post_multiply"), Payload.PostMultiplyAdditiveValue.Value);
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unrecognised 'magnitude_type' '%s' (scalable_float / attribute_based / set_by_caller / custom_calculation_class)"),
                *MagnitudeTypeToken));
    }

    // Capture the previous magnitude's calculation type so the caller
    // sees what the rewrite replaced.
    const EGameplayEffectMagnitudeCalculation PrevCalc = Effect->Modifiers[Index].ModifierMagnitude.GetMagnitudeCalculationType();
    FString PrevCanonical;
    switch (PrevCalc)
    {
    case EGameplayEffectMagnitudeCalculation::ScalableFloat:          PrevCanonical = TEXT("ScalableFloat"); break;
    case EGameplayEffectMagnitudeCalculation::AttributeBased:         PrevCanonical = TEXT("AttributeBased"); break;
    case EGameplayEffectMagnitudeCalculation::CustomCalculationClass: PrevCanonical = TEXT("CustomCalculationClass"); break;
    case EGameplayEffectMagnitudeCalculation::SetByCaller:            PrevCanonical = TEXT("SetByCaller"); break;
    default: PrevCanonical = TEXT("Unknown"); break;
    }

    Effect->Modifiers[Index].ModifierMagnitude = NewMagnitude;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_modifier_magnitude"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetNumberField(TEXT("modifier_index"), Index);
    Result->SetNumberField(TEXT("modifier_count"), Effect->Modifiers.Num());
    Result->SetStringField(TEXT("magnitude_type"), CanonicalType);
    Result->SetStringField(TEXT("previous_magnitude_type"), PrevCanonical);
    Result->SetObjectField(TEXT("payload"), VariantOut);
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

namespace
{
    /** Resolve a class token to a UClass*. Accepts a `/Script/Module.ClassName`
     *  reflection path, a `/Game/...` BP-class path (the resolver follows
     *  through to the BP's generated class), and a bare class name fallback
     *  through TObjectIterator<UClass>. Returns nullptr on miss. */
    UClass* ResolveClassByToken(const FString& Token)
    {
        if (Token.IsEmpty()) return nullptr;
        FString Norm = Token;
        Norm.TrimStartAndEndInline();

        if (Norm.StartsWith(TEXT("/Script/")))
        {
            return FindObject<UClass>(nullptr, *Norm);
        }
        if (Norm.StartsWith(TEXT("/")))
        {
            // /Game/.../BP_X or /Game/.../BP_X.BP_X_C
            UObject* Loaded = UEditorAssetLibrary::LoadAsset(Norm);
            if (UBlueprint* AsBP = Cast<UBlueprint>(Loaded))
            {
                return AsBP->GeneratedClass;
            }
            return Cast<UClass>(Loaded);
        }
        // Bare class name fallback. Strip a leading U / A prefix if the
        // candidate name is otherwise unique in the loaded set.
        FString BareName = Norm;
        FString WithoutPrefix = Norm;
        if ((Norm.StartsWith(TEXT("U")) || Norm.StartsWith(TEXT("A")))
            && Norm.Len() > 1 && FChar::IsUpper(Norm[1]))
        {
            WithoutPrefix = Norm.RightChop(1);
        }
        UClass* Found = nullptr;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Cls = *It;
            if (!Cls) continue;
            const FString CName = Cls->GetName();
            if (CName == BareName || CName == WithoutPrefix)
            {
                Found = Cls;
                break;
            }
        }
        return Found;
    }
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleAddExecution(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset' / 'effect' parameter (path to a UGameplayEffect or its Blueprint)"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    FString ClassToken;
    if (!Params->TryGetStringField(TEXT("calculation_class"), ClassToken)
        && !Params->TryGetStringField(TEXT("execution_class"), ClassToken)
        && !Params->TryGetStringField(TEXT("class"), ClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'calculation_class' parameter (path or class name of a UGameplayEffectExecutionCalculation subclass)"));
    }

    UClass* CalcClass = ResolveClassByToken(ClassToken);
    if (!CalcClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve calculation_class '%s'"), *ClassToken));
    }
    if (!CalcClass->IsChildOf(UGameplayEffectExecutionCalculation::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is not a UGameplayEffectExecutionCalculation subclass"),
                *CalcClass->GetPathName()));
    }
    if (CalcClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is abstract; pick a concrete subclass"),
                *CalcClass->GetPathName()));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Build the new execution definition. CalculationClass + PassedInTags
    // are the two fields exposed in the editor's "Executions" details
    // panel; the per-modifier scoped-modifier rows and conditional GE
    // rows stay on the BACKLOG for follow-on edit ops.
    FGameplayEffectExecutionDefinition NewDef;
    NewDef.CalculationClass = TSubclassOf<UGameplayEffectExecutionCalculation>(CalcClass);

    // Optional passed_in_tags surface. Accepts either a JSON array of
    // tag strings or a single tag string. Unknown tags surface a warning
    // and skip rather than crash so the call stays best-effort idempotent.
    TArray<FString> WarningTags;
    int32 AddedTags = 0;
    if (Params->HasField(TEXT("passed_in_tags")))
    {
        const TSharedPtr<FJsonValue> TagsVal = Params->TryGetField(TEXT("passed_in_tags"));
        UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
        auto AddOneTag = [&](const FString& TagString)
        {
            if (TagString.IsEmpty()) return;
            const FGameplayTag Tag = TagsManager.RequestGameplayTag(*TagString, /*bErrorIfNotFound=*/false);
            if (!Tag.IsValid())
            {
                WarningTags.Add(TagString);
                return;
            }
            NewDef.PassedInTags.AddTag(Tag);
            ++AddedTags;
        };
        if (TagsVal.IsValid() && TagsVal->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& V : TagsVal->AsArray())
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    AddOneTag(V->AsString());
                }
            }
        }
        else if (TagsVal.IsValid() && TagsVal->Type == EJson::String)
        {
            AddOneTag(TagsVal->AsString());
        }
    }

    Effect->Executions.Add(NewDef);
    const int32 NewIndex = Effect->Executions.Num() - 1;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_execution"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("calculation_class"), CalcClass->GetPathName());
    Result->SetNumberField(TEXT("execution_index"), NewIndex);
    Result->SetNumberField(TEXT("execution_count"), Effect->Executions.Num());
    Result->SetNumberField(TEXT("tags_added"), AddedTags);
    if (WarningTags.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnJson;
        for (const FString& T : WarningTags)
        {
            WarnJson.Add(MakeShared<FJsonValueString>(T));
        }
        Result->SetArrayField(TEXT("unknown_tags"), WarnJson);
    }
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleAddCalculationModifier(const TSharedPtr<FJsonObject>& Params)
{
    // Append an FGameplayEffectExecutionScopedModifierInfo onto the
    // chosen execution's CalculationModifiers array. These are the
    // per-execution scoped modifiers the editor surfaces under each
    // entry in the Executions list; the previous `add_execution` op
    // landed the calculation_class + passed_in_tags fields only.
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset' / 'effect' parameter (path to a UGameplayEffect or its Blueprint)"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    int32 ExecutionIndex = INDEX_NONE;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("execution_index"), TempNum)
        || Params->TryGetNumberField(TEXT("index"), TempNum)
        || Params->TryGetNumberField(TEXT("exec_index"), TempNum))
    {
        ExecutionIndex = static_cast<int32>(TempNum);
    }
    if (ExecutionIndex < 0 || ExecutionIndex >= Effect->Executions.Num())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("execution_index %d out of range [0, %d)"),
                ExecutionIndex, Effect->Executions.Num()));
    }

    // Resolve the captured attribute. Mirrors `add_modifier`'s
    // resolver: accepts `<set_path>:<name>`, `attribute_name` +
    // optional `attribute_set`, and a bare name fallback that walks
    // loaded UAttributeSet subclasses.
    FString AttributeToken;
    Params->TryGetStringField(TEXT("attribute"), AttributeToken);
    FString AttributeSetToken;
    Params->TryGetStringField(TEXT("attribute_set"), AttributeSetToken);
    FString AttributeNameToken;
    Params->TryGetStringField(TEXT("attribute_name"), AttributeNameToken);
    FGameplayAttribute Attribute;
    FString AttrError;
    if (!ResolveAttribute(AttributeToken, AttributeSetToken, AttributeNameToken, Attribute, AttrError))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(AttrError);
    }

    // Capture source token. Default Source (the canonical
    // EGameplayEffectAttributeCaptureDefinition default).
    FString SourceToken;
    Params->TryGetStringField(TEXT("source"), SourceToken);
    if (SourceToken.IsEmpty()) Params->TryGetStringField(TEXT("attribute_source"), SourceToken);
    if (SourceToken.IsEmpty()) Params->TryGetStringField(TEXT("capture_source"), SourceToken);
    const FString SourceLower = SourceToken.ToLower();
    EGameplayEffectAttributeCaptureSource CaptureSource = EGameplayEffectAttributeCaptureSource::Source;
    FString SourceCanonical = TEXT("Source");
    if (SourceLower.IsEmpty() || SourceLower == TEXT("source") || SourceLower == TEXT("caster"))
    {
        CaptureSource = EGameplayEffectAttributeCaptureSource::Source;
        SourceCanonical = TEXT("Source");
    }
    else if (SourceLower == TEXT("target") || SourceLower == TEXT("recipient"))
    {
        CaptureSource = EGameplayEffectAttributeCaptureSource::Target;
        SourceCanonical = TEXT("Target");
    }
    else
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported capture source '%s' (source / target)"), *SourceToken));
    }

    bool bSnapshot = false;
    Params->TryGetBoolField(TEXT("snapshot"), bSnapshot);

    // Optional modifier op + magnitude. Default to Additive + zero.
    FString ModOpToken;
    Params->TryGetStringField(TEXT("modifier_op"), ModOpToken);
    if (ModOpToken.IsEmpty()) Params->TryGetStringField(TEXT("op"), ModOpToken);
    if (ModOpToken.IsEmpty()) ModOpToken = TEXT("Add");
    EGameplayModOp::Type ModOp;
    if (!ParseModOp(ModOpToken, ModOp))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unrecognised modifier_op '%s'"), *ModOpToken));
    }

    bool bHasMagnitude = false;
    double MagnitudeRaw = 0.0;
    if (Params->TryGetNumberField(TEXT("magnitude"), MagnitudeRaw))
    {
        bHasMagnitude = true;
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Build the scoped modifier through the public capture-backed
    // constructor so AggregatorType lands on CapturedAttributeBacked.
    FGameplayEffectAttributeCaptureDefinition CaptureDef(Attribute, CaptureSource, bSnapshot);
    FGameplayEffectExecutionScopedModifierInfo NewMod(CaptureDef);
    NewMod.ModifierOp = ModOp;
    if (bHasMagnitude)
    {
        FScalableFloat Scale;
        Scale.Value = static_cast<float>(MagnitudeRaw);
        NewMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(Scale);
    }

    FGameplayEffectExecutionDefinition& ExecDef = Effect->Executions[ExecutionIndex];
    ExecDef.CalculationModifiers.Add(NewMod);
    const int32 NewModIndex = ExecDef.CalculationModifiers.Num() - 1;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_calculation_modifier"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetNumberField(TEXT("execution_index"), ExecutionIndex);
    Result->SetNumberField(TEXT("calculation_modifier_index"), NewModIndex);
    Result->SetNumberField(TEXT("calculation_modifier_count"), ExecDef.CalculationModifiers.Num());
    Result->SetStringField(TEXT("attribute_name"), Attribute.GetName());
    if (const FProperty* Prop = Attribute.GetUProperty())
    {
        if (UClass* OwnerClass = Prop->GetOwnerClass())
        {
            Result->SetStringField(TEXT("attribute_set_class"), OwnerClass->GetPathName());
        }
    }
    Result->SetStringField(TEXT("capture_source"), SourceCanonical);
    Result->SetBoolField(TEXT("snapshot"), bSnapshot);
    Result->SetStringField(TEXT("modifier_op"), ModOpToCanonical(ModOp));
    if (bHasMagnitude)
    {
        Result->SetNumberField(TEXT("magnitude"), MagnitudeRaw);
    }
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftGasEditCommands::HandleAddConditionalEffect(const TSharedPtr<FJsonObject>& Params)
{
    // Append an FConditionalGameplayEffect to the parent effect's
    // ConditionalGameplayEffects array. Each row binds a child
    // UGameplayEffect class plus an optional RequiredSourceTags
    // container that gates the conditional application.
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("effect"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset' / 'effect' parameter (path to the parent UGameplayEffect or its Blueprint)"));
    }
    UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
    if (!Asset)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath));
    }
    UClass* AssetClass = ResolveAssetClass(Asset);
    UObject* CDO = ResolveCDO(Asset);
    UGameplayEffect* Effect = CDO ? Cast<UGameplayEffect>(CDO) : nullptr;
    if (!Effect)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset at '%s' is not a UGameplayEffect (resolved class: %s)"),
                *AssetPath, AssetClass ? *AssetClass->GetName() : TEXT("null")));
    }

    FString ChildClassToken;
    if (!Params->TryGetStringField(TEXT("effect_class"), ChildClassToken)
        && !Params->TryGetStringField(TEXT("child_effect"), ChildClassToken)
        && !Params->TryGetStringField(TEXT("child"), ChildClassToken)
        && !Params->TryGetStringField(TEXT("class"), ChildClassToken)
        && !Params->TryGetStringField(TEXT("child_class"), ChildClassToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'effect_class' parameter (UGameplayEffect-derived class path)"));
    }
    UClass* ChildClass = ResolveClassByToken(ChildClassToken);
    if (!ChildClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve effect_class '%s'"), *ChildClassToken));
    }
    if (!ChildClass->IsChildOf(UGameplayEffect::StaticClass()))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class '%s' is not a UGameplayEffect subclass"),
                *ChildClass->GetPathName()));
    }

    // Optional RequiredSourceTags container. Same shape as
    // `add_execution`'s passed_in_tags: a JSON array of strings or a
    // single string.
    FConditionalGameplayEffect NewCond;
    NewCond.EffectClass = TSubclassOf<UGameplayEffect>(ChildClass);

    TArray<FString> WarningTags;
    int32 AddedTags = 0;
    auto AddOneTag = [&](const FString& TagString)
    {
        if (TagString.IsEmpty()) return;
        const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
            *TagString, /*bErrorIfNotFound=*/false);
        if (!Tag.IsValid())
        {
            WarningTags.Add(TagString);
            return;
        }
        NewCond.RequiredSourceTags.AddTag(Tag);
        ++AddedTags;
    };
    const TCHAR* TagFieldKeys[] = {
        TEXT("required_source_tags"),
        TEXT("source_tags"),
        TEXT("required_tags"),
    };
    for (const TCHAR* Key : TagFieldKeys)
    {
        if (!Params->HasField(Key)) continue;
        const TSharedPtr<FJsonValue> Val = Params->TryGetField(Key);
        if (!Val.IsValid()) continue;
        if (Val->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& V : Val->AsArray())
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    AddOneTag(V->AsString());
                }
            }
        }
        else if (Val->Type == EJson::String)
        {
            AddOneTag(Val->AsString());
        }
        break;
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Effect->ConditionalGameplayEffects.Add(NewCond);
    const int32 NewIndex = Effect->ConditionalGameplayEffects.Num() - 1;

    UBlueprint* OwningBP = Cast<UBlueprint>(Asset);
    if (OwningBP)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(OwningBP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(OwningBP);
        }
    }
    Effect->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_conditional_effect"));
    Result->SetStringField(TEXT("path"), Asset->GetPathName());
    Result->SetStringField(TEXT("effect_class"), ChildClass->GetPathName());
    Result->SetNumberField(TEXT("conditional_index"), NewIndex);
    Result->SetNumberField(TEXT("conditional_count"), Effect->ConditionalGameplayEffects.Num());
    Result->SetNumberField(TEXT("tags_added"), AddedTags);
    if (WarningTags.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnJson;
        for (const FString& T : WarningTags)
        {
            WarnJson.Add(MakeShared<FJsonValueString>(T));
        }
        Result->SetArrayField(TEXT("unknown_tags"), WarnJson);
    }
    Result->SetBoolField(TEXT("compiled"), OwningBP && bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
