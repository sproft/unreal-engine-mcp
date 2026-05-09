#include "Commands/SproftUnrealApiCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace
{
    /** Class resolver. Accepts:
     *   - `/Script/Module.ClassName`
     *   - `/Game/...` Blueprint class path (auto-suffixed with `_C`)
     *   - short class name (e.g. `Actor`, `MyCharacter`).
     */
    UClass* UnrealApi_ResolveClass(const FString& Input)
    {
        const FString Trimmed = Input.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return nullptr;
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

        // Short-name probe with optional A/U/F prefix variants.
        TArray<FString> Candidates;
        Candidates.Add(Trimmed);
        if (!Trimmed.StartsWith(TEXT("A")) && !Trimmed.StartsWith(TEXT("U")) && !Trimmed.StartsWith(TEXT("F")))
        {
            Candidates.Add(TEXT("A") + Trimmed);
            Candidates.Add(TEXT("U") + Trimmed);
        }
        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                return Found;
            }
        }
        for (const FString& Candidate : Candidates)
        {
            const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *Candidate);
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *EnginePath))
            {
                return Loaded;
            }
        }
        return nullptr;
    }

    /** Container token: `single` / `array` / `set` / `map`. */
    FString ContainerToken(const FProperty* Prop)
    {
        if (!Prop) return TEXT("single");
        if (Prop->IsA<FArrayProperty>())
        {
            return TEXT("array");
        }
        if (Prop->IsA<FSetProperty>())
        {
            return TEXT("set");
        }
        if (Prop->IsA<FMapProperty>())
        {
            return TEXT("map");
        }
        return TEXT("single");
    }

    /** Decoded UPROPERTY flag set as an FName-array. Only the
     *  designer-relevant subset; we do not echo every internal flag. */
    TArray<TSharedPtr<FJsonValue>> PropertyFlagStrings(uint64 Flags)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const auto Add = [&Out](const TCHAR* Name)
        {
            Out.Add(MakeShared<FJsonValueString>(FString(Name)));
        };

        if (Flags & CPF_Edit) Add(TEXT("Edit"));
        if (Flags & CPF_BlueprintVisible) Add(TEXT("BlueprintVisible"));
        if (Flags & CPF_BlueprintReadOnly) Add(TEXT("BlueprintReadOnly"));
        if (Flags & CPF_Net) Add(TEXT("Net"));
        if (Flags & CPF_RepNotify) Add(TEXT("RepNotify"));
        if (Flags & CPF_Transient) Add(TEXT("Transient"));
        if (Flags & CPF_Config) Add(TEXT("Config"));
        if (Flags & CPF_GlobalConfig) Add(TEXT("GlobalConfig"));
        if (Flags & CPF_DisableEditOnInstance) Add(TEXT("DisableEditOnInstance"));
        if (Flags & CPF_DisableEditOnTemplate) Add(TEXT("DisableEditOnTemplate"));
        if (Flags & CPF_EditConst) Add(TEXT("EditConst"));
        if (Flags & CPF_Deprecated) Add(TEXT("Deprecated"));
        if (Flags & CPF_SaveGame) Add(TEXT("SaveGame"));
        if (Flags & CPF_AdvancedDisplay) Add(TEXT("AdvancedDisplay"));
        if (Flags & CPF_Protected) Add(TEXT("Protected"));
        if (Flags & CPF_InstancedReference) Add(TEXT("InstancedReference"));
        if (Flags & CPF_EditorOnly) Add(TEXT("EditorOnly"));
        if (Flags & CPF_BlueprintCallable) Add(TEXT("BlueprintCallable"));
        if (Flags & CPF_BlueprintAuthorityOnly) Add(TEXT("BlueprintAuthorityOnly"));
        if (Flags & CPF_TextExportTransient) Add(TEXT("TextExportTransient"));
        if (Flags & CPF_NonPIEDuplicateTransient) Add(TEXT("NonPIEDuplicateTransient"));
        if (Flags & CPF_ExposeOnSpawn) Add(TEXT("ExposeOnSpawn"));
        if (Flags & CPF_PersistentInstance) Add(TEXT("PersistentInstance"));
        if (Flags & CPF_UObjectWrapper) Add(TEXT("UObjectWrapper"));
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> FunctionFlagStrings(uint32 Flags)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const auto Add = [&Out](const TCHAR* Name)
        {
            Out.Add(MakeShared<FJsonValueString>(FString(Name)));
        };

        if (Flags & FUNC_Final) Add(TEXT("Final"));
        if (Flags & FUNC_RequiredAPI) Add(TEXT("RequiredAPI"));
        if (Flags & FUNC_BlueprintAuthorityOnly) Add(TEXT("BlueprintAuthorityOnly"));
        if (Flags & FUNC_BlueprintCosmetic) Add(TEXT("BlueprintCosmetic"));
        if (Flags & FUNC_Net) Add(TEXT("Net"));
        if (Flags & FUNC_NetReliable) Add(TEXT("NetReliable"));
        if (Flags & FUNC_NetRequest) Add(TEXT("NetRequest"));
        if (Flags & FUNC_Exec) Add(TEXT("Exec"));
        if (Flags & FUNC_NetResponse) Add(TEXT("NetResponse"));
        if (Flags & FUNC_Static) Add(TEXT("Static"));
        if (Flags & FUNC_NetMulticast) Add(TEXT("NetMulticast"));
        if (Flags & FUNC_Public) Add(TEXT("Public"));
        if (Flags & FUNC_Private) Add(TEXT("Private"));
        if (Flags & FUNC_Protected) Add(TEXT("Protected"));
        if (Flags & FUNC_NetServer) Add(TEXT("NetServer"));
        if (Flags & FUNC_HasOutParms) Add(TEXT("HasOutParms"));
        if (Flags & FUNC_HasDefaults) Add(TEXT("HasDefaults"));
        if (Flags & FUNC_NetClient) Add(TEXT("NetClient"));
        if (Flags & FUNC_BlueprintCallable) Add(TEXT("BlueprintCallable"));
        if (Flags & FUNC_BlueprintEvent) Add(TEXT("BlueprintEvent"));
        if (Flags & FUNC_BlueprintPure) Add(TEXT("BlueprintPure"));
        if (Flags & FUNC_Const) Add(TEXT("Const"));
        if (Flags & FUNC_NetValidate) Add(TEXT("NetValidate"));
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> ClassFlagStrings(EClassFlags Flags)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const auto Add = [&Out](const TCHAR* Name)
        {
            Out.Add(MakeShared<FJsonValueString>(FString(Name)));
        };

        if (Flags & CLASS_Abstract) Add(TEXT("Abstract"));
        if (Flags & CLASS_Native) Add(TEXT("Native"));
        if (Flags & CLASS_Transient) Add(TEXT("Transient"));
        if (Flags & CLASS_Optional) Add(TEXT("Optional"));
        if (Flags & CLASS_DefaultConfig) Add(TEXT("DefaultConfig"));
        if (Flags & CLASS_Config) Add(TEXT("Config"));
        if (Flags & CLASS_Interface) Add(TEXT("Interface"));
        if (Flags & CLASS_Deprecated) Add(TEXT("Deprecated"));
        if (Flags & CLASS_NotPlaceable) Add(TEXT("NotPlaceable"));
        if (Flags & CLASS_PerObjectConfig) Add(TEXT("PerObjectConfig"));
        if (Flags & CLASS_EditInlineNew) Add(TEXT("EditInlineNew"));
        if (Flags & CLASS_CollapseCategories) Add(TEXT("CollapseCategories"));
        if (Flags & CLASS_HideDropDown) Add(TEXT("HideDropDown"));
        if (Flags & CLASS_GlobalUserConfig) Add(TEXT("GlobalUserConfig"));
        if (Flags & CLASS_Const) Add(TEXT("Const"));
        if (Flags & CLASS_Hidden) Add(TEXT("Hidden"));
        if (Flags & CLASS_HasInstancedReference) Add(TEXT("HasInstancedReference"));
        if (Flags & CLASS_MinimalAPI) Add(TEXT("MinimalAPI"));
        return Out;
    }

    bool ClassPassesContainerCheck(const UClass* Klass)
    {
        return Klass != nullptr;
    }

    bool IsBlueprintGeneratedClass(const UClass* Klass)
    {
        if (!Klass) return false;
        // Native classes are produced by UHT; Blueprint-generated
        // classes carry CLASS_CompiledFromBlueprint. Either flag
        // alone tells us enough about the origin.
        return Klass->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
    }

    void AddClassRefField(TSharedPtr<FJsonObject>& Out, const TCHAR* Field, const UClass* Klass)
    {
        if (!ClassPassesContainerCheck(Klass)) return;
        TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
        Inner->SetStringField(TEXT("class"), Klass->GetName());
        Inner->SetStringField(TEXT("class_path"), Klass->GetPathName());
        Inner->SetBoolField(TEXT("is_native"), Klass->HasAnyClassFlags(CLASS_Native));
        if (const UClass* Super = Klass->GetSuperClass())
        {
            Inner->SetStringField(TEXT("super_class"), Super->GetName());
        }
        Out->SetObjectField(Field, Inner);
    }

    /** Build a FProperty record. The owner_class field tells the caller
     *  whether this property came from the queried class itself or
     *  from an ancestor. */
    TSharedPtr<FJsonObject> BuildPropertyRecord(const FProperty* Prop, const UClass* QueriedClass)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        if (!Prop) return Row;

        Row->SetStringField(TEXT("name"), Prop->GetName());

        // CPP type / element type: for containers, GetCPPType returns
        // the wrapper. We also emit the inner type so a caller can
        // see "TArray<UStaticMesh*>" -> inner "UStaticMesh*".
        const FString ElementType = Prop->GetCPPType();
        Row->SetStringField(TEXT("cpp_type"), ElementType);
        Row->SetStringField(TEXT("container"), ContainerToken(Prop));

        // Inner type for array / set / map.
        if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
        {
            if (ArrayProp->Inner)
            {
                Row->SetStringField(TEXT("inner_type"), ArrayProp->Inner->GetCPPType());
            }
        }
        else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
        {
            if (SetProp->ElementProp)
            {
                Row->SetStringField(TEXT("inner_type"), SetProp->ElementProp->GetCPPType());
            }
        }
        else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
        {
            if (MapProp->KeyProp)
            {
                Row->SetStringField(TEXT("key_type"), MapProp->KeyProp->GetCPPType());
            }
            if (MapProp->ValueProp)
            {
                Row->SetStringField(TEXT("value_type"), MapProp->ValueProp->GetCPPType());
            }
        }

        Row->SetArrayField(TEXT("flags"), PropertyFlagStrings(Prop->GetPropertyFlags()));

