#include "Commands/SproftMetaSoundEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Engine.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"
#include "Metasound.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "PerPlatformProperties.h"
#include "UObject/Package.h"

namespace
{
    void MetaSoundEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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

    bool ParseOutputFormat(const FString& Token, EMetaSoundOutputAudioFormat& Out)
    {
        const FString Lower = Token.ToLower();
        if (Lower == TEXT("mono") || Lower == TEXT("1.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Mono;
            return true;
        }
        if (Lower == TEXT("stereo") || Lower == TEXT("2.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Stereo;
            return true;
        }
        if (Lower == TEXT("quad") || Lower == TEXT("4.0"))
        {
            Out = EMetaSoundOutputAudioFormat::Quad;
            return true;
        }
        if (Lower == TEXT("5_1") || Lower == TEXT("5.1") || Lower == TEXT("fivedotone"))
        {
            Out = EMetaSoundOutputAudioFormat::FiveDotOne;
            return true;
        }
        if (Lower == TEXT("7_1") || Lower == TEXT("7.1") || Lower == TEXT("sevendotone"))
        {
            Out = EMetaSoundOutputAudioFormat::SevenDotOne;
            return true;
        }
        return false;
    }
}

FSproftMetaSoundEditCommands::FSproftMetaSoundEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType != TEXT("metasound_edit"))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown metasound_edit command: %s"), *CommandType));
    }
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Op;
    Params->TryGetStringField(TEXT("op"), Op);
    if (Op.IsEmpty() || Op == TEXT("create_metasound_source"))
    {
        return HandleCreateSource(Params);
    }
    if (Op == TEXT("create_metasound_patch"))
    {
        return HandleCreatePatch(Params);
    }
    if (Op == TEXT("add_node"))
    {
        return HandleAddNode(Params);
    }
    if (Op == TEXT("connect_nodes"))
    {
        return HandleConnectNodes(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("metasound_edit: unsupported op '%s'. Supported: create_metasound_source, create_metasound_patch, add_node, connect_nodes"), *Op));
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCreateSource(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    MetaSoundEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    EMetaSoundOutputAudioFormat OutputFormat = EMetaSoundOutputAudioFormat::Stereo;
    FString FormatToken;
    if (Params->TryGetStringField(TEXT("output_format"), FormatToken)
        || Params->TryGetStringField(TEXT("format"), FormatToken)
        || Params->TryGetStringField(TEXT("channels"), FormatToken))
    {
        if (!ParseOutputFormat(FormatToken, OutputFormat))
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unrecognised output_format '%s'. Use mono / stereo / quad / 5_1 / 7_1."),
                    *FormatToken));
        }
    }

    int32 SampleRate = 0;
    Params->TryGetNumberField(TEXT("sample_rate"), SampleRate);
    double BlockRate = 0.0;
    Params->TryGetNumberField(TEXT("block_rate"), BlockRate);

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UMetaSoundSource* NewSource = NewObject<UMetaSoundSource>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewSource)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMetaSoundSource"));
    }

    NewSource->OutputFormat = OutputFormat;
#if WITH_EDITORONLY_DATA
    if (SampleRate > 0)
    {
        NewSource->SampleRateOverride = SampleRate;
    }
    if (BlockRate > 0.0)
    {
        NewSource->BlockRateOverride = static_cast<float>(BlockRate);
    }
