#include "EpicUnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/EpicUnrealMCPEditorCommands.h"
#include "Commands/EpicUnrealMCPBlueprintCommands.h"
#include "Commands/EpicUnrealMCPBlueprintGraphCommands.h"
#include "Commands/EpicUnrealMCPCommonUtils.h"
#include "Commands/SproftEditorActionsCommands.h"
#include "Commands/SproftWindowCaptureCommands.h"
#include "Commands/SproftAssetFactoryCommands.h"
#include "Commands/SproftWidgetEditCommands.h"
#include "Commands/SproftEditorLogCommands.h"
#include "Commands/SproftBpInputCommands.h"
#include "Commands/SproftWidgetInspectCommands.h"
#include "Commands/SproftBpComponentCommands.h"
#include "Commands/SproftSceneQueryCommands.h"
#include "Commands/SproftMaterialEditCommands.h"
#include "Commands/SproftActorInspectCommands.h"
#include "Commands/SproftSceneComposeCommands.h"
#include "Commands/SproftPythonExecutionCommands.h"
#include "Commands/SproftSceneBriefCommands.h"
#include "Commands/SproftLevelInspectCommands.h"
#include "Commands/SproftTagRegistryEditCommands.h"
#include "Commands/SproftBpCreateCommands.h"
#include "Commands/SproftBpBriefCommands.h"
#include "Commands/SproftBpInspectCommands.h"
#include "Commands/SproftBpNodesCommands.h"
#include "Commands/SproftBpWireCommands.h"
#include "Commands/SproftBpCommitCommands.h"
#include "Commands/SproftBpFunctionCreateCommands.h"
#include "Commands/SproftNiagaraInspectCommands.h"
#include "Commands/SproftMaterialInspectCommands.h"
#include "Commands/SproftSearchAssetsCommands.h"
#include "Commands/SproftAssetReferencesCommands.h"
#include "Commands/SproftBpExportCommands.h"
#include "Commands/SproftBehaviorTreeCommands.h"
#include "Commands/SproftGasEditCommands.h"
#include "Commands/SproftLandscapeInspectCommands.h"
#include "Commands/SproftFoliageInspectCommands.h"
#include "Commands/SproftSequencerEditCommands.h"
#include "Commands/SproftProjectContextCommands.h"
#include "Commands/SproftAnimationInspectCommands.h"
#include "Commands/SproftCppSourceCommands.h"
#include "Commands/SproftPieTestSceneCommands.h"
#include "Commands/SproftAnimationEditCommands.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

UEpicUnrealMCPBridge::UEpicUnrealMCPBridge()
{
    EditorCommands = MakeShared<FEpicUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FEpicUnrealMCPBlueprintCommands>();
    BlueprintGraphCommands = MakeShared<FEpicUnrealMCPBlueprintGraphCommands>();
    SproftEditorActions = MakeShared<FSproftEditorActionsCommands>();
    SproftWindowCapture = MakeShared<FSproftWindowCaptureCommands>();
    SproftAssetFactory = MakeShared<FSproftAssetFactoryCommands>();
    SproftWidgetEdit = MakeShared<FSproftWidgetEditCommands>();
    SproftEditorLog = MakeShared<FSproftEditorLogCommands>();
    SproftBpInput = MakeShared<FSproftBpInputCommands>();
    SproftWidgetInspect = MakeShared<FSproftWidgetInspectCommands>();
    SproftBpComponent = MakeShared<FSproftBpComponentCommands>();
    SproftSceneQuery = MakeShared<FSproftSceneQueryCommands>();
    SproftMaterialEdit = MakeShared<FSproftMaterialEditCommands>();
    SproftActorInspect = MakeShared<FSproftActorInspectCommands>();
    SproftSceneCompose = MakeShared<FSproftSceneComposeCommands>();
    SproftPythonExecution = MakeShared<FSproftPythonExecutionCommands>();
    SproftSceneBrief = MakeShared<FSproftSceneBriefCommands>();
    SproftLevelInspect = MakeShared<FSproftLevelInspectCommands>();
    SproftTagRegistryEdit = MakeShared<FSproftTagRegistryEditCommands>();
    SproftBpCreate = MakeShared<FSproftBpCreateCommands>();
    SproftBpBrief = MakeShared<FSproftBpBriefCommands>();
    SproftBpInspect = MakeShared<FSproftBpInspectCommands>();
    SproftBpVariable = MakeShared<FSproftBpVariableCommands>();
    SproftBpClass = MakeShared<FSproftBpClassCommands>();
    SproftBpGraph = MakeShared<FSproftBpGraphCommands>();
    SproftBpNodes = MakeShared<FSproftBpNodesCommands>();
    SproftBpWire = MakeShared<FSproftBpWireCommands>();
    SproftBpCommit = MakeShared<FSproftBpCommitCommands>();
    SproftBpFunctionCreate = MakeShared<FSproftBpFunctionCreateCommands>();
    SproftNiagaraInspect = MakeShared<FSproftNiagaraInspectCommands>();
    SproftMaterialInspect = MakeShared<FSproftMaterialInspectCommands>();
    SproftSearchAssets = MakeShared<FSproftSearchAssetsCommands>();
    SproftAssetReferences = MakeShared<FSproftAssetReferencesCommands>();
    SproftBpExport = MakeShared<FSproftBpExportCommands>();
    SproftBehaviorTree = MakeShared<FSproftBehaviorTreeCommands>();
    SproftGasEdit = MakeShared<FSproftGasEditCommands>();
    SproftLandscapeInspect = MakeShared<FSproftLandscapeInspectCommands>();
    SproftFoliageInspect = MakeShared<FSproftFoliageInspectCommands>();
    SproftSequencerEdit = MakeShared<FSproftSequencerEditCommands>();
    SproftProjectContext = MakeShared<FSproftProjectContextCommands>();
    SproftAnimationInspect = MakeShared<FSproftAnimationInspectCommands>();
    SproftCppSource = MakeShared<FSproftCppSourceCommands>();
    SproftPieTestScene = MakeShared<FSproftPieTestSceneCommands>();
    SproftAnimationEdit = MakeShared<FSproftAnimationEditCommands>();
}

