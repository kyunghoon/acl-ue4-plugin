#pragma once

// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include <acl/database/database.h>
#include <acl/database/idatabase_streamer.h>

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AnimationCompressionLibraryDatabase.generated.h"

using UE4DefaultDatabaseSettings = acl::default_database_settings;

/** An ACL database object references several UAnimSequence instances that it contains. */
UCLASS(MinimalAPI, config = Engine, meta = (DisplayName = "ACL Database"))
class UAnimationCompressionLibraryDatabase : public UObject
{
	GENERATED_UCLASS_BODY()

	/** The raw binary data for our compressed database and anim sequences. Present only in cooked builds. */
	UPROPERTY()
	TArray<uint8> CompressedBytes;

	/** Stores a mapping for each anim sequence, where its compresssed data lives in our compressed buffer. Present only in cooked builds. */
	UPROPERTY()
	TArray<uint32> CookedAnimSequenceNames;

	/** Stores a mapping for each anim sequence, where its compresssed data lives in our compressed buffer. Present only in cooked builds. */
	UPROPERTY()
	TArray<uint32> CookedAnimSequenceOffsets;

	/** The database decompression context object. Bound to the compressed database instance. */
	acl::database_context<UE4DefaultDatabaseSettings> DatabaseContext;

	/** The streamer instance used by the database context. */
	TUniquePtr<acl::idatabase_streamer> DatabaseStreamer;

#if WITH_EDITORONLY_DATA
	/** The anim sequences contained within the database. Built manually from the asset UI, content browser, or with a commandlet. */
	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	TArray<class UAnimSequence*> AnimSequences;

	//////////////////////////////////////////////////////////////////////////
	// UObject implementation
	virtual void PreSave(const class ITargetPlatform* TargetPlatform) override;
#endif

	// UObject implementation
	virtual void PostLoad() override;
};
