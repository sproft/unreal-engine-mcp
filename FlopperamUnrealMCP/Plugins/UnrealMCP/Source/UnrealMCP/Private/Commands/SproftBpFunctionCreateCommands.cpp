#include "Commands/SproftBpFunctionCreateCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Math/Color.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    UBlueprint* BpFunctionCreate_ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params)
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

    bool ValidateIdentifier(const FString& Name)
    {
        if (Name.IsEmpty()) { return false; }
        for (TCHAR Char : Name)
        {
            if (FChar::IsWhitespace(Char) || (!FChar::IsAlnum(Char) && Char != TEXT('_')))
            {
                return false;
            }
        }
        if (!FChar::IsAlpha(Name[0]) && Name[0] != TEXT('_'))
        {
            return false;
        }
        return true;
    }

    /** Type token resolver mirroring `bp_variable`. */
    bool BpFunctionCreate_ResolvePinTypeFromToken(const FString& InRaw, FEdGraphPinType& OutPinType, FString& OutError)
    {
        FString Token = InRaw.TrimStartAndEnd();
        if (Token.IsEmpty())
        {
            OutError = TEXT("type token is empty");
            return false;
        }

        if (Token.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase))
        {
            const FString Path = Token.Mid(7);
            UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Path);
            if (!Struct) { Struct = FindObject<UScriptStruct>(nullptr, *Path); }
            if (!Struct)
            {
                OutError = FString::Printf(TEXT("Could not load struct '%s'"), *Path);
                return false;
            }
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutPinType.PinSubCategoryObject = Struct;
            return true;
        }

        if (Token.StartsWith(TEXT("/Game/")))
        {
            FString WithSuffix = Token;
            if (!WithSuffix.EndsWith(TEXT("_C"))) { WithSuffix += TEXT("_C"); }
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

        if (Token.StartsWith(TEXT("/Script/")))
        {
            UClass* Klass = LoadClass<UObject>(nullptr, *Token);
            if (!Klass)
            {
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
            if (UClass* Klass = FindObject<UClass>(nullptr, *Candidate))
            {
                OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
                OutPinType.PinSubCategoryObject = Klass;
                return true;
            }
        }
        OutError = FString::Printf(TEXT("Could not resolve type token '%s'"), *Token);
        return false;
    }

    /** Set up a function-result node when one is needed. The standard flow
     *  in our existing FunctionIO helper does this without
     *  AllocateDefaultPins to avoid the well-known double-execute-pin
     *  bug. We follow the same approach here. */
    UK2Node_FunctionResult* EnsureResultNode(UEdGraph* Graph)
    {
        if (!Graph) { return nullptr; }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->IsA<UK2Node_FunctionResult>())
            {
                return Cast<UK2Node_FunctionResult>(Node);
            }
        }
        UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
        if (!Result) { return nullptr; }
        Result->NodePosX = 400;
        Result->NodePosY = 0;
        Result->CreateNewGuid();
        Graph->AddNode(Result, false, false);

        // Manually create the execute input pin BEFORE PostPlacedNewNode
        // (matches the comment in the existing FunctionIO helper).
        Result->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, FName(TEXT("execute")));
        Result->PostPlacedNewNode();
        return Result;
    }
}

FSproftBpFunctionCreateCommands::FSproftBpFunctionCreateCommands()
{
}