#if WITH_EDITORONLY_DATA
        const FString Tooltip = Prop->GetMetaData(TEXT("ToolTip"));
        if (!Tooltip.IsEmpty())
        {
            Row->SetStringField(TEXT("tooltip"), Tooltip);
        }
        const FString Category = Prop->GetMetaData(TEXT("Category"));
        if (!Category.IsEmpty())
        {
            Row->SetStringField(TEXT("category"), Category);
        }
#endif

        // Owner class: the class on which this property was originally
        // declared. For properties walked via TFieldIterator with
        // IncludeSuper, this tells the caller "this came from AActor"
        // even if the queried class was AStaticMeshActor.
        if (UStruct* OwnerStruct = Prop->GetOwnerStruct())
        {
            Row->SetStringField(TEXT("owner_class"), OwnerStruct->GetName());
            Row->SetStringField(TEXT("owner_class_path"), OwnerStruct->GetPathName());
            Row->SetBoolField(TEXT("inherited"), QueriedClass != nullptr && QueriedClass != OwnerStruct);
        }
        return Row;
    }

    /** Build a UFunction record. */
    TSharedPtr<FJsonObject> BuildFunctionRecord(UFunction* Func, const UClass* QueriedClass)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        if (!Func) return Row;

        Row->SetStringField(TEXT("name"), Func->GetName());
        const uint32 FunctionFlags = static_cast<uint32>(Func->FunctionFlags);
        Row->SetArrayField(TEXT("flags"), FunctionFlagStrings(FunctionFlags));
        Row->SetBoolField(TEXT("is_pure"), Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
        Row->SetBoolField(TEXT("is_blueprint_callable"), Func->HasAnyFunctionFlags(FUNC_BlueprintCallable));
        Row->SetBoolField(TEXT("is_blueprint_event"), Func->HasAnyFunctionFlags(FUNC_BlueprintEvent));
        Row->SetBoolField(TEXT("is_static"), Func->HasAnyFunctionFlags(FUNC_Static));
        Row->SetBoolField(TEXT("is_net"), Func->HasAnyFunctionFlags(FUNC_Net));
        Row->SetBoolField(TEXT("is_const"), Func->HasAnyFunctionFlags(FUNC_Const));

#if WITH_EDITORONLY_DATA
        const FString Tooltip = Func->GetMetaData(TEXT("ToolTip"));
        if (!Tooltip.IsEmpty())
        {
            Row->SetStringField(TEXT("tooltip"), Tooltip);
        }
        const FString Category = Func->GetMetaData(TEXT("Category"));
        if (!Category.IsEmpty())
        {
            Row->SetStringField(TEXT("category"), Category);
        }
#endif

        // Walk the function's properties: parameters first, then the
        // optional return value, distinguished by CPF_ReturnParm.
        TArray<TSharedPtr<FJsonValue>> ParamsArr;
        TSharedPtr<FJsonObject> ReturnRow;
        for (TFieldIterator<FProperty> ParamIt(Func); ParamIt; ++ParamIt)
        {
            FProperty* ParamProp = *ParamIt;
            if (!ParamProp) continue;
            // Only walk properties that are part of the function's
            // signature (parameters + return). The check shields us
            // against any future local-variable additions.
            if (!ParamProp->HasAnyPropertyFlags(CPF_Parm)) continue;

            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("name"), ParamProp->GetName());
            ParamObj->SetStringField(TEXT("cpp_type"), ParamProp->GetCPPType());
            ParamObj->SetStringField(TEXT("container"), ContainerToken(ParamProp));
            ParamObj->SetBoolField(TEXT("is_const"), ParamProp->HasAnyPropertyFlags(CPF_ConstParm));
            ParamObj->SetBoolField(TEXT("is_reference"), ParamProp->HasAnyPropertyFlags(CPF_ReferenceParm));
            ParamObj->SetBoolField(TEXT("is_out"), ParamProp->HasAnyPropertyFlags(CPF_OutParm)
                && !ParamProp->HasAnyPropertyFlags(CPF_ReturnParm));
            ParamObj->SetBoolField(TEXT("is_return"), ParamProp->HasAnyPropertyFlags(CPF_ReturnParm));

            if (ParamProp->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                ReturnRow = ParamObj;
            }
            else
            {
                ParamsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
            }
        }
        Row->SetArrayField(TEXT("params"), ParamsArr);
        if (ReturnRow.IsValid())
        {
            Row->SetObjectField(TEXT("return"), ReturnRow);
        }

        if (UStruct* OwnerStruct = Func->GetOwnerClass())
        {
            Row->SetStringField(TEXT("owner_class"), OwnerStruct->GetName());
            Row->SetStringField(TEXT("owner_class_path"), OwnerStruct->GetPathName());
            Row->SetBoolField(TEXT("inherited"), QueriedClass != nullptr && QueriedClass != OwnerStruct);
        }
        return Row;
    }

    /** Build the class header common to all ops. */
    void BuildClassHeader(UClass* Klass, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("class"), Klass->GetName());
        Out->SetStringField(TEXT("class_short"), Klass->GetName());
        Out->SetStringField(TEXT("class_path"), Klass->GetPathName());
        Out->SetBoolField(TEXT("is_native"), Klass->HasAnyClassFlags(CLASS_Native));
        Out->SetBoolField(TEXT("is_abstract"), Klass->HasAnyClassFlags(CLASS_Abstract));
        Out->SetBoolField(TEXT("is_blueprint"), IsBlueprintGeneratedClass(Klass));
        Out->SetBoolField(TEXT("is_interface"), Klass->HasAnyClassFlags(CLASS_Interface));
        Out->SetArrayField(TEXT("class_flags"), ClassFlagStrings(Klass->GetClassFlags()));

