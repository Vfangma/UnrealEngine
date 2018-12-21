// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Tools.DotNETCommon;

namespace UnrealBuildTool
{
	/// <summary>
	/// Builds a target
	/// </summary>
	[ToolMode("Build", ToolModeOptions.XmlConfig | ToolModeOptions.BuildPlatforms | ToolModeOptions.SingleInstance)]
	class BuildMode : ToolMode
	{
		/// <summary>
		/// Specifies the file to use for logging
		/// </summary>
		[XmlConfigFile(Category = "BuildConfiguration")]
		public string BaseLogFileName = "../Programs/UnrealBuildTool/Log.txt";

		/// <summary>
		/// Whether to skip checking for files identified by the junk manifest
		/// </summary>
		[XmlConfigFile]
		[CommandLine("-IgnoreJunk")]
		public bool bIgnoreJunk = false;

		/// <summary>
		/// Main entry point
		/// </summary>
		/// <param name="Arguments">Command-line arguments</param>
		/// <returns>One of the values of ECompilationResult</returns>
		public override int Execute(CommandLineArguments Arguments)
		{
			Stopwatch BuildTimer = Stopwatch.StartNew();
			Arguments.ApplyTo(this);

			// Initialize the log system, buffering the output until we can create the log file
			StartupTraceListener StartupListener = new StartupTraceListener();
			Trace.Listeners.Add(StartupListener);

			// Write the command line
			Log.TraceLog("Command line: {0}", Environment.CommandLine);

			// Grab the environment.
			UnrealBuildTool.InitialEnvironment = Environment.GetEnvironmentVariables();
			if (UnrealBuildTool.InitialEnvironment.Count < 1)
			{
				throw new BuildException("Environment could not be read");
			}

			// Read the XML configuration files
			XmlConfig.ApplyTo(this);

			// Create the log file, and flush the startup listener to it
			FileReference LogFile = null;
			if(!Arguments.HasOption("-NoLog") && !Log.HasFileWriter())
			{
				LogFile = new FileReference(BaseLogFileName);
				foreach(string LogSuffix in Arguments.GetValues("-LogSuffix="))
				{
					LogFile = LogFile.ChangeExtension(null) + "_" + LogSuffix + LogFile.GetExtension();
				}

				TextWriterTraceListener LogTraceListener = Log.AddFileWriter("DefaultLogTraceListener", LogFile);
				StartupListener.CopyTo(LogTraceListener);
			}
			Trace.Listeners.Remove(StartupListener);

			// Create the build configuration object, and read the settings
			BuildConfiguration BuildConfiguration = new BuildConfiguration();
			XmlConfig.ApplyTo(BuildConfiguration);
			Arguments.ApplyTo(BuildConfiguration);

			// Then let the command lines override any configs necessary.
			if(BuildConfiguration.bXGEExport)
			{
				BuildConfiguration.bAllowXGE = true;
			}

			// Parse the remote INI setting
			string RemoteIniPath;
			Arguments.TryGetValue("-RemoteIni=", out RemoteIniPath);
			UnrealBuildTool.SetRemoteIniPath(RemoteIniPath);

			// now that we know the available platforms, we can delete other platforms' junk. if we're only building specific modules from the editor, don't touch anything else (it may be in use).
			if (!bIgnoreJunk && !UnrealBuildTool.IsEngineInstalled())
			{
				using(Timeline.ScopeEvent("DeleteJunk()"))
				{
					JunkDeleter.DeleteJunk();
				}
			}

			// Parse and build the targets
			try
			{
				// Parse all the target descriptors
				List<TargetDescriptor> TargetDescriptors = new List<TargetDescriptor>();
				using(Timeline.ScopeEvent("TargetDescriptor.ParseCommandLine()"))
				{
					TargetDescriptors.AddRange(TargetDescriptor.ParseCommandLine(Arguments, BuildConfiguration.bUsePrecompiled, BuildConfiguration.bSkipRulesCompile));
				}

				// Handle remote builds
				for(int Idx = 0; Idx < TargetDescriptors.Count; Idx++)
				{
					TargetDescriptor TargetDesc = TargetDescriptors[Idx];
					if(RemoteMac.HandlesTargetPlatform(TargetDesc.Platform))
					{
						FileReference RemoteLogFile = null;
						if(LogFile != null)
						{
							RemoteLogFile = FileReference.Combine(LogFile.Directory, LogFile.GetFileNameWithoutExtension() + "_Remote.txt");
						}

						RemoteMac RemoteMac = new RemoteMac(TargetDesc.ProjectFile);
						if(!RemoteMac.Build(TargetDesc, LogFile))
						{
							return (int)CompilationResult.Unknown;
						}

						TargetDescriptors.RemoveAt(Idx--);
					}
				}

				// Handle local builds
				if(TargetDescriptors.Count > 0)
				{
					// Get a set of all the project directories.
					HashSet<DirectoryReference> ProjectDirs = new HashSet<DirectoryReference>();
					foreach(TargetDescriptor TargetDesc in TargetDescriptors)
					{
						if(TargetDesc.ProjectFile != null)
						{
							ProjectDirs.Add(TargetDesc.ProjectFile.Directory);
						}
					}

					// Create the working set provider
					using (ISourceFileWorkingSet WorkingSet = SourceFileWorkingSet.Create(UnrealBuildTool.RootDirectory, ProjectDirs))
					{
						Build(TargetDescriptors, BuildConfiguration, WorkingSet);
					}
				}
			}
			finally
			{
				// Save all the caches
				SourceFileMetadataCache.SaveAll();
				CppDependencyCache.SaveAll();
			}

			// Figure out how long we took to execute.
			Log.TraceInformation("Total build time: {0:0.00} seconds", BuildTimer.Elapsed.TotalSeconds);
			return 0;
		}

