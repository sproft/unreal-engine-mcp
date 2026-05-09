#include "Commands/SproftBpVariableCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedStruct.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Math/Color.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/UnrealType.h"

namespace
{
    UBlueprint* BpVariable_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
    {
        FString Input;
        if (!Params->TryGetStringField(TEXT("blueprint"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_path"), Input)
            && !Params->TryGetStringField(TEXT("blueprint_name"), Input))
        {
            return nullptr;
        }
        UBlueprint* BP = FEpicUnrealMCPCommonUtils::FindBlueprint(Input);
        if (!BP && Input.StartsWith(TEXT("/")))
        {
            BP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(Input));
        }
        return BP;
    }

    /** Convert a token (bool / int / float / vector / "/Script/..." / "/Game/...") into an FEdGraphPinType. */
    bool BpVariable_ResolvePinTypeFromToken(const FString& InRaw, FEdGraphPinType& OutPinType, FString& OutError)
    {
        FString Token = InRaw.TrimStartAndEnd();
        if (Token.IsEmpty())
        {
            OutError = TEXT("type token is empty");
            return false;
        }

        // struct:/Game/... or struct:/Script/Module.StructName
        if (Token.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase))
        {
            const FString Path = Token.Mid(7);
            UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Path);
            if (!Struct)
            {
                Struct = FindObject<UScriptStruct>(nullptr, *Path);
            }
            if (!Struct)
            {
                OutError = FString::Printf(TEXT("Could not load struct '%s'"), *Path);
                return false;
            }
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = Struct;
            return true;
        }