#if WITH_EDITORONLY_DATA
        const FString Tooltip = Klass->GetMetaData(TEXT("ToolTip"));
        if (!Tooltip.IsEmpty())
        {
            Out->SetStringField(TEXT("tooltip"), Tooltip);
        }
#endif

        if (UClass* Super = Klass->GetSuperClass())
        {
            AddClassRefField(Out, TEXT("parent"), Super);
        }
    }
}

FSproftUnrealApiCommands::FSproftUnrealApiCommands()
{
}

TSharedPtr<FJsonObject> FSproftUnrealApiCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("unreal_api"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown unreal_api command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing parameters"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("describe"))
    {
        return HandleDescribe(Params);
    }
    if (Op == TEXT("find_property"))
    {
        return HandleFindProperty(Params);
    }
    if (Op == TEXT("find_function"))
    {
        return HandleFindFunction(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("unreal_api: unsupported op '%s'. Supported: describe, find_property, find_function"), *Op));
}

TSharedPtr<FJsonObject> FSproftUnrealApiCommands::HandleDescribe(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassParam;
    if (!Params->TryGetStringField(TEXT("class"), ClassParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class' parameter"));
    }

    UClass* Klass = UnrealApi_ResolveClass(ClassParam);
    if (!Klass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to resolve class: %s"), *ClassParam));
    }

    bool bIncludeInherited = true;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
    bool bIncludeChildren = true;
    Params->TryGetBoolField(TEXT("include_children"), bIncludeChildren);

    int32 MaxChildren = 256;
    int32 MaxProperties = 512;
    int32 MaxFunctions = 512;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_children"), TempNum)) MaxChildren = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_properties"), TempNum)) MaxProperties = FMath::Max(0, static_cast<int32>(TempNum));
    if (Params->TryGetNumberField(TEXT("max_functions"), TempNum)) MaxFunctions = FMath::Max(0, static_cast<int32>(TempNum));

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    BuildClassHeader(Klass, Out);

    // Implemented interfaces. UClass::Interfaces is a TArray<FImplementedInterface>.
    TArray<TSharedPtr<FJsonValue>> InterfaceArr;
    for (const FImplementedInterface& Impl : Klass->Interfaces)
    {
        UClass* IfaceClass = Impl.Class;
        if (!IfaceClass) continue;
        TSharedPtr<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
        IfaceObj->SetStringField(TEXT("class"), IfaceClass->GetName());
        IfaceObj->SetStringField(TEXT("class_path"), IfaceClass->GetPathName());
        IfaceObj->SetBoolField(TEXT("is_native"), IfaceClass->HasAnyClassFlags(CLASS_Native));
        IfaceObj->SetBoolField(TEXT("implemented_by_k2"), Impl.bImplementedByK2);
        InterfaceArr.Add(MakeShared<FJsonValueObject>(IfaceObj));
    }
    Out->SetArrayField(TEXT("interfaces"), InterfaceArr);

    // Walk properties.
    const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
        ? EFieldIteratorFlags::IncludeSuper
        : EFieldIteratorFlags::ExcludeSuper;

    int32 PropsTotal = 0;
    TArray<TSharedPtr<FJsonValue>> PropsArr;
    for (TFieldIterator<FProperty> PropIt(Klass, SuperFlag); PropIt; ++PropIt)
    {
        ++PropsTotal;
        if (PropsArr.Num() >= MaxProperties) continue;
        FProperty* Prop = *PropIt;
        PropsArr.Add(MakeShared<FJsonValueObject>(BuildPropertyRecord(Prop, Klass)));
    }
    Out->SetArrayField(TEXT("properties"), PropsArr);
    Out->SetNumberField(TEXT("properties_total"), PropsTotal);
    Out->SetBoolField(TEXT("properties_truncated"), PropsArr.Num() < PropsTotal);

    // Walk functions.
    int32 FuncsTotal = 0;
    TArray<TSharedPtr<FJsonValue>> FuncsArr;
    for (TFieldIterator<UFunction> FuncIt(Klass, SuperFlag); FuncIt; ++FuncIt)
    {
        ++FuncsTotal;
        if (FuncsArr.Num() >= MaxFunctions) continue;
        UFunction* Func = *FuncIt;
        FuncsArr.Add(MakeShared<FJsonValueObject>(BuildFunctionRecord(Func, Klass)));
    }
    Out->SetArrayField(TEXT("functions"), FuncsArr);
    Out->SetNumberField(TEXT("functions_total"), FuncsTotal);
    Out->SetBoolField(TEXT("functions_truncated"), FuncsArr.Num() < FuncsTotal);

    // Children: GetDerivedClasses with bRecursive=false gives the
    // direct child set; we also report the recursive count.
    int32 ChildrenTotal = 0;
    int32 ChildrenRecursiveTotal = 0;
    if (bIncludeChildren)
    {
        TArray<UClass*> Direct;
        GetDerivedClasses(Klass, Direct, /*bRecursive=*/false);
        ChildrenTotal = Direct.Num();

        TArray<UClass*> Recursive;
        GetDerivedClasses(Klass, Recursive, /*bRecursive=*/true);
        ChildrenRecursiveTotal = Recursive.Num();

        TArray<TSharedPtr<FJsonValue>> ChildArr;
        for (UClass* Child : Direct)
        {
            if (ChildArr.Num() >= MaxChildren) break;
            if (!Child) continue;
            TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
            ChildObj->SetStringField(TEXT("class"), Child->GetName());
            ChildObj->SetStringField(TEXT("class_path"), Child->GetPathName());
            ChildObj->SetBoolField(TEXT("is_native"), Child->HasAnyClassFlags(CLASS_Native));
            ChildObj->SetBoolField(TEXT("is_abstract"), Child->HasAnyClassFlags(CLASS_Abstract));
            ChildArr.Add(MakeShared<FJsonValueObject>(ChildObj));
        }
        Out->SetArrayField(TEXT("children"), ChildArr);
        Out->SetBoolField(TEXT("children_truncated"), ChildArr.Num() < ChildrenTotal);
    }
    Out->SetNumberField(TEXT("child_count"), ChildrenTotal);
    Out->SetNumberField(TEXT("child_count_total"), ChildrenRecursiveTotal);

    return Out;
}