UEpicUnrealMCPBridge::~UEpicUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintGraphCommands.Reset();
    SproftEditorActions.Reset();
    SproftWindowCapture.Reset();
    SproftAssetFactory.Reset();
    SproftWidgetEdit.Reset();
    SproftEditorLog.Reset();
    SproftBpInput.Reset();
    SproftWidgetInspect.Reset();
    SproftBpComponent.Reset();
    SproftSceneQuery.Reset();
    SproftMaterialEdit.Reset();
    SproftActorInspect.Reset();
    SproftSceneCompose.Reset();
    SproftPythonExecution.Reset();
    SproftSceneBrief.Reset();
    SproftLevelInspect.Reset();
    SproftTagRegistryEdit.Reset();
    SproftBpCreate.Reset();
    SproftBpBrief.Reset();
    SproftBpInspect.Reset();
    SproftBpVariable.Reset();
    SproftBpClass.Reset();
    SproftBpGraph.Reset();
    SproftBpNodes.Reset();
    SproftBpWire.Reset();
    SproftBpCommit.Reset();
    SproftBpFunctionCreate.Reset();
    SproftNiagaraInspect.Reset();
    SproftMaterialInspect.Reset();
    SproftSearchAssets.Reset();
    SproftAssetReferences.Reset();
    SproftBpExport.Reset();
    SproftBehaviorTree.Reset();
    SproftGasEdit.Reset();
    SproftLandscapeInspect.Reset();
    SproftFoliageInspect.Reset();
    SproftSequencerEdit.Reset();
    SproftProjectContext.Reset();
    SproftAnimationInspect.Reset();
    SproftCppSource.Reset();
    SproftPieTestScene.Reset();
    SproftAnimationEdit.Reset();
}

