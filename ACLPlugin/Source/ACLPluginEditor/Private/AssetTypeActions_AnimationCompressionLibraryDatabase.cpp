// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AssetTypeActions_AnimationCompressionLibraryDatabase.h"
#include "AnimBoneCompressionCodec_ACLDatabase.h"

#include "Animation/AnimBoneCompressionSettings.h"
#include "EditorStyleSet.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "UObject/UObjectIterator.h"

void FAssetTypeActions_AnimationCompressionLibraryDatabase::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor)
{
	TSharedRef<FSimpleAssetEditor> AssetEditor = FSimpleAssetEditor::CreateEditor(EToolkitMode::Standalone, EditWithinLevelEditor, InObjects);

	auto DatabaseAssets = GetTypedWeakObjectPtrs<UAnimationCompressionLibraryDatabase>(InObjects);
	if (DatabaseAssets.Num() == 1)
	{
		TSharedPtr<class FUICommandList> PluginCommands = MakeShareable(new FUICommandList);
		TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
		ToolbarExtender->AddToolBarExtension("Asset", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FAssetTypeActions_AnimationCompressionLibraryDatabase::AddToolbarExtension, DatabaseAssets[0]));
		AssetEditor->AddToolbarExtender(ToolbarExtender);

		AssetEditor->RegenerateMenusAndToolbars();
	}
}

void FAssetTypeActions_AnimationCompressionLibraryDatabase::AddToolbarExtension(FToolBarBuilder& Builder, TWeakObjectPtr<UAnimationCompressionLibraryDatabase> DatabasePtr)
{
	Builder.BeginSection("Build");
	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_AnimationCompressionLibraryDatabase::ExecuteBuild, DatabasePtr)
		),
		NAME_None,
		FText::FromString(TEXT("Build")),
		FText::FromString(TEXT("Builds the database from all the animation sequences that reference this database through their codec.")),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "Persona.ApplyCompression")
	);
	Builder.EndSection();
}

void FAssetTypeActions_AnimationCompressionLibraryDatabase::GetActions(const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder)
{
	auto DatabaseAssets = GetTypedWeakObjectPtrs<UAnimationCompressionLibraryDatabase>(InObjects);

	if (DatabaseAssets.Num() != 1)
	{
		return;
	}

	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Build")),
		FText::FromString(TEXT("Builds the database from all the animation sequences that reference this database through their codec.")),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "Persona.ApplyCompression.Small"),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_AnimationCompressionLibraryDatabase::ExecuteBuild, DatabaseAssets[0])
		)
	);
}

void FAssetTypeActions_AnimationCompressionLibraryDatabase::ExecuteBuild(TWeakObjectPtr<UAnimationCompressionLibraryDatabase> DatabasePtr)
{
	if (!DatabasePtr.IsValid())
	{
		return;
	}

	UAnimationCompressionLibraryDatabase* Database = DatabasePtr.Get();

	TArray<UAnimSequence*> AnimSequences;
	for (TObjectIterator<UAnimSequence> It; It; ++It)
	{
		UAnimSequence* AnimSeq = *It;
		if (AnimSeq->GetOutermost() == GetTransientPackage())
		{
			continue;
		}

		UAnimBoneCompressionSettings* Settings = AnimSeq->BoneCompressionSettings;
		if (Settings == nullptr)
		{
			continue;
		}

		// TODO: Warn if more than one codec present in the settings asset

		for (UAnimBoneCompressionCodec* Codec : Settings->Codecs)
		{
			UAnimBoneCompressionCodec_ACLDatabase* DatabaseCodec = Cast<UAnimBoneCompressionCodec_ACLDatabase>(Codec);
			if (DatabaseCodec != nullptr && DatabaseCodec->DatabaseAsset == Database)
			{
				AnimSequences.Add(AnimSeq);
			}
		}
	}

	if (AnimSequences.Num() == 0)
	{
		return;
	}

	// Sort our anim sequences by path name to ensure predictable results
	struct FCompareObjectNames
	{
		FORCEINLINE bool operator()(const UAnimSequence& Lhs, const UAnimSequence& Rhs) const
		{
			return Lhs.GetPathName().Compare(Rhs.GetPathName()) < 0;
		}
	};
	AnimSequences.Sort(FCompareObjectNames());

	// Check if the list is any different to avoir marking as dirty if we aren't
	const int32 NumSeqs = AnimSequences.Num();
	bool bIsDirty = false;
	if (Database->AnimSequences.Num() == NumSeqs)
	{
		for (int32 SeqIdx = 0; SeqIdx < NumSeqs; ++SeqIdx)
		{
			if (Database->AnimSequences[SeqIdx] != AnimSequences[SeqIdx])
			{
				// Sorted arrays do not match, we are dirty
				bIsDirty = true;
				break;
			}
		}
	}
	else
	{
		// Count differs, dirty for sure
		bIsDirty = true;
	}

	if (bIsDirty)
	{
		// Swap our content since we are new
		Swap(Database->AnimSequences, AnimSequences);

		Database->MarkPackageDirty();
	}
}
