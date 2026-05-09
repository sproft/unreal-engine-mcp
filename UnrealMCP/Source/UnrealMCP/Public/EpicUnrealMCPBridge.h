#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Http.h"
#include "Json.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Commands/EpicUnrealMCPEditorCommands.h"
#include "Commands/EpicUnrealMCPBlueprintCommands.h"
#include "Commands/EpicUnrealMCPBlueprintGraphCommands.h"
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
#include "Commands/SproftBpVariableCommands.h"
#include "Commands/SproftBpClassCommands.h"
#include "Commands/SproftBpGraphCommands.h"
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
#include "EpicUnrealMCPBridge.generated.h"

class FMCPServerRunnable;

/**
 * Editor subsystem for MCP Bridge
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
UCLASS()
class UNREALMCP_API UEpicUnrealMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UEpicUnrealMCPBridge();
	virtual ~UEpicUnrealMCPBridge();

	// UEditorSubsystem implementation
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

	// Command execution
	FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// Server state
	bool bIsRunning;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

	// Command handler instances
	TSharedPtr<FEpicUnrealMCPEditorCommands> EditorCommands;
	TSharedPtr<FEpicUnrealMCPBlueprintCommands> BlueprintCommands;
	TSharedPtr<FEpicUnrealMCPBlueprintGraphCommands> BlueprintGraphCommands;
	// Sproft fork additions: lower-level primitives mirroring the hosted Flop tools
	TSharedPtr<FSproftEditorActionsCommands> SproftEditorActions;
	TSharedPtr<FSproftWindowCaptureCommands> SproftWindowCapture;
	TSharedPtr<FSproftAssetFactoryCommands> SproftAssetFactory;
	TSharedPtr<FSproftWidgetEditCommands> SproftWidgetEdit;
	TSharedPtr<FSproftEditorLogCommands> SproftEditorLog;
	TSharedPtr<FSproftBpInputCommands> SproftBpInput;
	TSharedPtr<FSproftWidgetInspectCommands> SproftWidgetInspect;
	TSharedPtr<FSproftBpComponentCommands> SproftBpComponent;
	TSharedPtr<FSproftSceneQueryCommands> SproftSceneQuery;
	TSharedPtr<FSproftMaterialEditCommands> SproftMaterialEdit;
	TSharedPtr<FSproftActorInspectCommands> SproftActorInspect;
	TSharedPtr<FSproftSceneComposeCommands> SproftSceneCompose;
	TSharedPtr<FSproftPythonExecutionCommands> SproftPythonExecution;
	TSharedPtr<FSproftSceneBriefCommands> SproftSceneBrief;
	TSharedPtr<FSproftLevelInspectCommands> SproftLevelInspect;
	TSharedPtr<FSproftTagRegistryEditCommands> SproftTagRegistryEdit;
	TSharedPtr<FSproftBpCreateCommands> SproftBpCreate;
	TSharedPtr<FSproftBpBriefCommands> SproftBpBrief;
	TSharedPtr<FSproftBpInspectCommands> SproftBpInspect;
	TSharedPtr<FSproftBpVariableCommands> SproftBpVariable;
	TSharedPtr<FSproftBpClassCommands> SproftBpClass;
	TSharedPtr<FSproftBpGraphCommands> SproftBpGraph;
	TSharedPtr<FSproftBpNodesCommands> SproftBpNodes;
	TSharedPtr<FSproftBpWireCommands> SproftBpWire;
	TSharedPtr<FSproftBpCommitCommands> SproftBpCommit;
	TSharedPtr<FSproftBpFunctionCreateCommands> SproftBpFunctionCreate;
	TSharedPtr<FSproftNiagaraInspectCommands> SproftNiagaraInspect;
	TSharedPtr<FSproftMaterialInspectCommands> SproftMaterialInspect;
	TSharedPtr<FSproftSearchAssetsCommands> SproftSearchAssets;
	TSharedPtr<FSproftAssetReferencesCommands> SproftAssetReferences;
	TSharedPtr<FSproftBpExportCommands> SproftBpExport;
	TSharedPtr<FSproftBehaviorTreeCommands> SproftBehaviorTree;
	TSharedPtr<FSproftGasEditCommands> SproftGasEdit;
	TSharedPtr<FSproftLandscapeInspectCommands> SproftLandscapeInspect;
	TSharedPtr<FSproftFoliageInspectCommands> SproftFoliageInspect;
};