TSharedPtr<FJsonObject> FSproftUnrealApiCommands::HandleFindProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassParam;
    if (!Params->TryGetStringField(TEXT("class"), ClassParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class' parameter"));
    }
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }

    UClass* Klass = UnrealApi_ResolveClass(ClassParam);
    if (!Klass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to resolve class: %s"), *ClassParam));
    }

    bool bIncludeInherited = true;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
    int32 MaxProperties = 512;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_properties"), TempNum)) MaxProperties = FMath::Max(0, static_cast<int32>(TempNum));

    const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
        ? EFieldIteratorFlags::IncludeSuper
        : EFieldIteratorFlags::ExcludeSuper;
    const FString PatternLower = Pattern.ToLower();

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    BuildClassHeader(Klass, Out);
    Out->SetStringField(TEXT("pattern"), Pattern);

    int32 MatchedTotal = 0;
    TArray<TSharedPtr<FJsonValue>> MatchArr;
    for (TFieldIterator<FProperty> PropIt(Klass, SuperFlag); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop) continue;
        if (!Prop->GetName().ToLower().Contains(PatternLower)) continue;
        ++MatchedTotal;
        if (MatchArr.Num() >= MaxProperties) continue;
        MatchArr.Add(MakeShared<FJsonValueObject>(BuildPropertyRecord(Prop, Klass)));
    }
    Out->SetArrayField(TEXT("matches"), MatchArr);
    Out->SetNumberField(TEXT("matched_total"), MatchedTotal);
    Out->SetBoolField(TEXT("truncated"), MatchArr.Num() < MatchedTotal);
    return Out;
}

