#include "Commands/SproftWidgetEditCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
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
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
    /** Split "/Game/Foo/Bar" into ("/Game/Foo/", "Bar"). */
    void WidgetEdit_SplitPackagePath(const FString& InPath, FString& OutPackageDir, FString& OutAssetName)
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
    FString WidgetEdit_JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
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
    if (Operation == TEXT("add_animation") || Operation == TEXT("create_animation"))
    {
        return AddAnimation(Params);
    }
    if (Operation == TEXT("add_animation_track") || Operation == TEXT("add_track")
        || Operation == TEXT("animation_add_track"))
    {
        return AddAnimationTrack(Params);
    }

    return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unsupported widget_edit operation '%s'. Supported: create_widget_blueprint, add_child_widget, set_slot_property, add_animation, add_animation_track"), *Operation));
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
    WidgetEdit_SplitPackagePath(PackagePath, PackageDir, AssetName);
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

        const FString TextValue = WidgetEdit_JsonValueToImportText(JsonVal);
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

namespace
{
    /** Resolve a UMovieSceneTrack subclass by short token, full
     *  `/Script/Module.ClassName` path, or bare class name. Mirrors the
     *  resolver `sequencer_edit add_track` uses, narrowed to the tracks
     *  designers actually drop on a Widget animation (transform-style
     *  tracks plus the property tracks UMG materializes most often). */
    UClass* WidgetEdit_ResolveTrackClass(const FString& Token)
    {
        if (Token.IsEmpty())
        {
            return nullptr;
        }
        const FString Lower = Token.ToLower();
        struct FShortTokenMap
        {
            const TCHAR* Token;
            const TCHAR* Path;
        };
        static const FShortTokenMap Map[] = {
            { TEXT("float"),     TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack") },
            { TEXT("color"),     TEXT("/Script/MovieSceneTracks.MovieSceneColorTrack") },
            { TEXT("vector"),    TEXT("/Script/MovieSceneTracks.MovieSceneVectorTrack") },
            { TEXT("vector2d"),  TEXT("/Script/MovieSceneTracks.MovieSceneVectorTrack") },
            { TEXT("transform"), TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack") },
            { TEXT("visibility"),TEXT("/Script/MovieSceneTracks.MovieSceneVisibilityTrack") },
            { TEXT("bool"),      TEXT("/Script/MovieSceneTracks.MovieSceneBoolTrack") },
            { TEXT("byte"),      TEXT("/Script/MovieSceneTracks.MovieSceneByteTrack") },
            { TEXT("event"),     TEXT("/Script/MovieSceneTracks.MovieSceneEventTrack") },
            { TEXT("audio"),     TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack") },
            { TEXT("material"),  TEXT("/Script/MovieSceneTracks.MovieSceneComponentMaterialTrack") },
        };
        for (const FShortTokenMap& Entry : Map)
        {
            if (Lower == Entry.Token)
            {
                if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, Entry.Path))
                {
                    return Loaded;
                }
            }
        }

        if (Token.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *Token))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *Token))
        {
            if (Found->IsChildOf(UMovieSceneTrack::StaticClass()))
            {
                return Found;
            }
        }
        const FString TracksPath = FString::Printf(TEXT("/Script/MovieSceneTracks.%s"), *Token);
        if (UClass* Loaded = LoadClass<UMovieSceneTrack>(nullptr, *TracksPath))
        {
            return Loaded;
        }
        return nullptr;
    }

    /** Locate a UWidgetAnimation by FName on a target UWidgetBlueprint.
     *  Matches against the sub-object name (which is what the editor
     *  surfaces in the Animations panel) and falls back to the
     *  asset's display label. */
    UWidgetAnimation* WidgetEdit_FindAnimation(UWidgetBlueprint* WBP, const FString& AnimationName)
    {
        if (!WBP || AnimationName.IsEmpty())
        {
            return nullptr;
        }
        const FName TargetName(*AnimationName);
        for (TObjectPtr<UWidgetAnimation>& Anim : WBP->Animations)
        {
            UWidgetAnimation* Cur = Anim.Get();
            if (!Cur)
            {
                continue;
            }
            if (Cur->GetFName() == TargetName)
            {
                return Cur;
            }
            if (Cur->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return Cur;
            }
#if WITH_EDITOR
            if (Cur->GetDisplayLabel().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return Cur;
            }
#endif
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
        && !Params->TryGetStringField(TEXT("name"), AnimationName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'animation_name' parameter"));
    }

    double Duration = 1.0;
    if (Params->HasField(TEXT("duration")))
    {
        Duration = Params->GetNumberField(TEXT("duration"));
    }
    else if (Params->HasField(TEXT("duration_seconds")))
    {
        Duration = Params->GetNumberField(TEXT("duration_seconds"));
    }
    if (Duration <= 0.0)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'duration' must be greater than zero, got %f"), Duration));
    }

    double DisplayRateNumerator = 20.0;
    double DisplayRateDenominator = 1.0;
    if (Params->HasField(TEXT("display_rate")))
    {
        DisplayRateNumerator = Params->GetNumberField(TEXT("display_rate"));
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

    // Refuse a duplicate name to keep the variable surface stable; the editor
    // does the same when the user types a duplicate label.
    const FName TargetName(*AnimationName);
    for (const TObjectPtr<UWidgetAnimation>& Existing : WBP->Animations)
    {
        if (UWidgetAnimation* Cur = Existing.Get())
        {
            if (Cur->GetFName() == TargetName
                || Cur->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("An animation named '%s' already exists on %s"), *AnimationName, *WidgetBlueprintPath));
            }
        }
    }

    UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, TargetName, RF_Transactional);
    if (!NewAnim)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UWidgetAnimation"));
    }

    UMovieScene* NewScene = NewObject<UMovieScene>(NewAnim, TargetName, RF_Transactional);
    if (!NewScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to construct UMovieScene for animation"));
    }
    NewAnim->MovieScene = NewScene;