		/// <summary>
		/// Build a list of targets
		/// </summary>
		/// <param name="TargetDescriptors">Target descriptors</param>
		/// <param name="BuildConfiguration">Current build configuration</param>
		/// <param name="WorkingSet">The source file working set</param>
		/// <returns>Result from the compilation</returns>
		public static void Build(List<TargetDescriptor> TargetDescriptors, BuildConfiguration BuildConfiguration, ISourceFileWorkingSet WorkingSet)
		{
			// Create a makefile for each target
			TargetMakefile[] Makefiles = new TargetMakefile[TargetDescriptors.Count];
			for(int TargetIdx = 0; TargetIdx < TargetDescriptors.Count; TargetIdx++)
			{
				Makefiles[TargetIdx] = CreateMakefile(BuildConfiguration, TargetDescriptors[TargetIdx], WorkingSet);
			}

			// Execute the build
			if(!BuildConfiguration.bSkipBuild)
			{
				// Make sure that none of the actions conflict with any other (producing output files differently, etc...)
				ActionGraph.CheckForConflicts(Makefiles.SelectMany(x => x.Actions));
				
				// Find all the actions to be executed
				HashSet<Action>[] ActionsToExecute = new HashSet<Action>[TargetDescriptors.Count];
				for(int TargetIdx = 0; TargetIdx < TargetDescriptors.Count; TargetIdx++)
				{
					ActionsToExecute[TargetIdx] = GetActionsForTarget(BuildConfiguration, TargetDescriptors[TargetIdx], Makefiles[TargetIdx]);
				}

				// If there are multiple targets being built, merge the actions together
				List<Action> MergedActionsToExecute;
				if(TargetDescriptors.Count == 1)
				{
					MergedActionsToExecute = new List<Action>(ActionsToExecute[0]);
				}
				else
				{
					MergedActionsToExecute = MergeActionGraphs(TargetDescriptors, ActionsToExecute);
				}

				// Link all the actions together
				ActionGraph.Link(MergedActionsToExecute);

				// Make sure the appropriate executor is selected
				foreach(TargetDescriptor TargetDescriptor in TargetDescriptors)
				{
					UEBuildPlatform BuildPlatform = UEBuildPlatform.GetBuildPlatform(TargetDescriptor.Platform);
					BuildConfiguration.bAllowXGE &= BuildPlatform.CanUseXGE();
					BuildConfiguration.bAllowDistcc &= BuildPlatform.CanUseDistcc();
					BuildConfiguration.bAllowSNDBS &= BuildPlatform.CanUseSNDBS();
				}

				// Delete produced items that are outdated.
				ActionGraph.DeleteOutdatedProducedItems(MergedActionsToExecute);

				// Save all the action histories now that files have been removed. We have to do this after deleting produced items to ensure that any
				// items created during the build don't have the wrong command line.
				ActionHistory.SaveAll();

				// Create directories for the outdated produced items.
				ActionGraph.CreateDirectoriesForProducedItems(MergedActionsToExecute);

				// Execute the actions
				if (BuildConfiguration.bXGEExport)
				{
					using(Timeline.ScopeEvent("XGE.ExportActions()"))
					{
						XGE.ExportActions(MergedActionsToExecute);
					}
				}
				else
				{
					using(Timeline.ScopeEvent("ActionGraph.ExecuteActions()"))
					{
						ActionGraph.ExecuteActions(BuildConfiguration, MergedActionsToExecute);
					}
				}

				// Run the deployment steps
				if (!BuildConfiguration.bXGEExport)
				{
					foreach(TargetMakefile Makefile in Makefiles)
					{
						if (Makefile.bDeployAfterCompile)
						{
							TargetReceipt Receipt = TargetReceipt.Read(Makefile.ReceiptFile);
							Log.TraceInformation("Deploying {0} {1} {2}...", Receipt.TargetName, Receipt.Platform, Receipt.Configuration);
							UEBuildPlatform.GetBuildPlatform(Receipt.Platform).Deploy(Receipt);
						}
					}
				}
			}
		}