TSharedPtr<FJsonObject> FSproftUnrealApiCommands::HandleFindFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassParam;
    if (!Params->TryGetStringField(TEXT("class"), ClassParam))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class' parameter"));
    }
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }

    UClass* Klass = UnrealApi_ResolveClass(ClassParam);
    if (!Klass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to resolve class: %s"), *ClassParam));
    }

    bool bIncludeInherited = true;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
    int32 MaxFunctions = 512;
    double TempNum = 0.0;
    if (Params->TryGetNumberField(TEXT("max_functions"), TempNum)) MaxFunctions = FMath::Max(0, static_cast<int32>(TempNum));

    const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
        ? EFieldIteratorFlags::IncludeSuper
        : EFieldIteratorFlags::ExcludeSuper;
    const FString PatternLower = Pattern.ToLower();

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    BuildClassHeader(Klass, Out);
    Out->SetStringField(TEXT("pattern"), Pattern);

    int32 MatchedTotal = 0;
    TArray<TSharedPtr<FJsonValue>> MatchArr;
    for (TFieldIterator<UFunction> FuncIt(Klass, SuperFlag); FuncIt; ++FuncIt)
    {
        UFunction* Func = *FuncIt;
        if (!Func) continue;
        if (!Func->GetName().ToLower().Contains(PatternLower)) continue;
        ++MatchedTotal;
        if (MatchArr.Num() >= MaxFunctions) continue;
        MatchArr.Add(MakeShared<FJsonValueObject>(BuildFunctionRecord(Func, Klass)));
    }
    Out->SetArrayField(TEXT("matches"), MatchArr);
    Out->SetNumberField(TEXT("matched_total"), MatchedTotal);
    Out->SetBoolField(TEXT("truncated"), MatchArr.Num() < MatchedTotal);
    return Out;
}