// Initialize subsystem
void UEpicUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("EpicUnrealMCPBridge: Initializing"));
    
    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Start the server automatically
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UEpicUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("EpicUnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UEpicUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("EpicUnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("EpicUnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("EpicUnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("EpicUnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("EpicUnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("EpicUnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("EpicUnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UEpicUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("EpicUnrealMCPBridge: Server stopped"));
}

// Execute a command received from a client
FString UEpicUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Display, TEXT("EpicUnrealMCPBridge: Executing command: %s"), *CommandType);
    
    // Create a promise to wait for the result
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();
    
    // Queue execution on Game Thread
    AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);
        
        try
        {
            TSharedPtr<FJsonObject> ResultJson;
            
            if (CommandType == TEXT("ping"))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            }
            // Editor Commands (including actor manipulation)
            else if (CommandType == TEXT("get_actors_in_level") || 
                     CommandType == TEXT("find_actors_by_name") ||
                     CommandType == TEXT("spawn_actor") ||
                     CommandType == TEXT("delete_actor") || 
                     CommandType == TEXT("set_actor_transform") ||
                     CommandType == TEXT("spawn_blueprint_actor"))
            {
                ResultJson = EditorCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Commands
            else if (CommandType == TEXT("create_blueprint") ||
                     CommandType == TEXT("add_component_to_blueprint") ||
                     CommandType == TEXT("set_physics_properties") ||
                     CommandType == TEXT("compile_blueprint") ||
                     CommandType == TEXT("set_static_mesh_properties") ||
                     CommandType == TEXT("set_mesh_material_color") ||
                     CommandType == TEXT("get_available_materials") ||
                     CommandType == TEXT("apply_material_to_actor") ||
                     CommandType == TEXT("apply_material_to_blueprint") ||
                     CommandType == TEXT("get_actor_material_info") ||
                     CommandType == TEXT("get_blueprint_material_info") ||
                     CommandType == TEXT("read_blueprint_content") ||
                     CommandType == TEXT("analyze_blueprint_graph") ||
                     CommandType == TEXT("get_blueprint_variable_details") ||
                     CommandType == TEXT("get_blueprint_function_details"))
            {
                ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Graph Commands
            else if (CommandType == TEXT("add_blueprint_node") ||
                     CommandType == TEXT("connect_nodes") ||
                     CommandType == TEXT("create_variable") ||
                     CommandType == TEXT("set_blueprint_variable_properties") ||
                     CommandType == TEXT("add_event_node") ||
                     CommandType == TEXT("delete_node") ||
                     CommandType == TEXT("set_node_property") ||
                     CommandType == TEXT("create_function") ||
                     CommandType == TEXT("add_function_input") ||
                     CommandType == TEXT("add_function_output") ||
                     CommandType == TEXT("delete_function") ||
                     CommandType == TEXT("rename_function"))
            {
                ResultJson = BlueprintGraphCommands->HandleCommand(CommandType, Params);
            }
            // Sproft fork additions
            else if (CommandType == TEXT("editor_actions"))
            {
                ResultJson = SproftEditorActions->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("window_capture"))
            {
                ResultJson = SproftWindowCapture->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("asset_factory"))
            {
                ResultJson = SproftAssetFactory->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("widget_edit"))
            {
                ResultJson = SproftWidgetEdit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("editor_log"))
            {
                ResultJson = SproftEditorLog->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_input"))
            {
                ResultJson = SproftBpInput->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("widget_inspect"))
            {
                ResultJson = SproftWidgetInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_component"))
            {
                ResultJson = SproftBpComponent->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("scene_query"))
            {
                ResultJson = SproftSceneQuery->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("material_edit"))
            {
                ResultJson = SproftMaterialEdit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("actor_inspect"))
            {
                ResultJson = SproftActorInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("scene_compose"))
            {
                ResultJson = SproftSceneCompose->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("python_execution"))
            {
                ResultJson = SproftPythonExecution->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("scene_brief"))
            {
                ResultJson = SproftSceneBrief->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("level_inspect"))
            {
                ResultJson = SproftLevelInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("tag_registry_edit"))
            {
                ResultJson = SproftTagRegistryEdit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_create"))
            {
                ResultJson = SproftBpCreate->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_brief"))
            {
                ResultJson = SproftBpBrief->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_inspect"))
            {
                ResultJson = SproftBpInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_variable"))
            {
                ResultJson = SproftBpVariable->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_class"))
            {
                ResultJson = SproftBpClass->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_graph"))
            {
                ResultJson = SproftBpGraph->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_nodes"))
            {
                ResultJson = SproftBpNodes->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_wire"))
            {
                ResultJson = SproftBpWire->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_commit"))
            {
                ResultJson = SproftBpCommit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_function_create"))
            {
                ResultJson = SproftBpFunctionCreate->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("niagara_inspect"))
            {
                ResultJson = SproftNiagaraInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("material_inspect"))
            {
                ResultJson = SproftMaterialInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("search_assets"))
            {
                ResultJson = SproftSearchAssets->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("asset_references"))
            {
                ResultJson = SproftAssetReferences->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("bp_export"))
            {
                ResultJson = SproftBpExport->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("behavior_tree"))
            {
                ResultJson = SproftBehaviorTree->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("gas_edit"))
            {
                ResultJson = SproftGasEdit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("landscape_inspect"))
            {
                ResultJson = SproftLandscapeInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("foliage_inspect"))
            {
                ResultJson = SproftFoliageInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("sequencer_edit"))
            {
                ResultJson = SproftSequencerEdit->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("project_context"))
            {
                ResultJson = SproftProjectContext->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("animation_inspect"))
            {
                ResultJson = SproftAnimationInspect->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("cpp_source"))
            {
                ResultJson = SproftCppSource->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("pie_test_scene"))
            {
                ResultJson = SproftPieTestScene->HandleCommand(CommandType, Params);
            }
            else if (CommandType == TEXT("animation_edit"))
            {
                ResultJson = SproftAnimationEdit->HandleCommand(CommandType, Params);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));
                
                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                Promise.SetValue(ResultString);
                return;
            }
            
            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;
            
            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess && ResultJson->HasField(TEXT("error")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the error message
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }
        
        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise.SetValue(ResultString);
    });
    
    return Future.Get();
}