#endif

    // Initialise the document + register the editor graph so the asset
    // opens cleanly in the MetaSound editor. GetChecked() asserts the
    // subsystem exists; that's true whenever the editor module is up.
    UMetaSoundEditorSubsystem& EditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
    EditorSubsystem.InitAsset(*NewSource, /*ReferencedMetaSound*/ nullptr, /*bClearDocument*/ false);
    EditorSubsystem.RegisterGraphWithFrontend(*NewSource);

    FAssetRegistryModule::AssetCreated(NewSource);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_metasound_source"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), NewSource->GetClass()->GetName());
    Result->SetStringField(TEXT("output_format"), FormatToken.IsEmpty() ? TEXT("stereo") : FormatToken.ToLower());
    Result->SetNumberField(TEXT("sample_rate"), SampleRate);
    Result->SetNumberField(TEXT("block_rate"), BlockRate);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleCreatePatch(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("path"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset"), PackagePath)
        && !Params->TryGetStringField(TEXT("asset_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset path '%s' must start with /Game/"), *PackagePath));
    }

    FString PackageDir;
    FString AssetName;
    MetaSoundEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }
    const FString AssetObjectPath = PackageDir + AssetName;

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UMetaSoundPatch* NewPatch = NewObject<UMetaSoundPatch>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!NewPatch)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UMetaSoundPatch"));
    }

    UMetaSoundEditorSubsystem& EditorSubsystem = UMetaSoundEditorSubsystem::GetChecked();
    EditorSubsystem.InitAsset(*NewPatch, /*ReferencedMetaSound*/ nullptr, /*bClearDocument*/ false);
    EditorSubsystem.RegisterGraphWithFrontend(*NewPatch);

    FAssetRegistryModule::AssetCreated(NewPatch);
    Package->MarkPackageDirty();

    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("create_metasound_patch"));
    Result->SetStringField(TEXT("name"), AssetName);
    Result->SetStringField(TEXT("path"), AssetObjectPath);
    Result->SetStringField(TEXT("class"), NewPatch->GetClass()->GetName());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

namespace
{
    /** Resolve a target MetaSound asset (UMetaSoundSource / UMetaSoundPatch
     *  or any UObject implementing IMetaSoundDocumentInterface) and
     *  return the attached UMetaSoundBuilderBase. Errors land on
     *  OutError. */
    UMetaSoundBuilderBase* ResolveBuilderForAsset(const FString& AssetPath, UObject*& OutAsset, FString& OutError)
    {
        OutAsset = nullptr;
        UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("Could not load asset at '%s'"), *AssetPath);
            return nullptr;
        }
        if (!Asset->Implements<UMetaSoundDocumentInterface>())
        {
            OutError = FString::Printf(TEXT("Asset at '%s' does not implement IMetaSoundDocumentInterface"), *AssetPath);
            return nullptr;
        }
        UMetaSoundBuilderSubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>() : nullptr;
        if (!Subsystem)
        {
            OutError = TEXT("UMetaSoundBuilderSubsystem is not available");
            return nullptr;
        }
        UMetaSoundBuilderBase& Builder = Subsystem->AttachBuilderToAssetChecked(*Asset);
        OutAsset = Asset;
        return &Builder;
    }

    /** Build an FMetasoundFrontendClassName from a `Namespace.Name` /
     *  `Namespace.Name.Variant` token. The public Parse function on the
     *  struct does the parse. */
    bool ParseClassNameToken(const FString& Token, FMetasoundFrontendClassName& OutClassName, FString& OutError)
    {
        if (Token.IsEmpty())
        {
            OutError = TEXT("class_name was empty");
            return false;
        }
        if (FMetasoundFrontendClassName::Parse(Token, OutClassName))
        {
            return true;
        }
        // Fall back: bare name with empty namespace.
        OutClassName.Namespace = NAME_None;
        OutClassName.Name = FName(*Token);
        OutClassName.Variant = NAME_None;
        return true;
    }
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleAddNode(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' parameter"));
    }
    FString ClassNameToken;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassNameToken)
        && !Params->TryGetStringField(TEXT("node_class"), ClassNameToken)
        && !Params->TryGetStringField(TEXT("class"), ClassNameToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'class_name' parameter"));
    }
    int32 MajorVersion = 1;
    Params->TryGetNumberField(TEXT("major_version"), MajorVersion);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = nullptr;
    FString ResolveError;
    UMetaSoundBuilderBase* Builder = ResolveBuilderForAsset(AssetPath, Asset, ResolveError);
    if (!Builder)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }
    FMetasoundFrontendClassName ClassName;
    FString ParseError;
    if (!ParseClassNameToken(ClassNameToken, ClassName, ParseError))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ParseError);
    }

    EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
    FMetaSoundNodeHandle NodeHandle = Builder->AddNodeByClassName(ClassName, BuildResult, MajorVersion);
    if (BuildResult != EMetaSoundBuilderResult::Succeeded)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMetaSoundBuilderBase::AddNodeByClassName failed for '%s' (major %d)"),
                *ClassNameToken, MajorVersion));
    }

    Asset->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("add_node"));
    Result->SetStringField(TEXT("asset"), Asset->GetPathName());
    Result->SetStringField(TEXT("class_name"), ClassName.GetFullName().ToString());
    Result->SetStringField(TEXT("namespace"), ClassName.Namespace.ToString());
    Result->SetStringField(TEXT("name"), ClassName.Name.ToString());
    if (!ClassName.Variant.IsNone())
    {
        Result->SetStringField(TEXT("variant"), ClassName.Variant.ToString());
    }
    Result->SetNumberField(TEXT("major_version"), MajorVersion);
    Result->SetStringField(TEXT("node_id"), NodeHandle.NodeID.ToString());
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}

