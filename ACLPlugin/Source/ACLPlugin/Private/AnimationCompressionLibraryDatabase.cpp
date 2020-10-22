// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AnimationCompressionLibraryDatabase.h"
#include "AnimBoneCompressionCodec_ACLDatabase.h"

#if WITH_EDITORONLY_DATA
#include "ACLImpl.h"

#include <acl/compression/compress.h>
#endif	// WITH_EDITORONLY_DATA

UAnimationCompressionLibraryDatabase::UAnimationCompressionLibraryDatabase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITORONLY_DATA
void UAnimationCompressionLibraryDatabase::PreSave(const ITargetPlatform* TargetPlatform)
{
	Super::PreSave(TargetPlatform);

	if (TargetPlatform != nullptr && TargetPlatform->RequiresCookedData())
	{
		// We are cooking, iterate over every animation sequence that references this database
		// and merge them together into our final database instance. Note that the mapping could
		// be stale and we must double check.

		// TODO???
		// Note that this modifies the compressed data of each anim sequence.
		// TODO???

		TArray<acl::database_merge_mapping> ACLDatabaseMappings;
		for (UAnimSequence* AnimSeq : AnimSequences)
		{
			UAnimBoneCompressionCodec_ACLDatabase* DatabaseCodec = Cast<UAnimBoneCompressionCodec_ACLDatabase>(AnimSeq->CompressedData.BoneCompressionCodec);
			if (DatabaseCodec == nullptr || DatabaseCodec->DatabaseAsset != this)
			{
				UE_LOG(LogAnimationCompression, Warning, TEXT("ACL Database mapping is stale. [%s] no longer references it."), *AnimSeq->GetPathName());
				continue;
			}

			FACLDatabaseCompressedAnimData& AnimData = static_cast<FACLDatabaseCompressedAnimData&>(*AnimSeq->CompressedData.CompressedDataStructure);

			acl::database_merge_mapping ACLMapping;
			ACLMapping.tracks = acl::make_compressed_tracks(AnimData.CompressedByteStream.GetData());
			ACLMapping.database = AnimData.GetCompressedDatabase();

			ACLDatabaseMappings.Add(ACLMapping);
		}

		acl::compressed_database* MergedDB = nullptr;
		const acl::error_result MergeResult = acl::merge_compressed_databases(ACLAllocatorImpl, ACLDatabaseMappings.GetData(), ACLDatabaseMappings.Num(), MergedDB);

		if (!MergeResult.empty())
		{
			UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed to merge databases: %s"), ANSI_TO_TCHAR(MergeResult.c_str()));
			return;
		}

		checkSlow(MergedDB->is_valid(true).empty());

		const uint32 CompressedDatabaseSize = MergedDB->get_size();

		// Copy our database
		CompressedDatabaseBytes.Empty(CompressedDatabaseSize);
		CompressedDatabaseBytes.AddUninitialized(CompressedDatabaseSize);
		FMemory::Memcpy(CompressedDatabaseBytes.GetData(), MergedDB, CompressedDatabaseSize);

		// Free the instance we no longer need
		ACLAllocatorImpl.deallocate(MergedDB, CompressedDatabaseSize);
	}
}
#endif

void UAnimationCompressionLibraryDatabase::PostLoad()
{
	Super::PostLoad();

	if (CompressedDatabaseBytes.Num() != 0)
	{
		const acl::compressed_database* CompressedDatabase = acl::make_compressed_database(CompressedDatabaseBytes.GetData());

		DatabaseStreamer = MakeUnique<NullDatabaseStreamer>(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

		const bool ContextInitResult = DatabaseContext.initialize(ACLAllocatorImpl, *CompressedDatabase, *DatabaseStreamer);
		checkf(ContextInitResult, TEXT("ACL failed to initialize the database context"));

		// Stream everything right away, we live in memory for now
		DatabaseContext.stream_in();
	}
}
