#include "Commands/SproftWidgetEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/OutputDeviceNull.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
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

        // Strip a trailing ".AssetName" suffix if the caller passed an object path.
        int32 DotIdx = INDEX_NONE;
        if (OutAssetName.FindChar('.', DotIdx))
        {
            OutAssetName = OutAssetName.Left(DotIdx);
        }
    }

    /** Resolve a UUserWidget subclass for the parent_class param. Defaults to UUserWidget. */
    UClass* ResolveUserWidgetParent(const FString& ParentClassPath)
    {
        if (ParentClassPath.IsEmpty())
        {
            return UUserWidget::StaticClass();
        }

        // Full object path?
        if (ParentClassPath.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UUserWidget>(nullptr, *ParentClassPath))
            {
                return Loaded;
            }
            // Caller may have passed a Blueprint asset path without a "_C" suffix.
            const FString WithSuffix = ParentClassPath + TEXT("_C");
            if (UClass* LoadedSuffix = LoadClass<UUserWidget>(nullptr, *WithSuffix))
            {
                return LoadedSuffix;
            }
            return nullptr;
        }

        // Short name lookup, e.g. "UUserWidget".
        if (UClass* Found = FindObject<UClass>(nullptr, *ParentClassPath))
        {
            if (Found->IsChildOf(UUserWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *ParentClassPath);
        if (UClass* UMG = LoadClass<UUserWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }

    /** Resolve a UPanelWidget subclass for an optional root panel. Defaults to UCanvasPanel. */
    UClass* ResolveRootPanelClass(const FString& RootPanelClass)
    {
        if (RootPanelClass.IsEmpty())
        {
            return UCanvasPanel::StaticClass();
        }

        if (RootPanelClass.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UPanelWidget>(nullptr, *RootPanelClass))
            {
                return Loaded;
            }
            return nullptr;
        }

        if (UClass* Found = FindObject<UClass>(nullptr, *RootPanelClass))
        {
            if (Found->IsChildOf(UPanelWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *RootPanelClass);
        if (UClass* UMG = LoadClass<UPanelWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }

    /** Render a JSON value as ImportText input. Mirrors the helper used
     *  in bp_component / scene_compose so the slot-property surface
     *  accepts the same dict shape as the rest of the property tools. */
    FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
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
                return TEXT("None");
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

    /** Map a short widget-type string to its UClass. Returns nullptr if unknown. */
    UClass* ResolveChildWidgetClass(const FString& WidgetType)
    {
        const FString Type = WidgetType.ToLower();

        if (Type == TEXT("vertical_box") || Type == TEXT("verticalbox"))
        {
            return UVerticalBox::StaticClass();
        }
        if (Type == TEXT("horizontal_box") || Type == TEXT("horizontalbox"))
        {
            return UHorizontalBox::StaticClass();
        }
        if (Type == TEXT("canvas_panel") || Type == TEXT("canvaspanel"))
        {
            return UCanvasPanel::StaticClass();
        }
        if (Type == TEXT("overlay"))
        {
            return UOverlay::StaticClass();
        }
        if (Type == TEXT("scroll_box") || Type == TEXT("scrollbox"))
        {
            return UScrollBox::StaticClass();
        }
        if (Type == TEXT("border"))
        {
            return UBorder::StaticClass();
        }
        if (Type == TEXT("size_box") || Type == TEXT("sizebox"))
        {
            return USizeBox::StaticClass();
        }
        if (Type == TEXT("spacer"))
        {
            return USpacer::StaticClass();
        }
        if (Type == TEXT("progress_bar") || Type == TEXT("progressbar"))
        {
            return UProgressBar::StaticClass();
        }
        if (Type == TEXT("text_block") || Type == TEXT("textblock") || Type == TEXT("text"))
        {
            return UTextBlock::StaticClass();
        }
        if (Type == TEXT("button"))
        {
            return UButton::StaticClass();
        }
        if (Type == TEXT("image"))
        {
            return UImage::StaticClass();
        }

        // Fall back to a class lookup. Accept fully qualified names like
        // "/Script/UMG.RichTextBlock" or short class names.
        if (WidgetType.StartsWith(TEXT("/")))
        {
            if (UClass* Loaded = LoadClass<UWidget>(nullptr, *WidgetType))
            {
                return Loaded;
            }
            return nullptr;
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *WidgetType))
        {
            if (Found->IsChildOf(UWidget::StaticClass()))
            {
                return Found;
            }
        }
        const FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *WidgetType);
        if (UClass* UMG = LoadClass<UWidget>(nullptr, *UMGPath))
        {
            return UMG;
        }
        return nullptr;
    }
}

FSproftWidgetEditCommands::FSproftWidgetEditCommands()
{
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("widget_edit"))
    {
        return HandleWidgetEdit(Params);
    }
    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown widget edit command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::HandleWidgetEdit(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing params object"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }
    Operation = Operation.ToLower();

    if (Operation == TEXT("create_widget_blueprint") || Operation == TEXT("create"))
    {
        return CreateWidgetBlueprint(Params);
    }
    if (Operation == TEXT("add_child_widget") || Operation == TEXT("add_child"))
    {
        return AddChildWidget(Params);
    }
    if (Operation == TEXT("set_slot_property") || Operation == TEXT("set_slot")
        || Operation == TEXT("slot_set") || Operation == TEXT("set_slot_properties"))
    {
        return SetSlotProperty(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported widget_edit operation '%s'. Supported: create_widget_blueprint, add_child_widget, set_slot_property"), *Operation));
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'package_path' parameter"));
    }
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'package_path' must be an absolute content-browser path, got '%s'"), *PackagePath));
    }

    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);

    FString RootPanelClass;
    Params->TryGetStringField(TEXT("root_panel_class"), RootPanelClass);

    bool bSaveAfterCreate = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterCreate);

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    FString PackageDir;
    FString AssetName;
    SplitPackagePath(PackagePath, PackageDir, AssetName);
    if (AssetName.IsEmpty())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not derive asset name from '%s'"), *PackagePath));
    }

    const FString AssetObjectPath = PackageDir + AssetName;
    if (UEditorAssetLibrary::DoesAssetExist(AssetObjectPath) && !bOverwrite)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s (set 'overwrite': true to replace)"), *AssetObjectPath));
    }

    UClass* ParentClass = ResolveUserWidgetParent(ParentClassPath);
    if (!ParentClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent_class '%s'. Pass a UUserWidget subclass path or short name."), *ParentClassPath));
    }

    UClass* RootPanel = ResolveRootPanelClass(RootPanelClass);
    if (!RootPanel)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve root_panel_class '%s'. Pass a UPanelWidget subclass path or short name."), *RootPanelClass));
    }

    UPackage* Package = CreatePackage(*AssetObjectPath);
    if (!Package)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package at '%s'"), *AssetObjectPath));
    }
    Package->FullyLoad();

    UWidgetBlueprint* NewWBP = CastChecked<UWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            Package,
            *AssetName,
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(),
            FName("Sproft.widget_edit")));

    if (!NewWBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create UWidgetBlueprint"));
    }

    if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget == nullptr)
    {
        UWidget* Root = NewWBP->WidgetTree->ConstructWidget<UWidget>(RootPanel);
        if (Root)
        {
            NewWBP->WidgetTree->RootWidget = Root;
            NewWBP->OnVariableAdded(Root->GetFName());
        }
    }

    FAssetRegistryModule::AssetCreated(NewWBP);
    FBlueprintEditorUtils::MarkBlueprintAsModified(NewWBP);
    Package->MarkPackageDirty();

    if (bSaveAfterCreate)
    {
        UEditorAssetLibrary::SaveAsset(AssetObjectPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("create_widget_blueprint"));
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), AssetObjectPath);
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    ResultObj->SetStringField(TEXT("root_panel_class"), RootPanel->GetPathName());
    if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget)
    {
        ResultObj->SetStringField(TEXT("root_widget_name"), NewWBP->WidgetTree->RootWidget->GetName());
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterCreate);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddChildWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString WidgetType;
    if (!Params->TryGetStringField(TEXT("widget_type"), WidgetType))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_type' parameter"));
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }

    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    bool bExposeAsVariable = true;
    Params->TryGetBoolField(TEXT("expose_as_variable"), bExposeAsVariable);

    FString InitialText;
    Params->TryGetStringField(TEXT("text"), InitialText);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }

    UClass* ChildClass = ResolveChildWidgetClass(WidgetType);
    if (!ChildClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported widget_type '%s'. Pass one of: vertical_box, horizontal_box, canvas_panel, overlay, scroll_box, border, size_box, spacer, progress_bar, text_block, button, image, or a fully qualified UWidget class path."), *WidgetType));
    }

    // Resolve the parent panel.
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
        if (!ParentPanel)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Root widget is not a UPanelWidget. Pass 'parent_name' or rebuild the asset with a panel root."));
        }
    }
    else
    {
        UWidget* Found = WBP->WidgetTree->FindWidget(FName(*ParentName));
        if (!Found)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not find parent widget '%s' in %s"), *ParentName, *WidgetBlueprintPath));
        }
        ParentPanel = Cast<UPanelWidget>(Found);
        if (!ParentPanel)
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent widget '%s' is not a UPanelWidget"), *ParentName));
        }
    }

    // Refuse a duplicate name in the tree, since UWidgetTree requires unique
    // FNames per widget asset.
    if (UWidget* Existing = WBP->WidgetTree->FindWidget(FName(*WidgetName)))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("A widget named '%s' already exists in %s"), *WidgetName, *WidgetBlueprintPath));
    }

    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(ChildClass, FName(*WidgetName));
    if (!NewWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to construct widget of type '%s'"), *WidgetType));
    }

    NewWidget->bIsVariable = bExposeAsVariable;

    UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
    if (!Slot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent panel '%s' refused to attach the new widget"), *ParentPanel->GetName()));
    }

    // Apply a small set of optional initial properties.
    if (!InitialText.IsEmpty())
    {
        if (UTextBlock* AsText = Cast<UTextBlock>(NewWidget))
        {
            AsText->SetText(FText::FromString(InitialText));
        }
    }

    if (bExposeAsVariable)
    {
        WBP->OnVariableAdded(NewWidget->GetFName());
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_child_widget"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("widget_type"), WidgetType);
    ResultObj->SetStringField(TEXT("widget_class"), ChildClass->GetPathName());
    ResultObj->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    ResultObj->SetBoolField(TEXT("expose_as_variable"), bExposeAsVariable);
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::SetSlotProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName)
        && !Params->TryGetStringField(TEXT("target"), WidgetName)
        && !Params->TryGetStringField(TEXT("name"), WidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !(*PropsObj).IsValid())
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'properties' object"));
    }
    if ((*PropsObj)->Values.Num() == 0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'properties' object is empty"));
    }

    bool bSaveAfterEdit = true;
    Params->TryGetBoolField(TEXT("save"), bSaveAfterEdit);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetBlueprintPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetBlueprintPath));
    }
    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }

    UWidget* Found = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Found)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find widget '%s' in %s"), *WidgetName, *WidgetBlueprintPath));
    }

    UPanelSlot* Slot = Found->Slot;
    if (!Slot)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget '%s' has no Slot. The root widget has no parent panel; only children of a panel widget carry a slot."), *WidgetName));
    }

    UClass* SlotClass = Slot->GetClass();

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    TArray<TSharedPtr<FJsonValue>> SkippedJson;
    FOutputDeviceNull NullDevice;
    for (const auto& Pair : (*PropsObj)->Values)
    {
        const FString& PropName = Pair.Key;
        const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

        FProperty* Prop = FindFProperty<FProperty>(SlotClass, *PropName);
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("not_a_uproperty"));
            SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        const FString TextValue = JsonValueToImportText(JsonVal);
        const TCHAR* TextPtr = *TextValue;
        const TCHAR* Result = Prop->ImportText_InContainer(
            TextPtr, Slot, Slot, PPF_None, &NullDevice);
        if (Result == nullptr)
        {
            TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
            Skip->SetStringField(TEXT("name"), PropName);
            Skip->SetStringField(TEXT("reason"), TEXT("import_text_failed"));
            Skip->SetStringField(TEXT("attempted_value"), TextValue);
            SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
            continue;
        }

        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
        Applied->SetStringField(TEXT("name"), PropName);
        Applied->SetStringField(TEXT("type"), Prop->GetCPPType());
        AppliedJson.Add(MakeShared<FJsonValueObject>(Applied));
    }

    // Push the slot's edits back through the runtime hook so a re-layout
    // tick picks them up. Most slot classes implement SynchronizeProperties()
    // to copy serialised state onto the live SObjectWidget; the base
    // UPanelSlot calls it from PostEditChangeProperty, so we replicate that
    // here for the reflective dict path.
    Slot->SynchronizeProperties();

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("set_slot_property"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("slot_class"), SlotClass->GetName());
    ResultObj->SetStringField(TEXT("slot_class_path"), SlotClass->GetPathName());
    ResultObj->SetArrayField(TEXT("applied"), AppliedJson);
    ResultObj->SetArrayField(TEXT("skipped"), SkippedJson);
    ResultObj->SetNumberField(TEXT("applied_count"), AppliedJson.Num());
    ResultObj->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}
