#pragma once

// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include <acl/database/database.h>
#include <acl/database/idatabase_streamer.h>

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AnimationCompressionLibraryDatabase.generated.h"

using UE4DefaultDatabaseSettings = acl::default_database_settings;

/** An enum to represent the ACL database streaming preview state. */
UENUM()
enum class ACLDBPreviewState : uint8
{
	None UMETA(DisplayName = "No Preview"),
	HighQuality UMETA(DisplayName = "High Quality"),
	//MediumQuality UMETA(DisplayName = "Medium Quality"),
	LowQuality UMETA(DisplayName = "Low Quality"),
};

/** An ACL database object references several UAnimSequence instances that it contains. */
UCLASS(MinimalAPI, config = Engine, meta = (DisplayName = "ACL Database"))
class UAnimationCompressionLibraryDatabase : public UObject
{
	GENERATED_UCLASS_BODY()

	/** The raw binary data for our compressed database and anim sequences. Present only in cooked builds. */
	UPROPERTY()
	TArray<uint8> CompressedBytes;

	/** Stores a mapping for each anim sequence, where its compresssed data lives in our compressed buffer. Each 64 bit value is split into 32 bits: (Hash << 32) | Offset. Present only in cooked builds. */
	UPROPERTY()
	TArray<uint64> CookedAnimSequenceMappings;

	/** Bulk data that we'll stream. Present only in cooked builds. */
	FByteBulkData StreamableBulkData;

	/** The database decompression context object. Bound to the compressed database instance. */
	acl::database_context<UE4DefaultDatabaseSettings> DatabaseContext;

	/** The streamer instance used by the database context. */
	TUniquePtr<acl::idatabase_streamer> DatabaseStreamer;

#if WITH_EDITORONLY_DATA
	/** The database streaming state to use when decompressing and preview is enabled. */
	UPROPERTY(EditAnywhere, Transient, Category = "ACL Debug Options")
	ACLDBPreviewState PreviewState;

	/** The anim sequences contained within the database. Built manually from the asset UI, content browser, or with a commandlet. */
	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	TArray<class UAnimSequence*> AnimSequences;

	//////////////////////////////////////////////////////////////////////////
	// UObject implementation
	virtual void PreSave(const class ITargetPlatform* TargetPlatform) override;
#endif

	// UObject implementation
	virtual void BeginDestroy() override;
	virtual void PostLoad() override;
	virtual void Serialize(FArchive& Ar) override;

	// Initiate a database stream in request
	UFUNCTION(BlueprintCallable, Category = "Animation|ACL", meta = (DisplayName = "Stream Database In"))
	static void StreamDatabaseIn(UAnimationCompressionLibraryDatabase* DatabaseAsset);

	// Initiate a database stream out request
	UFUNCTION(BlueprintCallable, Category = "Animation|ACL", meta = (DisplayName = "Stream Database Out"))
	static void StreamDatabaseOut(UAnimationCompressionLibraryDatabase* DatabaseAsset);
};
