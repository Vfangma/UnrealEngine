// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "DesktopPlatformPrivatePCH.h"
#include "DesktopPlatformBase.h"
#include "UProjectInfo.h"

FString FDesktopPlatformBase::GetCurrentEngineIdentifier()
{
	FString Identifier;
	return GetEngineIdentifierFromRootDir(FPlatformMisc::RootDir(), Identifier) ? Identifier : TEXT("");
}

void FDesktopPlatformBase::EnumerateLauncherEngineInstallations(TMap<FString, FString> &OutInstallations)
{
	// Just enumerate the known manifests for the time being 
	CheckForLauncherEngineInstallation(TEXT("40003"), TEXT("4.0"), OutInstallations);
	CheckForLauncherEngineInstallation(TEXT("1040003"), TEXT("4.1"), OutInstallations);
}

bool FDesktopPlatformBase::GetEngineRootDirFromIdentifier(const FString &Identifier, FString &OutRootDir)
{
	// Get all the installations
	TMap<FString, FString> Installations;
	EnumerateEngineInstallations(Installations);

	// Find the one with the right identifier
	for (TMap<FString, FString>::TConstIterator Iter(Installations); Iter; ++Iter)
	{
		if (Iter->Key == Identifier)
		{
			OutRootDir = Iter->Value;
			return true;
		}
	}
	return false;
}

bool FDesktopPlatformBase::GetEngineIdentifierFromRootDir(const FString &RootDir, FString &OutIdentifier)
{
	// Get all the installations
	TMap<FString, FString> Installations;
	EnumerateEngineInstallations(Installations);

	// Normalize the root directory
	FString NormalizedRootDir = RootDir;
	FPaths::NormalizeDirectoryName(NormalizedRootDir);

	// Find the label for the given directory
	for (TMap<FString, FString>::TConstIterator Iter(Installations); Iter; ++Iter)
	{
		if (Iter->Value == NormalizedRootDir)
		{
			OutIdentifier = Iter->Key;
			return true;
		}
	}

	// Otherwise just try to add it
	return RegisterEngineInstallation(RootDir, OutIdentifier);
}

bool FDesktopPlatformBase::GetDefaultEngineIdentifier(FString &OutId)
{
	TMap<FString, FString> Installations;
	EnumerateEngineInstallations(Installations);

	bool bRes = false;
	if (Installations.Num() > 0)
	{
		// Default to the first install
		TMap<FString, FString>::TConstIterator Iter(Installations);
		OutId = Iter.Key();
		++Iter;

		// Try to find the most preferred install
		for(; Iter; ++Iter)
		{
			if(IsPreferredEngineIdentifier(Iter.Key(), OutId))
			{
				OutId = Iter.Key();
			}
		}
	}
	return bRes;
}

bool FDesktopPlatformBase::GetDefaultEngineRootDir(FString &OutDirName)
{
	FString Identifier;
	return GetDefaultEngineIdentifier(Identifier) && GetEngineRootDirFromIdentifier(Identifier, OutDirName);
}

bool FDesktopPlatformBase::IsPreferredEngineIdentifier(const FString &Identifier, const FString &OtherIdentifier)
{
	int32 Version = ParseReleaseVersion(Identifier);
	int32 OtherVersion = ParseReleaseVersion(OtherIdentifier);

	if(Version != OtherVersion)
	{
		return Version > OtherVersion;
	}
	else
	{
		return Identifier > OtherIdentifier;
	}
}

bool FDesktopPlatformBase::IsSourceDistribution(const FString &EngineRootDir)
{
	// Check for the existence of a SourceBuild.txt file
	FString SourceBuildPath = EngineRootDir / TEXT("Engine/Build/SourceDistribution.txt");
	return (IFileManager::Get().FileSize(*SourceBuildPath) >= 0);
}

bool FDesktopPlatformBase::IsPerforceBuild(const FString &EngineRootDir)
{
	// Check for the existence of a SourceBuild.txt file
	FString PerforceBuildPath = EngineRootDir / TEXT("Engine/Build/PerforceBuild.txt");
	return (IFileManager::Get().FileSize(*PerforceBuildPath) >= 0);
}

bool FDesktopPlatformBase::IsValidRootDirectory(const FString &RootDir)
{
	FString EngineBinariesDirName = RootDir / TEXT("Engine/Binaries");
	FPaths::NormalizeDirectoryName(EngineBinariesDirName);
	return IFileManager::Get().DirectoryExists(*EngineBinariesDirName);
}