TSharedPtr<FJsonObject> FSproftBpFunctionCreateCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_function_create"))
    {
        return HandleFunctionCreate(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown bp_function_create command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftBpFunctionCreateCommands::HandleFunctionCreate(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    UBlueprint* Blueprint = BpFunctionCreate_ResolveBlueprintParam(Params);
    if (!Blueprint)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Blueprint not found or 'blueprint' parameter missing"));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName)
        && !Params->TryGetStringField(TEXT("name"), FunctionName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"));
    }
    if (!ValidateIdentifier(FunctionName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid function name '%s'"), *FunctionName));
    }

    // Reject collisions on existing function graphs by name. Auto-suffix
    // would silently change the name and break the caller's assumptions.
    for (UEdGraph* Existing : Blueprint->FunctionGraphs)
    {
        if (Existing && Existing->GetFName() == FName(*FunctionName))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Function '%s' already exists on Blueprint"), *FunctionName));
        }
    }

    bool bPure = false;
    Params->TryGetBoolField(TEXT("pure"), bPure);

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // Create the new graph.
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*FunctionName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create function graph for '%s'"), *FunctionName));
    }
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, nullptr);

    // Locate the FunctionEntry node spawned by AddFunctionGraph.
    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : NewGraph->Nodes)
    {
        if (Node && Node->IsA<UK2Node_FunctionEntry>())
        {
            EntryNode = Cast<UK2Node_FunctionEntry>(Node);
            break;
        }
    }
    if (!EntryNode)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("FunctionEntry node missing on new graph '%s'"), *FunctionName));
    }

    // Apply optional metadata + flags.
    if (bPure)
    {
        EntryNode->SetExtraFlags(EntryNode->GetExtraFlags() | FUNC_BlueprintPure);
    }
    FString CategoryStr;
    if (Params->TryGetStringField(TEXT("category"), CategoryStr))
    {
        EntryNode->MetaData.Category = FText::FromString(CategoryStr);
    }
    FString KeywordsStr;
    if (Params->TryGetStringField(TEXT("keywords"), KeywordsStr))
    {
        EntryNode->MetaData.Keywords = FText::FromString(KeywordsStr);
    }
    FString TooltipStr;
    if (Params->TryGetStringField(TEXT("tooltip"), TooltipStr))
    {
        EntryNode->MetaData.ToolTip = FText::FromString(TooltipStr);
    }
    FString CallInEditorStr;
    bool bCallInEditor = false;
    if (Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor))
    {
        EntryNode->MetaData.bCallInEditor = bCallInEditor;
    }

    auto SpecToString = [](const FString& Type, const FString& Name)
    {
        return FString::Printf(TEXT("%s %s"), *Type, *Name);
    };

    auto AddPinSpecs = [&](const TArray<TSharedPtr<FJsonValue>>& Specs, EEdGraphPinDirection Direction,
                           UK2Node_EditablePinBase* Owner, TArray<TSharedPtr<FJsonValue>>& OutLog)
    {
        for (const TSharedPtr<FJsonValue>& Entry : Specs)
        {
            TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
            if (!Entry.IsValid() || Entry->Type != EJson::Object)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("entry must be an object"));
                OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            const TSharedPtr<FJsonObject>& EntryObj = Entry->AsObject();
            FString PinName;
            if (!EntryObj->TryGetStringField(TEXT("name"), PinName) || !ValidateIdentifier(PinName))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("missing or invalid 'name'"));
                OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            EntryRow->SetStringField(TEXT("name"), PinName);
            FString TypeToken;
            if (!EntryObj->TryGetStringField(TEXT("type"), TypeToken))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("missing 'type'"));
                OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            EntryRow->SetStringField(TEXT("type"), TypeToken);

            FEdGraphPinType PinType;
            FString TypeError;
            if (!BpFunctionCreate_ResolvePinTypeFromToken(TypeToken, PinType, TypeError))
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TypeError);
                OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }
            bool bIsArray = false;
            if (EntryObj->TryGetBoolField(TEXT("is_array"), bIsArray) && bIsArray)
            {
                PinType.ContainerType = EPinContainerType::Array;
            }
            bool bIsRef = false;
            if (EntryObj->TryGetBoolField(TEXT("is_reference"), bIsRef) && bIsRef)
            {
                PinType.bIsReference = true;
            }

            UEdGraphPin* NewPin = Owner->CreateUserDefinedPin(*PinName, PinType, Direction);
            if (!NewPin)
            {
                EntryRow->SetBoolField(TEXT("success"), false);
                EntryRow->SetStringField(TEXT("error"), TEXT("CreateUserDefinedPin failed"));
                OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
                continue;
            }

            FString DefaultStr;
            if (EntryObj->TryGetStringField(TEXT("default"), DefaultStr) && !DefaultStr.IsEmpty())
            {
                NewPin->DefaultValue = DefaultStr;
                if (NewPin->GetSchema())
                {
                    NewPin->AutogeneratedDefaultValue = DefaultStr;
                }
            }
            EntryRow->SetBoolField(TEXT("success"), true);
            OutLog.Add(MakeShared<FJsonValueObject>(EntryRow));
        }
    };

    TArray<TSharedPtr<FJsonValue>> InputsLog;
    const TArray<TSharedPtr<FJsonValue>>* InputArr = nullptr;
    if (Params->TryGetArrayField(TEXT("inputs"), InputArr) && InputArr)
    {
        // Inputs go on the FunctionEntry as OUTPUT pins (the engine
        // reads them as values produced for the function body).
        AddPinSpecs(*InputArr, EGPD_Output, EntryNode, InputsLog);
    }

    TArray<TSharedPtr<FJsonValue>> OutputsLog;
    UK2Node_FunctionResult* ResultNode = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* OutputArr = nullptr;
    if (Params->TryGetArrayField(TEXT("outputs"), OutputArr) && OutputArr && OutputArr->Num() > 0)
    {
        ResultNode = EnsureResultNode(NewGraph);
        if (!ResultNode)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Failed to create FunctionResult node for outputs"));
        }
        // Outputs go on the FunctionResult as INPUT pins (the function
        // body feeds these on its way out).
        AddPinSpecs(*OutputArr, EGPD_Input, ResultNode, OutputsLog);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    NewGraph->NotifyGraphChanged();

    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_function"));
    Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("graph_name"), NewGraph->GetFName().ToString());
    Result->SetStringField(TEXT("entry_node"), EntryNode->GetFName().ToString());
    if (ResultNode)
    {
        Result->SetStringField(TEXT("result_node"), ResultNode->GetFName().ToString());
    }
    Result->SetBoolField(TEXT("pure"), bPure);
    Result->SetArrayField(TEXT("inputs_added"), InputsLog);
    Result->SetArrayField(TEXT("outputs_added"), OutputsLog);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
