// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
	public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDefinitions.Add("UNREALMCP_EXPORTS=1");

		PublicIncludePaths.AddRange(
			new string[] {
				System.IO.Path.Combine(ModuleDirectory, "Public"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/BlueprintGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/BlueprintGraph/Nodes")
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				System.IO.Path.Combine(ModuleDirectory, "Private"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/BlueprintGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/BlueprintGraph/Nodes")
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Networking",
				"Sockets",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
				"PhysicsCore",
				"UnrealEd",           // For Blueprint editing
				"BlueprintGraph",     // For K2Node classes (F15-F22)
				"KismetCompiler",     // For Blueprint compilation (F15-F22)
				"UMG",                // For UWidget / UPanelWidget / UWidgetTree (Sproft widget_edit)
				"UMGEditor",          // For UWidgetBlueprint (Sproft widget_edit)
				"EnhancedInput",      // For UInputAction / UInputMappingContext (Sproft bp_input)
				"InputBlueprintNodes", // For UK2Node_EnhancedInputAction (Sproft bp_input graph wiring)
				"GameplayTags",       // For UGameplayTagsManager (Sproft tag_registry_edit)
				"Niagara",            // For UNiagaraSystem / UNiagaraEmitter (Sproft niagara_inspect)
				"AIModule",           // For UBehaviorTree / UBlackboardData (Sproft behavior_tree)
				"GameplayAbilities",  // For UGameplayAbility / UGameplayEffect / UAttributeSet (Sproft gas_edit)
				"Landscape",          // For ALandscape / ULandscapeInfo / ULandscapeLayerInfoObject (Sproft landscape_inspect)
				"Foliage",            // For AInstancedFoliageActor / UFoliageType (Sproft foliage_inspect)
				"MovieScene",         // For UMovieScene / UMovieSceneTrack / UMovieSceneSection (Sproft sequencer_edit)
				"LevelSequence",      // For ULevelSequence (Sproft sequencer_edit)
				"EngineSettings",     // For UGameMapsSettings (Sproft project_context); Projects is already in PrivateDependencyModuleNames below
				"RenderCore",         // For GGameThreadTime / GRenderThreadTime / GRHIThreadTime (Sproft performance_audit)
				"RHI",                // For GGPUFrameTime (Sproft performance_audit)
				"MetasoundEngine",    // For UMetaSoundSource / UMetaSoundPatch (Sproft metasound_edit)
				"MetasoundFrontend",  // For FMetasoundFrontendClassName::Parse / GetFullName (Sproft metasound_edit)
				"IKRig",              // For UIKRetargeter / FRetargetChainMapping / FInstancedStruct ops (Sproft ik_retarget)
				"GeometryCollectionEngine", // For UGeometryCollection (Sproft chaos_edit)
				"Chaos",              // For FGeometryCollection / FTransformCollection managed-array data (Sproft chaos_edit)
				"ImageWrapper",       // For IImageWrapperModule PNG decode (Sproft landscape_edit)
				"PCG",                // For UPCGGraph / UPCGNode / UPCGPin / UPCGEdge (Sproft pcg_graph_edit)
				"ModelViewViewModel", // For UMVVMViewModelBase + INotifyFieldValueChanged (Sproft widget_edit set_viewmodel)
				"FieldNotification"   // For INotifyFieldValueChanged interface check (Sproft widget_edit set_viewmodel)
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"EditorScriptingUtilities",
				"EditorSubsystem",
				"Slate",
				"SlateCore",
				"Kismet",
				"Projects",
				"AssetRegistry",
				"PythonScriptPlugin"  // For IPythonScriptPlugin (Sproft python_execution)
			}
		);

		if (Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"PropertyEditor",      // For property editing
					"ToolMenus",           // For editor UI
					"BlueprintEditorLibrary", // For Blueprint utilities
					"MaterialEditor",      // For UMaterialEditingLibrary (Sproft material_edit)
					"GameplayTagsEditor",  // For IGameplayTagsEditorModule (Sproft tag_registry_edit)
					"AnimationBlueprintLibrary", // For UAnimationBlueprintLibrary (Sproft animation_edit)
					"MetasoundEditor",     // For UMetaSoundEditorSubsystem (Sproft metasound_edit)
					"NiagaraEditor",       // For UNiagaraSystemFactoryNew::InitializeSystem (Sproft niagara_edit)
					"IKRigEditor",         // For UIKRigController::SetRetargetRoot / AddRetargetChain / AddNewGoal (Sproft ik_rig_edit)
					"GeometryCollectionEditor", // For FGeometryCollectionConversion::AppendStaticMesh (Sproft chaos_edit)
					"AnimGraph",           // For FEdGraphSchemaAction_NewStateNode / UAnimationStateMachineGraph (Sproft animation_graph_edit add_state)
					"ModelViewViewModelBlueprint",  // For UMVVMBlueprintView / UMVVMWidgetBlueprintExtension_View / FMVVMBlueprintViewModelContext (Sproft widget_edit set_viewmodel)
					"PlanarCut",           // For FPlanarCells + CutWithPlanarCells (Sproft chaos_edit fracture_box)
					"GeometryCore",        // Transitively pulled by PlanarCut; listed explicit for clarity (Sproft chaos_edit fracture_box)
					"DynamicMesh",         // Transitively pulled by PlanarCut; listed explicit for clarity (Sproft chaos_edit fracture_box)
					"GeometryAlgorithms"   // Transitively pulled by PlanarCut; listed explicit for clarity (Sproft chaos_edit fracture_box)
				}
			);
		}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
} 