		/// <summary>
		/// Creates the makefile for a target. If an existing, valid makefile already exists on disk, loads that instead.
		/// </summary>
		/// <param name="BuildConfiguration">The build configuration</param>
		/// <param name="TargetDescriptor">Target being built</param>
		/// <param name="WorkingSet">Set of source files which are part of the working set</param>
		/// <returns>Makefile for the given target</returns>
		static TargetMakefile CreateMakefile(BuildConfiguration BuildConfiguration, TargetDescriptor TargetDescriptor, ISourceFileWorkingSet WorkingSet)
		{
			// Get the path to the makefile for this target
			FileReference MakefileLocation = null;
			if(BuildConfiguration.bUseUBTMakefiles && TargetDescriptor.SingleFileToCompile == null)
			{
				MakefileLocation = TargetMakefile.GetLocation(TargetDescriptor.ProjectFile, TargetDescriptor.Name, TargetDescriptor.Platform, TargetDescriptor.Configuration);
			}

			// Try to load an existing makefile
			TargetMakefile Makefile = null;
			if(MakefileLocation != null)
			{
				using(Timeline.ScopeEvent("TargetMakefile.Load()"))
				{
					string ReasonNotLoaded;
					Makefile = TargetMakefile.Load(MakefileLocation, TargetDescriptor.ProjectFile, TargetDescriptor.Platform, WorkingSet, out ReasonNotLoaded);
					if (Makefile == null)
					{
						Log.TraceInformation("Creating makefile for {0} ({1})", TargetDescriptor.Name, ReasonNotLoaded);
					}
				}
			}

			// If we couldn't load a makefile, create a new one
			if(Makefile == null)
			{
				// Create the target
				UEBuildTarget Target;
				using(Timeline.ScopeEvent("UEBuildTarget.Create()"))
				{
					Target = UEBuildTarget.Create(TargetDescriptor, BuildConfiguration.bSkipRulesCompile, BuildConfiguration.bUsePrecompiled);
				}

				// Build the target
				const bool bIsAssemblingBuild = true;

				using(Timeline.ScopeEvent("UEBuildTarget.Build()"))
				{
					Makefile = Target.Build(BuildConfiguration, WorkingSet, bIsAssemblingBuild);
				}

				// Save the environment variables
				foreach (System.Collections.DictionaryEntry EnvironmentVariable in Environment.GetEnvironmentVariables())
				{
					Makefile.EnvironmentVariables.Add(Tuple.Create((string)EnvironmentVariable.Key, (string)EnvironmentVariable.Value));
				}

				// Save the makefile for next time
				if(MakefileLocation != null)
				{
					using(Timeline.ScopeEvent("TargetMakefile.Save()"))
					{
						Makefile.Save(MakefileLocation);
					}
				}
			}
			else
			{
				// Restore the environment variables
				foreach (Tuple<string, string> EnvironmentVariable in Makefile.EnvironmentVariables)
				{
					Environment.SetEnvironmentVariable(EnvironmentVariable.Item1, EnvironmentVariable.Item2);
				}

				// Execute all the pre-build steps
				if (!BuildConfiguration.bXGEExport)
				{
					Utils.ExecuteCustomBuildSteps(Makefile.PreBuildScripts);
				}

				// If the target needs UHT to be run, we'll go ahead and do that now
				if (Makefile.UObjectModules.Count > 0)
				{
					const bool bIsGatheringBuild = false;
					const bool bIsAssemblingBuild = true;

					FileReference ModuleInfoFileName = FileReference.Combine(Makefile.ProjectIntermediateDirectory, TargetDescriptor.Name + ".uhtmanifest");
					ExternalExecution.ExecuteHeaderToolIfNecessary(BuildConfiguration, TargetDescriptor.ProjectFile, TargetDescriptor.Name, Makefile.TargetType, Makefile.bHasProjectScriptPlugin, UObjectModules: Makefile.UObjectModules, ModuleInfoFileName: ModuleInfoFileName, bIsGatheringBuild: bIsGatheringBuild, bIsAssemblingBuild: bIsAssemblingBuild, WorkingSet: WorkingSet);
				}
			}
			return Makefile;
		}