#if WITH_EDITOR
    NewAnim->SetDisplayLabel(AnimationName);
#endif

    // Match the AnimationTabSummoner default: 20 fps display rate. The
    // tick resolution stays on the engine default which is what the
    // Sequencer editor uses for new widget animations.
    NewScene->SetDisplayRate(FFrameRate(
        FMath::Max<int32>(1, static_cast<int32>(DisplayRateNumerator)),
        FMath::Max<int32>(1, static_cast<int32>(DisplayRateDenominator))));

    const FFrameTime EndFrame = Duration * NewScene->GetTickResolution();
    // The MovieScene uses an inclusive start / exclusive end bound, so we
    // bump the end by one tick to mirror the AnimationTabSummoner pattern.
    NewScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame.FrameNumber + 1));
#if WITH_EDITORONLY_DATA
    NewScene->GetEditorData().WorkStart = 0.0;
    NewScene->GetEditorData().WorkEnd = static_cast<float>(Duration);
#endif

    WBP->Animations.Add(NewAnim);
    WBP->OnVariableAdded(TargetName);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_animation"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
    ResultObj->SetNumberField(TEXT("duration"), Duration);
    ResultObj->SetNumberField(TEXT("display_rate_numerator"), DisplayRateNumerator);
    ResultObj->SetNumberField(TEXT("display_rate_denominator"), DisplayRateDenominator);
    ResultObj->SetNumberField(TEXT("animation_count"), WBP->Animations.Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}