        // /Game/... Blueprint class path (resolves to the generated class).
        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C")))
            {
                WithSuffix += TEXT("_C");
            }
            UClass* GenClass = LoadClass<UObject>(nullptr, *WithSuffix);
            if (!GenClass)
            {
                OutError = FString::Printf(TEXT("Could not load Blueprint class '%s'"), *Token);
                return false;
            }
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            OutPinType.PinSubCategoryObject = GenClass;
            return true;
        }

        // /Script/Module.ClassName: object reference to a native UClass.
        if (Token.StartsWith(TEXT("/Script/")))
        {
            UClass* Klass = LoadClass<UObject>(nullptr, *Token);
            if (!Klass)
            {
                // Fall back: treat as a struct path, since /Script/CoreUObject.Vector etc. resolves there.
                if (UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Token))
                {
                    OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                    OutPinType.PinSubCategoryObject = Struct;
                    return true;
                }
                OutError = FString::Printf(TEXT("Could not load class '%s'"), *Token);
                return false;
            }
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            OutPinType.PinSubCategoryObject = Klass;
            return true;
        }

        // Scalar / built-in struct tokens (case-insensitive).
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
            return true;
        }
        if (Lower == TEXT("int") || Lower == TEXT("int32") || Lower == TEXT("integer"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
            return true;
        }
        if (Lower == TEXT("int64"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
            return true;
        }
        if (Lower == TEXT("byte") || Lower == TEXT("uint8"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            return true;
        }
        if (Lower == TEXT("float"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
            return true;
        }
        if (Lower == TEXT("double") || Lower == TEXT("real"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
            return true;
        }
        if (Lower == TEXT("string"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
            return true;
        }
        if (Lower == TEXT("name") || Lower == TEXT("fname"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
            return true;
        }
        if (Lower == TEXT("text") || Lower == TEXT("ftext"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
            return true;
        }
        if (Lower == TEXT("vector") || Lower == TEXT("fvector"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
            return true;
        }
        if (Lower == TEXT("vector2d") || Lower == TEXT("fvector2d") || Lower == TEXT("vec2"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
            return true;
        }
        if (Lower == TEXT("rotator") || Lower == TEXT("frotator"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
            return true;
        }
        if (Lower == TEXT("transform") || Lower == TEXT("ftransform"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
            return true;
        }
        if (Lower == TEXT("color") || Lower == TEXT("fcolor"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
            return true;
        }
        if (Lower == TEXT("linearcolor") || Lower == TEXT("linear_color") || Lower == TEXT("flinearcolor"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
            return true;
        }

        // Last-ditch: probe an in-memory UClass with optional A/U prefix variants.
        TArray<FString> Candidates;
        Candidates.Add(Token);
        if (!Token.StartsWith(TEXT("A")) && !Token.StartsWith(TEXT("U")))
        {
            Candidates.Add(TEXT("A") + Token);
            Candidates.Add(TEXT("U") + Token);
        }
        for (const FString& Candidate : Candidates)
        {
            if (UClass* Found = FindObject<UClass>(nullptr, *Candidate))
            {
                OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
                OutPinType.PinSubCategoryObject = Found;
                return true;
            }
        }
        for (const FString& Candidate : Candidates)
        {
            const FString Path = FString::Printf(TEXT("/Script/Engine.%s"), *Candidate);
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Path))
            {
                OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
                OutPinType.PinSubCategoryObject = Loaded;
                return true;
            }
        }

        OutError = FString::Printf(TEXT("Unrecognised type token '%s'"), *Token);
        return false;
    }

    FString BpVariable_DescribePinType(const FEdGraphPinType& PinType)
    {
        FString Result = PinType.PinCategory.ToString();
        if (!PinType.PinSubCategory.IsNone())
        {
            Result += TEXT(":") + PinType.PinSubCategory.ToString();
        }
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Result += TEXT("<") + PinType.PinSubCategoryObject->GetName() + TEXT(">");
        }
        if (PinType.IsArray())
        {
            Result += TEXT("[]");
        }
        else if (PinType.IsSet())
        {
            Result += TEXT("{set}");
        }
        else if (PinType.IsMap())
        {
            Result += TEXT("{map}");
        }
        return Result;
    }

    /** Render any FJsonValue into a default-value string. Lists and objects round-trip as compact JSON. */
    FString JsonToDefaultText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
            case EJson::String:
                return Value->AsString();
            case EJson::Number:
                return LexToString(Value->AsNumber());
            case EJson::Boolean:
                return Value->AsBool() ? TEXT("true") : TEXT("false");
            case EJson::Null:
                return FString();
            default:
            {
                FString Buffer;
                TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Buffer);
                FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
                return Buffer;
            }
        }
    }

    /** Find the `FBPVariableDescription` for a given variable name on a Blueprint. */
    FBPVariableDescription* FindVarByName(UBlueprint* Blueprint, const FName& VarName)
    {
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == VarName)
            {
                return &Var;
            }
        }
        return nullptr;
    }

    void SerialiseVariable(const FBPVariableDescription& Var, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetStringField(TEXT("name"), Var.VarName.ToString());
        Out->SetStringField(TEXT("type"), BpVariable_DescribePinType(Var.VarType));
        Out->SetStringField(TEXT("default_value"), Var.DefaultValue);
        Out->SetStringField(TEXT("category"), Var.Category.ToString());
        Out->SetStringField(TEXT("friendly_name"), Var.FriendlyName);
        Out->SetBoolField(TEXT("editable"), (Var.PropertyFlags & CPF_Edit) != 0);
        Out->SetBoolField(TEXT("blueprint_read_only"), (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
        Out->SetBoolField(TEXT("instance_editable"), (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
        Out->SetBoolField(TEXT("expose_on_spawn"), (Var.PropertyFlags & CPF_ExposeOnSpawn) != 0);
        Out->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
        Out->SetBoolField(TEXT("expose_to_cinematics"), (Var.PropertyFlags & CPF_Interp) != 0);
    }
}

FSproftBpVariableCommands::FSproftBpVariableCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_variable"))
    {
        return HandleBpVariable(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_variable command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::HandleBpVariable(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("op"), Operation)
        && !Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'op' parameter"));
    }
    Operation = Operation.ToLower();

    UBlueprint* Blueprint = BpVariable_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    if (Operation == TEXT("list") || Operation == TEXT("list_variables"))
    {
        return ListVariables(Blueprint);
    }
    if (Operation == TEXT("add") || Operation == TEXT("create"))
    {
        return AddVariable(Blueprint, Params);
    }
    if (Operation == TEXT("remove") || Operation == TEXT("delete"))
    {
        return RemoveVariable(Blueprint, Params);
    }
    if (Operation == TEXT("set_default"))
    {
        return SetDefault(Blueprint, Params);
    }
    if (Operation == TEXT("set_flags"))
    {
        return SetFlags(Blueprint, Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported bp_variable op '%s'. Supported: list, add, remove, set_default, set_flags"), *Operation));
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::ListVariables(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Vars;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        SerialiseVariable(Var, Entry);
        Vars.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("list"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetNumberField(TEXT("count"), Vars.Num());
    Result->SetArrayField(TEXT("variables"), Vars);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::AddVariable(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString NameInput;
    if (!Params->TryGetStringField(TEXT("name"), NameInput)
        && !Params->TryGetStringField(TEXT("variable_name"), NameInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is required"));
    }
    NameInput.TrimStartAndEndInline();
    if (NameInput.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is empty"));
    }
    if (!FName::IsValidXName(NameInput, INVALID_OBJECTNAME_CHARACTERS))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Variable name '%s' contains invalid characters"), *NameInput));
    }
    const FName VarName(*NameInput);

    if (FindVarByName(Blueprint, VarName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Variable '%s' already exists on Blueprint '%s'"), *NameInput, *Blueprint->GetName()));
    }

    FString TypeToken;
    if (!Params->TryGetStringField(TEXT("type"), TypeToken)
        && !Params->TryGetStringField(TEXT("variable_type"), TypeToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'type' parameter is required"));
    }
    FEdGraphPinType PinType;
    FString TypeError;
    if (!BpVariable_ResolvePinTypeFromToken(TypeToken, PinType, TypeError))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }

    // Container type: single, array, set, map.
    FString Container = TEXT("single");
    Params->TryGetStringField(TEXT("container"), Container);
    Container = Container.ToLower();
    if (Container == TEXT("array"))
    {
        PinType.ContainerType = EPinContainerType::Array;
    }
    else if (Container == TEXT("set"))
    {
        PinType.ContainerType = EPinContainerType::Set;
    }
    else if (Container == TEXT("map"))
    {
        PinType.ContainerType = EPinContainerType::Map;
        FString ValueTypeToken;
        if (!Params->TryGetStringField(TEXT("value_type"), ValueTypeToken))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Container 'map' requires a 'value_type' parameter"));
        }
        FEdGraphPinType ValuePinType;
        FString ValueError;
        if (!BpVariable_ResolvePinTypeFromToken(ValueTypeToken, ValuePinType, ValueError))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ValueError);
        }
        PinType.PinValueType = FEdGraphTerminalType::FromPinType(ValuePinType);
    }
    else if (Container != TEXT("single") && !Container.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported container '%s'. Supported: single, array, set, map"), *Container));
    }

    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FBlueprintEditorUtils::AddMemberVariable failed for '%s'"), *NameInput));
    }
    FBPVariableDescription* VarDesc = FindVarByName(Blueprint, VarName);
    if (!VarDesc)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Variable created but no FBPVariableDescription found post-add"));
    }

    // Default flags: blueprint visible. Caller can flip read-only off explicitly.
    VarDesc->PropertyFlags |= CPF_BlueprintVisible;

    // Optional metadata.
    FString Category;
    if (Params->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
    {
        VarDesc->Category = FText::FromString(Category);
    }
    FString FriendlyName;
    if (Params->TryGetStringField(TEXT("friendly_name"), FriendlyName))
    {
        VarDesc->FriendlyName = FriendlyName;
    }
    FString Tooltip;
    if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
    {
        VarDesc->SetMetaData(FBlueprintMetadata::MD_Tooltip, *Tooltip);
    }

    auto SetBoolFlag = [&](const TCHAR* Field, uint64 Flag, bool bInvertOnTrue = false)
    {
        bool bValue = false;
        if (!Params->TryGetBoolField(Field, bValue))
        {
            return;
        }
        const bool bSetFlag = bInvertOnTrue ? !bValue : bValue;
        if (bSetFlag)
        {
            VarDesc->PropertyFlags |= Flag;
        }
        else
        {
            VarDesc->PropertyFlags &= ~Flag;
        }
    };

    SetBoolFlag(TEXT("editable"), CPF_Edit);
    // blueprint_read_only is implicitly true by default; explicitly clearing makes it Set-able.
    SetBoolFlag(TEXT("blueprint_read_only"), CPF_BlueprintReadOnly);
    bool bWritable = false;
    if (Params->TryGetBoolField(TEXT("blueprint_writable"), bWritable))
    {
        if (bWritable)
        {
            VarDesc->PropertyFlags &= ~CPF_BlueprintReadOnly;
        }
        else
        {
            VarDesc->PropertyFlags |= CPF_BlueprintReadOnly;
        }
    }
    SetBoolFlag(TEXT("expose_on_spawn"), CPF_ExposeOnSpawn);
    SetBoolFlag(TEXT("replicated"), CPF_Net);
    SetBoolFlag(TEXT("expose_to_cinematics"), CPF_Interp);
    // instance_editable is the inverse of CPF_DisableEditOnInstance.
    bool bInstanceEditable = false;
    if (Params->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
    {
        if (bInstanceEditable)
        {
            VarDesc->PropertyFlags &= ~CPF_DisableEditOnInstance;
        }
        else
        {
            VarDesc->PropertyFlags |= CPF_DisableEditOnInstance;
        }
    }
    bool bPrivate = false;
    if (Params->TryGetBoolField(TEXT("private"), bPrivate))
    {
        if (bPrivate)
        {
            VarDesc->SetMetaData(TEXT("AllowPrivateAccess"), TEXT("true"));
        }
        else
        {
            VarDesc->RemoveMetaData(TEXT("AllowPrivateAccess"));
        }
    }

    // Convenience: map "expose_on_spawn=true" to the matching metadata as well so the
    // detail panel pickup matches FBPVariableDescription's serialised flag.
    if ((VarDesc->PropertyFlags & CPF_ExposeOnSpawn) != 0)
    {
        VarDesc->SetMetaData(TEXT("ExposeOnSpawn"), TEXT("true"));
    }

    // Optional default value.
    if (const TSharedPtr<FJsonValue>* DefaultPtr = Params->Values.Find(TEXT("default")))
    {
        VarDesc->DefaultValue = JsonToDefaultText(*DefaultPtr);
    }
    else if (const TSharedPtr<FJsonValue>* AltDefault = Params->Values.Find(TEXT("default_value")))
    {
        VarDesc->DefaultValue = JsonToDefaultText(*AltDefault);
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> VarSummary = MakeShared<FJsonObject>();
    SerialiseVariable(*VarDesc, VarSummary);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetObjectField(TEXT("variable"), VarSummary);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::RemoveVariable(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString NameInput;
    if (!Params->TryGetStringField(TEXT("name"), NameInput)
        && !Params->TryGetStringField(TEXT("variable_name"), NameInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is required"));
    }
    const FName VarName(*NameInput);
    if (!FindVarByName(Blueprint, VarName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Variable '%s' not found on Blueprint '%s'"), *NameInput, *Blueprint->GetName()));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("remove"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("name"), NameInput);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::SetDefault(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString NameInput;
    if (!Params->TryGetStringField(TEXT("name"), NameInput)
        && !Params->TryGetStringField(TEXT("variable_name"), NameInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is required"));
    }
    const FName VarName(*NameInput);
    FBPVariableDescription* VarDesc = FindVarByName(Blueprint, VarName);
    if (!VarDesc)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Variable '%s' not found on Blueprint '%s'"), *NameInput, *Blueprint->GetName()));
    }

    const TSharedPtr<FJsonValue>* DefaultPtr = Params->Values.Find(TEXT("default"));
    if (!DefaultPtr)
    {
        DefaultPtr = Params->Values.Find(TEXT("default_value"));
    }
    if (!DefaultPtr)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'default' (or 'default_value') parameter is required"));
    }
    VarDesc->DefaultValue = JsonToDefaultText(*DefaultPtr);

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> VarSummary = MakeShared<FJsonObject>();
    SerialiseVariable(*VarDesc, VarSummary);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_default"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetObjectField(TEXT("variable"), VarSummary);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftBpVariableCommands::SetFlags(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params)
{
    FString NameInput;
    if (!Params->TryGetStringField(TEXT("name"), NameInput)
        && !Params->TryGetStringField(TEXT("variable_name"), NameInput))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'name' parameter is required"));
    }
    const FName VarName(*NameInput);
    FBPVariableDescription* VarDesc = FindVarByName(Blueprint, VarName);
    if (!VarDesc)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Variable '%s' not found on Blueprint '%s'"), *NameInput, *Blueprint->GetName()));
    }

    const TSharedPtr<FJsonObject>* FlagsObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("flags"), FlagsObjPtr) || !FlagsObjPtr || !FlagsObjPtr->IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'flags' object parameter is required"));
    }

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    for (const auto& Pair : (*FlagsObjPtr)->Values)
    {
        const FString Key = Pair.Key.ToLower();
        bool bValue = false;
        if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Boolean)
        {
            bValue = Pair.Value->AsBool();
        }
        else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
        {
            bValue = Pair.Value->AsNumber() != 0.0;
        }
        else
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), Pair.Key);
            Skip->SetStringField(TEXT("reason"), TEXT("flag_value_not_boolean"));
            Skipped.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        auto AssignBit = [&](uint64 Flag, bool bInvert = false)
        {
            const bool bSet = bInvert ? !bValue : bValue;
            if (bSet)
            {
                VarDesc->PropertyFlags |= Flag;
            }
            else
            {
                VarDesc->PropertyFlags &= ~Flag;
            }
        };

        if (Key == TEXT("editable") || Key == TEXT("is_public"))
        {
            AssignBit(CPF_Edit);
        }
        else if (Key == TEXT("blueprint_read_only"))
        {
            AssignBit(CPF_BlueprintReadOnly);
        }
        else if (Key == TEXT("blueprint_writable"))
        {
            AssignBit(CPF_BlueprintReadOnly, /*bInvert=*/true);
        }
        else if (Key == TEXT("instance_editable"))
        {
            AssignBit(CPF_DisableEditOnInstance, /*bInvert=*/true);
        }
        else if (Key == TEXT("expose_on_spawn"))
        {
            AssignBit(CPF_ExposeOnSpawn);
            if (bValue)
            {
                VarDesc->SetMetaData(TEXT("ExposeOnSpawn"), TEXT("true"));
            }
            else
            {
                VarDesc->RemoveMetaData(TEXT("ExposeOnSpawn"));
            }
        }
        else if (Key == TEXT("replicated"))
        {
            AssignBit(CPF_Net);
        }
        else if (Key == TEXT("expose_to_cinematics"))
        {
            AssignBit(CPF_Interp);
        }
        else if (Key == TEXT("private"))
        {
            if (bValue)
            {
                VarDesc->SetMetaData(TEXT("AllowPrivateAccess"), TEXT("true"));
            }
            else
            {
                VarDesc->RemoveMetaData(TEXT("AllowPrivateAccess"));
            }
        }
        else
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), Pair.Key);
            Skip->SetStringField(TEXT("reason"), TEXT("unknown_flag"));
            Skipped.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
        Hit->SetStringField(TEXT("name"), Pair.Key);
        Hit->SetBoolField(TEXT("value"), bValue);
        Applied.Add(MakeShared<FJsonValueObject>(Hit));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> VarSummary = MakeShared<FJsonObject>();
    SerialiseVariable(*VarDesc, VarSummary);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("set_flags"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetObjectField(TEXT("variable"), VarSummary);
    Result->SetArrayField(TEXT("applied"), Applied);
    Result->SetArrayField(TEXT("skipped"), Skipped);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