		/// <summary>
		/// Determine what needs to be built for a target
		/// </summary>
		/// <param name="BuildConfiguration">The build configuration</param>
		/// <param name="TargetDescriptor">Target being built</param>
		/// <param name="Makefile">Makefile generated for this target</param>
		/// <returns>Set of actions to execute</returns>
		static HashSet<Action> GetActionsForTarget(BuildConfiguration BuildConfiguration, TargetDescriptor TargetDescriptor, TargetMakefile Makefile)
		{
			// Create the action graph
			ActionGraph.Link(Makefile.Actions);

			// Parse the list of hot-reload module names
			Dictionary<string, int> HotReloadModuleNameToSuffix = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
			foreach(string ModuleWithSuffix in TargetDescriptor.AdditionalArguments.GetValues("-ModuleWithSuffix="))
			{
				int SuffixIdx = ModuleWithSuffix.LastIndexOf(',');
				if(SuffixIdx == -1)
				{
					throw new BuildException("Missing suffix argument from -ModuleWithSuffix=Name,Suffix");
				}

				string ModuleName = ModuleWithSuffix.Substring(0, SuffixIdx);

				int Suffix;
				if(!Int32.TryParse(ModuleWithSuffix.Substring(SuffixIdx + 1), out Suffix))
				{
					throw new BuildException("Suffix for modules must be an integer");
				}

				HotReloadModuleNameToSuffix[ModuleName] = Suffix;
			}

			// Get the hot-reload mode
			HotReloadMode HotReloadMode = HotReloadMode.Disabled;
			if(TargetDescriptor.AdditionalArguments.Any(x => x.Equals("-ForceHotReload", StringComparison.InvariantCultureIgnoreCase)))
			{
				HotReloadMode = HotReloadMode.FromIDE;
			}
			else if (!TargetDescriptor.AdditionalArguments.Any(x => x.Equals("-NoHotReload", StringComparison.InvariantCultureIgnoreCase)))
			{
				if (BuildConfiguration.bAllowHotReloadFromIDE && HotReload.ShouldDoHotReloadFromIDE(BuildConfiguration, TargetDescriptor))
				{
					HotReloadMode = HotReloadMode.FromIDE;
				}
				else if (HotReloadModuleNameToSuffix.Count > 0 && TargetDescriptor.ForeignPlugin == null)
				{
					HotReloadMode = HotReloadMode.FromEditor;
				}
			}

			// Get the output items we're trying to build
			List<FileItem> TargetOutputItems = Makefile.OutputItems;

			// Parse the list of modules to build
			HashSet<string> OnlyModuleNames = new HashSet<string>(TargetDescriptor.AdditionalArguments.GetValues("-Module="));
			if (OnlyModuleNames.Count > 0)
			{
				TargetOutputItems = new List<FileItem>();
				foreach(string OnlyModuleName in OnlyModuleNames)
				{
					FileItem[] OutputItemsForModule;
					if(!Makefile.ModuleNameToOutputItems.TryGetValue(OnlyModuleName, out OutputItemsForModule))
					{
						throw new BuildException("Unable to find output items for module '{0}'", OnlyModuleName);
					}
					TargetOutputItems.AddRange(OutputItemsForModule);
				}
			}

			// If we're just compiling a single file, set the target items to be all the derived items
			if(TargetDescriptor.SingleFileToCompile != null)
			{
				FileItem FileToCompile = FileItem.GetItemByFileReference(TargetDescriptor.SingleFileToCompile);
				TargetOutputItems = Makefile.Actions.Where(x => x.PrerequisiteItems.Contains(FileToCompile)).SelectMany(x => x.ProducedItems).ToList();
			}

			// Get the root prerequisite actions
			List<Action> PrerequisiteActions = ActionGraph.GatherPrerequisiteActions(Makefile.Actions, new HashSet<FileItem>(TargetOutputItems));

			// Get the path to the hot reload state file for this target
			FileReference HotReloadStateFile = global::UnrealBuildTool.HotReloadState.GetLocation(TargetDescriptor.ProjectFile, TargetDescriptor.Name, TargetDescriptor.Platform, TargetDescriptor.Configuration, TargetDescriptor.Architecture);

			// Apply the previous hot reload state
			HotReloadState HotReloadState = null;
			if(HotReloadMode == HotReloadMode.Disabled)
			{
				// Delete the previous state file
				HotReload.DeleteTemporaryFiles(HotReloadStateFile);
			}
			else
			{
				// Read the previous state file and apply it to the action graph
				if(FileReference.Exists(HotReloadStateFile))
				{
					HotReloadState = HotReloadState.Load(HotReloadStateFile);
				}
				else
				{
					HotReloadState = new HotReloadState();
				}

				// Update the action graph to produce these new files
				HotReload.PatchActionGraph(PrerequisiteActions, HotReloadState.OriginalFileToHotReloadFile);

				// Update the module to output file mapping
				foreach(string HotReloadModuleName in Makefile.HotReloadModuleNames)
				{
					FileItem[] ModuleOutputItems = Makefile.ModuleNameToOutputItems[HotReloadModuleName];
					for(int Idx = 0; Idx < ModuleOutputItems.Length; Idx++)
					{
						FileReference NewLocation;
						if(HotReloadState.OriginalFileToHotReloadFile.TryGetValue(ModuleOutputItems[Idx].Location, out NewLocation))
						{
							ModuleOutputItems[Idx] = FileItem.GetItemByFileReference(NewLocation);
						}
					}
				}

				// If we want a specific suffix on any modules, apply that now. We'll track the outputs later, but the suffix has to be forced  (and is always out of date if it doesn't exist).
				if(HotReloadModuleNameToSuffix.Count > 0)
				{
					Dictionary<FileReference, FileReference> OldLocationToNewLocation = new Dictionary<FileReference, FileReference>();
					foreach(string HotReloadModuleName in Makefile.HotReloadModuleNames)
					{
						int ModuleSuffix;
						if(HotReloadModuleNameToSuffix.TryGetValue(HotReloadModuleName, out ModuleSuffix))
						{
							FileItem[] ModuleOutputItems = Makefile.ModuleNameToOutputItems[HotReloadModuleName];
							foreach(FileItem ModuleOutputItem in ModuleOutputItems)
							{
								FileReference OldLocation = ModuleOutputItem.Location;
								FileReference NewLocation = HotReload.ReplaceSuffix(OldLocation, ModuleSuffix);
								OldLocationToNewLocation[OldLocation] = NewLocation;
							}
						}
					}
					HotReload.PatchActionGraph(PrerequisiteActions, OldLocationToNewLocation);
				}

				// Re-link the action graph
				ActionGraph.Link(PrerequisiteActions);
			}

			// Create the dependencies cache
			CppDependencyCache CppDependencies;
			using(Timeline.ScopeEvent("Reading dependency cache"))
			{
				CppDependencies = CppDependencyCache.CreateHierarchy(TargetDescriptor.ProjectFile, TargetDescriptor.Name, TargetDescriptor.Platform, TargetDescriptor.Configuration, Makefile.TargetType);
			}

			// Create the action history
			ActionHistory History;
			using(Timeline.ScopeEvent("Reading action history"))
			{
				History = ActionHistory.CreateHierarchy(TargetDescriptor.ProjectFile, TargetDescriptor.Name, TargetDescriptor.Platform, Makefile.TargetType);
			}

			// Plan the actions to execute for the build.
			HashSet<Action> TargetActionsToExecute;
			if (TargetDescriptor.SingleFileToCompile == null)
			{
				TargetActionsToExecute = ActionGraph.GetActionsToExecute(Makefile.Actions, PrerequisiteActions, CppDependencies, History, BuildConfiguration.bIgnoreOutdatedImportLibraries);
			}
			else
			{
				TargetActionsToExecute = new HashSet<Action>(PrerequisiteActions);
			}
			
			// Patch action history for hot reload when running in assembler mode.  In assembler mode, the suffix on the output file will be
			// the same for every invocation on that makefile, but we need a new suffix each time.
			if (HotReloadMode != HotReloadMode.Disabled)
			{
				// For all the hot-reloadable modules that may need a unique suffix appended, build a mapping from output item to all the output items in that module. We can't 
				// apply a suffix to one without applying a suffix to all of them.
				Dictionary<FileItem, FileItem[]> HotReloadItemToDependentItems = new Dictionary<FileItem, FileItem[]>();
				foreach(string HotReloadModuleName in Makefile.HotReloadModuleNames)
				{
					int ModuleSuffix;
					if(!HotReloadModuleNameToSuffix.TryGetValue(HotReloadModuleName, out ModuleSuffix) || ModuleSuffix == -1)
					{
						FileItem[] ModuleOutputItems;
						if(Makefile.ModuleNameToOutputItems.TryGetValue(HotReloadModuleName, out ModuleOutputItems))
						{
							foreach(FileItem ModuleOutputItem in ModuleOutputItems)
							{
								HotReloadItemToDependentItems[ModuleOutputItem] = ModuleOutputItems;
							}
						}
					} 
				}

				// Expand the list of actions to execute to include everything that references any files with a new suffix. Unlike a regular build, we can't ignore
				// dependencies on import libraries under the assumption that a header would change if the API changes, because the dependency will be on a different DLL.
				HashSet<FileItem> FilesRequiringSuffix = new HashSet<FileItem>(TargetActionsToExecute.SelectMany(x => x.ProducedItems).Where(x => HotReloadItemToDependentItems.ContainsKey(x)));
				for(int LastNumFilesWithNewSuffix = 0; FilesRequiringSuffix.Count > LastNumFilesWithNewSuffix;)
				{
					LastNumFilesWithNewSuffix = FilesRequiringSuffix.Count;
					foreach(Action PrerequisiteAction in PrerequisiteActions)
					{
						if(!TargetActionsToExecute.Contains(PrerequisiteAction))
						{
							foreach(FileItem ProducedItem in PrerequisiteAction.ProducedItems)
							{
								FileItem[] DependentItems;
								if(HotReloadItemToDependentItems.TryGetValue(ProducedItem, out DependentItems))
								{
									TargetActionsToExecute.Add(PrerequisiteAction);
									FilesRequiringSuffix.UnionWith(DependentItems);
								}
							}
						}
					}
				}

				// Build a list of file mappings
				Dictionary<FileReference, FileReference> OldLocationToNewLocation = new Dictionary<FileReference, FileReference>();
				foreach(FileItem FileRequiringSuffix in FilesRequiringSuffix)
				{
					FileReference OldLocation = FileRequiringSuffix.Location;
					FileReference NewLocation = HotReload.ReplaceSuffix(OldLocation, HotReloadState.NextSuffix);
					OldLocationToNewLocation[OldLocation] = NewLocation;
				}

				// Update the action graph with these new paths
				HotReload.PatchActionGraph(PrerequisiteActions, OldLocationToNewLocation);

				// Get a new list of actions to execute now that the graph has been modified
				TargetActionsToExecute = ActionGraph.GetActionsToExecute(Makefile.Actions, PrerequisiteActions, CppDependencies, History, BuildConfiguration.bIgnoreOutdatedImportLibraries);

				// Build a mapping of all file items to their original
				Dictionary<FileReference, FileReference> HotReloadFileToOriginalFile = new Dictionary<FileReference, FileReference>();
				foreach(KeyValuePair<FileReference, FileReference> Pair in HotReloadState.OriginalFileToHotReloadFile)
				{
					HotReloadFileToOriginalFile[Pair.Value] = Pair.Key;
				}
				foreach(KeyValuePair<FileReference, FileReference> Pair in OldLocationToNewLocation)
				{
					FileReference OriginalLocation;
					if(!HotReloadFileToOriginalFile.TryGetValue(Pair.Key, out OriginalLocation))
					{
						OriginalLocation = Pair.Key;
					}
					HotReloadFileToOriginalFile[Pair.Value] = OriginalLocation;
				}

				// Now filter out all the hot reload files and update the state
				foreach(Action Action in TargetActionsToExecute)
				{
					foreach(FileItem ProducedItem in Action.ProducedItems)
					{
						FileReference OriginalLocation;
						if(HotReloadFileToOriginalFile.TryGetValue(ProducedItem.Location, out OriginalLocation))
						{
							HotReloadState.OriginalFileToHotReloadFile[OriginalLocation] = ProducedItem.Location;
							HotReloadState.TemporaryFiles.Add(ProducedItem.Location);
						}
					}
				}

				// Increment the suffix for the next iteration
				if(TargetActionsToExecute.Count > 0)
				{
					HotReloadState.NextSuffix++;
				}

				// Save the new state
				HotReloadState.Save(HotReloadStateFile);

				// Prevent this target from deploying
				Makefile.bDeployAfterCompile = false;
			}

			return TargetActionsToExecute;
		}