TSharedPtr<FJsonObject> FSproftWidgetEditCommands::AddAnimationTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBlueprintPath;
    if (!Params->TryGetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_blueprint' parameter"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName)
        && !Params->TryGetStringField(TEXT("animation"), AnimationName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'animation_name' parameter"));
    }

    FString TargetWidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), TargetWidgetName)
        && !Params->TryGetStringField(TEXT("target_widget"), TargetWidgetName)
        && !Params->TryGetStringField(TEXT("target"), TargetWidgetName))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter (target widget on the WBP for the binding)"));
    }

    FString TrackToken;
    if (!Params->TryGetStringField(TEXT("track_class"), TrackToken)
        && !Params->TryGetStringField(TEXT("class"), TrackToken)
        && !Params->TryGetStringField(TEXT("track"), TrackToken))
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'track_class' parameter"));
    }

    FString PropertyPath;
    Params->TryGetStringField(TEXT("property_path"), PropertyPath);
    if (PropertyPath.IsEmpty())
    {
        Params->TryGetStringField(TEXT("property"), PropertyPath);
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

    UWidgetAnimation* Animation = WidgetEdit_FindAnimation(WBP, AnimationName);
    if (!Animation)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find animation '%s' on %s. Run widget_edit add_animation first."), *AnimationName, *WidgetBlueprintPath));
    }
    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Animation '%s' has no UMovieScene; widget_edit add_animation will repair this asset."), *AnimationName));
    }

    if (!WBP->WidgetTree)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(TEXT("WidgetBlueprint has no WidgetTree"));
    }
    UWidget* TargetWidget = WBP->WidgetTree->FindWidget(FName(*TargetWidgetName));
    if (!TargetWidget)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find target widget '%s' in %s"), *TargetWidgetName, *WidgetBlueprintPath));
    }

    UClass* TrackClass = WidgetEdit_ResolveTrackClass(TrackToken);
    if (!TrackClass)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve track class '%s'. Pass a short token (float / color / vector / transform / visibility / event / material / audio) or a UMovieSceneTrack subclass path."), *TrackToken));
    }

    // Reuse an existing FWidgetAnimationBinding for the same widget, or
    // create a fresh possessable + binding pair. This mirrors how the
    // editor wires a new track on an existing widget row in the Sequencer
    // panel: one MovieScene possessable + one FWidgetAnimationBinding the
    // runtime maps back through.
    FGuid BindingGuid;
    bool bReusedBinding = false;
    for (const FWidgetAnimationBinding& ExistingBind : Animation->GetBindings())
    {
        if (ExistingBind.WidgetName == TargetWidget->GetFName())
        {
            BindingGuid = ExistingBind.AnimationGuid;
            bReusedBinding = true;
            break;
        }
    }

    if (!BindingGuid.IsValid())
    {
        BindingGuid = MovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
        if (!BindingGuid.IsValid())
        {
            return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UMovieScene::AddPossessable failed for widget '%s'"), *TargetWidgetName));
        }

        // Wire the runtime binding so playback resolves the GUID back to
        // this UWidget once the preview / live UUserWidget instances spin up.
        // UWidgetAnimation::BindPossessableObject expects a UUserWidget
        // context that does not exist at asset-author time, so we write the
        // FWidgetAnimationBinding row directly. UMG's runtime resolution
        // path goes through FWidgetAnimationBinding::FindRuntimeObject,
        // which takes a WidgetTree + UUserWidget pair, so the binding row
        // is enough to round-trip the GUID at play time.
        FWidgetAnimationBinding NewBinding;
        NewBinding.WidgetName = TargetWidget->GetFName();
        NewBinding.AnimationGuid = BindingGuid;
        NewBinding.bIsRootWidget = (WBP->WidgetTree->RootWidget == TargetWidget);
        Animation->AnimationBindings.Add(NewBinding);
    }

    UMovieSceneTrack* NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
    if (!NewTrack)
    {
        return FEpicUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UMovieScene::AddTrack returned null for class '%s' on binding for widget '%s'"), *TrackClass->GetName(), *TargetWidgetName));
    }

    // Property tracks expect a property path ImportText'd onto the
    // reflected `PropertyPath` member; rather than tying us to per-class
    // includes for every property-track subclass, we route through the
    // reflection database. Older property-track classes use FName
    // PropertyName + FString PropertyPath; newer ones consolidate on a
    // PropertyPath name. We write whichever one the resolved class
    // declares. Designers calling without a property_path (transform /
    // visibility / event / material on a component slot) leave this
    // alone; the next Sequencer panel open populates the row.
    bool bAppliedPropertyPath = false;
    if (!PropertyPath.IsEmpty())
    {
        FOutputDeviceNull NullDevice;
        for (const TCHAR* PropName : { TEXT("PropertyPath"), TEXT("PropertyName") })
        {
            if (FProperty* Prop = FindFProperty<FProperty>(TrackClass, FName(PropName)))
            {
                if (Prop->ImportText_InContainer(*PropertyPath, NewTrack, NewTrack, PPF_None, &NullDevice) != nullptr)
                {
                    bAppliedPropertyPath = true;
                    break;
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (UPackage* Package = WBP->GetOutermost())
    {
        Package->MarkPackageDirty();
    }

    if (bSaveAfterEdit)
    {
        UEditorAssetLibrary::SaveAsset(WidgetBlueprintPath, /*bOnlyIfIsDirty=*/false);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("operation"), TEXT("add_animation_track"));
    ResultObj->SetStringField(TEXT("widget_blueprint"), WidgetBlueprintPath);
    ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
    ResultObj->SetStringField(TEXT("widget_name"), TargetWidgetName);
    ResultObj->SetStringField(TEXT("track_class"), TrackClass->GetName());
    ResultObj->SetStringField(TEXT("track_class_path"), TrackClass->GetPathName());
    ResultObj->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetBoolField(TEXT("reused_binding"), bReusedBinding);
    if (!PropertyPath.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("property_path"), PropertyPath);
        ResultObj->SetBoolField(TEXT("property_path_applied"), bAppliedPropertyPath);
    }
    ResultObj->SetBoolField(TEXT("saved"), bSaveAfterEdit);
    return ResultObj;
}