TSharedPtr<FJsonObject> FSproftMetaSoundEditCommands::HandleConnectNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset"), AssetPath)
        && !Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset' parameter"));
    }
    FString FromNodeIdString;
    if (!Params->TryGetStringField(TEXT("from_node"), FromNodeIdString)
        && !Params->TryGetStringField(TEXT("from_node_id"), FromNodeIdString)
        && !Params->TryGetStringField(TEXT("source_node"), FromNodeIdString))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_node' parameter (node ID GUID)"));
    }
    FString ToNodeIdString;
    if (!Params->TryGetStringField(TEXT("to_node"), ToNodeIdString)
        && !Params->TryGetStringField(TEXT("to_node_id"), ToNodeIdString)
        && !Params->TryGetStringField(TEXT("destination_node"), ToNodeIdString))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_node' parameter (node ID GUID)"));
    }
    FString OutputName;
    if (!Params->TryGetStringField(TEXT("from_output"), OutputName)
        && !Params->TryGetStringField(TEXT("output_name"), OutputName)
        && !Params->TryGetStringField(TEXT("from_pin"), OutputName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'from_output' parameter"));
    }
    FString InputName;
    if (!Params->TryGetStringField(TEXT("to_input"), InputName)
        && !Params->TryGetStringField(TEXT("input_name"), InputName)
        && !Params->TryGetStringField(TEXT("to_pin"), InputName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'to_input' parameter"));
    }
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UObject* Asset = nullptr;
    FString ResolveError;
    UMetaSoundBuilderBase* Builder = ResolveBuilderForAsset(AssetPath, Asset, ResolveError);
    if (!Builder)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(ResolveError);
    }

    FGuid FromGuid;
    if (!FGuid::Parse(FromNodeIdString, FromGuid))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'from_node' GUID '%s'"), *FromNodeIdString));
    }
    FGuid ToGuid;
    if (!FGuid::Parse(ToNodeIdString, ToGuid))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'to_node' GUID '%s'"), *ToNodeIdString));
    }
    FMetaSoundNodeHandle FromNode;
    FromNode.NodeID = FromGuid;
    FMetaSoundNodeHandle ToNode;
    ToNode.NodeID = ToGuid;

    EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
    Builder->ConnectNodes(FromNode, FName(*OutputName), ToNode, FName(*InputName), BuildResult);
    if (BuildResult != EMetaSoundBuilderResult::Succeeded)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMetaSoundBuilderBase::ConnectNodes failed (from %s.%s -> to %s.%s)"),
                *FromNodeIdString, *OutputName, *ToNodeIdString, *InputName));
    }

    Asset->MarkPackageDirty();
    if (bSave)
    {
        UEditorAssetLibrary::SaveAsset(Asset->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("operation"), TEXT("connect_nodes"));
    Result->SetStringField(TEXT("asset"), Asset->GetPathName());
    Result->SetStringField(TEXT("from_node"), FromNodeIdString);
    Result->SetStringField(TEXT("from_output"), OutputName);
    Result->SetStringField(TEXT("to_node"), ToNodeIdString);
    Result->SetStringField(TEXT("to_input"), InputName);
    Result->SetBoolField(TEXT("saved"), bSave);
    return Result;
}