bool FDesktopPlatformBase::SetEngineIdentifierForProject(const FString &ProjectFileName, const FString &InIdentifier)
{
	// Load the project file
	TSharedPtr<FJsonObject> ProjectFile = LoadProjectFile(ProjectFileName);
	if (!ProjectFile.IsValid())
	{
		return false;
	}

	// Check if the project is a non-foreign project of the given engine installation. If so, blank the identifier 
	// string to allow portability between source control databases. GetEngineIdentifierForProject will translate
	// the association back into a local identifier on other machines or syncs.
	FString Identifier = InIdentifier;
	if(Identifier.Len() > 0)
	{
		FString RootDir;
		if(GetEngineRootDirFromIdentifier(Identifier, RootDir))
		{
			FUProjectDictionary Dictionary(RootDir);
			if(!Dictionary.IsForeignProject(ProjectFileName))
			{
				Identifier.Empty();
			}
		}
	}

	// Set the association on the project and save it
	ProjectFile->SetStringField(TEXT("EngineAssociation"), Identifier);
	return SaveProjectFile(ProjectFileName, ProjectFile);
}

bool FDesktopPlatformBase::GetEngineIdentifierForProject(const FString &ProjectFileName, FString &OutIdentifier)
{
	// Load the project file
	TSharedPtr<FJsonObject> ProjectFile = LoadProjectFile(ProjectFileName);
	if(!ProjectFile.IsValid())
	{
		return false;
	}

	// Read the identifier from it
	OutIdentifier = ProjectFile->GetStringField(TEXT("EngineAssociation"));
	if(OutIdentifier.Len() > 0)
	{
		return true;
	}

	// Otherwise scan up through the directory hierarchy to find an installation which references it through .uprojectdirs
	FString ParentDir = FPaths::GetPath(ProjectFileName);
	FPaths::NormalizeDirectoryName(ParentDir);

	int32 SeparatorIdx;
	while(ParentDir.FindLastChar(TEXT('/'), SeparatorIdx))
	{
		ParentDir.RemoveAt(SeparatorIdx, ParentDir.Len() - SeparatorIdx);

		FUProjectDictionary Dictionary(ParentDir);
		if(!Dictionary.IsForeignProject(ProjectFileName) && GetEngineIdentifierFromRootDir(ParentDir, OutIdentifier))
		{
			return true;
		}
	}

	return false;
}

void FDesktopPlatformBase::CheckForLauncherEngineInstallation(const FString &AppId, const FString &Identifier, TMap<FString, FString> &OutInstallations)
{
	FString ManifestText;
	FString ManifestFileName = FString(FPlatformProcess::ApplicationSettingsDir()) / FString::Printf(TEXT("UnrealEngineLauncher/Data/Manifests/%s.manifest"), *AppId);
	if (FFileHelper::LoadFileToString(ManifestText, *ManifestFileName))
	{
		TSharedPtr< FJsonObject > RootObject;
		TSharedRef< TJsonReader<> > Reader = TJsonReaderFactory<>::Create(ManifestText);
		if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
		{
			TSharedPtr<FJsonObject> CustomFieldsObject = RootObject->GetObjectField(TEXT("CustomFields"));
			if (CustomFieldsObject.IsValid())
			{
				FString InstallLocation = CustomFieldsObject->GetStringField("InstallLocation");
				if (InstallLocation.Len() > 0)
				{
					OutInstallations.Add(Identifier, InstallLocation);
				}
			}
		}
	}
}

int32 FDesktopPlatformBase::ParseReleaseVersion(const FString &Version)
{
	TCHAR *End;

	uint64 Major = FCString::Strtoui64(*Version, &End, 10);
	if (Major >= MAX_int16 || *(End++) != '.')
	{
		return INDEX_NONE;
	}

	uint64 Minor = FCString::Strtoui64(End, &End, 10);
	if (Minor >= MAX_int16 || *End != 0)
	{
		return INDEX_NONE;
	}

	return (Major << 16) + Minor;
}

TSharedPtr<FJsonObject> FDesktopPlatformBase::LoadProjectFile(const FString &FileName)
{
	FString FileContents;

	if (!FFileHelper::LoadFileToString(FileContents, *FileName))
	{
		return TSharedPtr<FJsonObject>(NULL);
	}

	TSharedPtr< FJsonObject > JsonObject;
	TSharedRef< TJsonReader<> > Reader = TJsonReaderFactory<>::Create(FileContents);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return TSharedPtr<FJsonObject>(NULL);
	}

	return JsonObject;
}

bool FDesktopPlatformBase::SaveProjectFile(const FString &FileName, TSharedPtr<FJsonObject> Object)
{
	FString FileContents;

	TSharedRef< TJsonWriter<> > Writer = TJsonWriterFactory<>::Create(&FileContents);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return false;
	}

	if (!FFileHelper::SaveStringToFile(FileContents, *FileName))
	{
		return false;
	}

	return true;
}