		/// <summary>
		/// Merge action graphs for multiple targets into a single set of actions. Sets group names on merged actions to indicate which target they belong to.
		/// </summary>
		/// <param name="TargetDescriptors">List of target descriptors</param>
		/// <param name="ActionsToExecute">Set of actions to execute for each target</param>
		/// <returns>List of merged actions</returns>
		static List<Action> MergeActionGraphs(List<TargetDescriptor> TargetDescriptors, HashSet<Action>[] ActionsToExecute)
		{
			// Set of all output items. Knowing that there are no conflicts in produced items, we use this to eliminate duplicate actions.
			Dictionary<FileItem, Action> OutputItemToProducingAction = new Dictionary<FileItem, Action>();
			for(int TargetIdx = 0; TargetIdx < TargetDescriptors.Count; TargetIdx++)
			{
				string GroupPrefix = String.Format("{0}-{1}-{2}", TargetDescriptors[TargetIdx].Name, TargetDescriptors[TargetIdx].Platform, TargetDescriptors[TargetIdx].Configuration);
				foreach(Action Action in ActionsToExecute[TargetIdx])
				{
					Action ExistingAction;
					if(!OutputItemToProducingAction.TryGetValue(Action.ProducedItems[0], out ExistingAction))
					{
						OutputItemToProducingAction[Action.ProducedItems[0]] = Action;
						ExistingAction = Action;
					}
					ExistingAction.GroupNames.Add(GroupPrefix);
				}
			}
			return new List<Action>(OutputItemToProducingAction.Values);
		}
	}
}

