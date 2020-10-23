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

		// Gather the sequences we need to cook, these are already sorted by FName by construction
		TArray<UAnimSequence*> CookedSequences;
		CookedSequences.Empty(AnimSequences.Num());

		for (UAnimSequence* AnimSeq : AnimSequences)
		{
			UAnimBoneCompressionCodec_ACLDatabase* DatabaseCodec = Cast<UAnimBoneCompressionCodec_ACLDatabase>(AnimSeq->CompressedData.BoneCompressionCodec);
			if (DatabaseCodec == nullptr || DatabaseCodec->DatabaseAsset != this)
			{
				UE_LOG(LogAnimationCompression, Warning, TEXT("ACL Database mapping is stale. [%s] no longer references it."), *AnimSeq->GetPathName());
				continue;
			}

			CookedSequences.Add(AnimSeq);
		}

		if (CookedSequences.Num() == 0)
		{
			return;	// Nothing to cook
		}

		TArray<acl::database_merge_mapping> ACLDatabaseMappings;
		for (UAnimSequence* AnimSeq : CookedSequences)
		{
			FACLDatabaseCompressedAnimData& AnimData = static_cast<FACLDatabaseCompressedAnimData&>(*AnimSeq->CompressedData.CompressedDataStructure);

			// Duplicate the compressed clip since we'll modify it
			const int32 CompressedSize = AnimData.CompressedClip.Num();
			uint8* CompressedTracks = reinterpret_cast<uint8*>(GMalloc->Malloc(CompressedSize, 16));
			FMemory::Memcpy(CompressedTracks, AnimData.CompressedClip.GetData(), CompressedSize);

			acl::database_merge_mapping ACLMapping;
			ACLMapping.tracks = acl::make_compressed_tracks(CompressedTracks);
			ACLMapping.database = AnimData.GetCompressedDatabase();

			check(ACLMapping.tracks != nullptr);

			ACLDatabaseMappings.Add(ACLMapping);
		}

		acl::compressed_database* MergedDB = nullptr;
		const acl::error_result MergeResult = acl::merge_compressed_databases(ACLAllocatorImpl, ACLDatabaseMappings.GetData(), ACLDatabaseMappings.Num(), MergedDB);

		if (!MergeResult.empty())
		{
			// Free our duplicate compressed clips
			for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
			{
				GMalloc->Free(Mapping.tracks);
			}

			UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed to merge databases: %s"), ANSI_TO_TCHAR(MergeResult.c_str()));
			return;
		}

		checkSlow(MergedDB->is_valid(true).empty());

		const uint32 CompressedDatabaseSize = MergedDB->get_size();

		// Our compressed sequences follow the database in memory, aligned to 16 bytes
		uint32 CompressedSequenceOffset = acl::align_to(CompressedDatabaseSize, 16);

		// Write our our cooked offset mappings
		// We use two arrays for simplicity. UE4 doesn't support serializing a TMap or TSortedMap and so instead
		// we store an array of sorted FNames hashes, and another array of offsets. We'll use binary search to find our
		// index at runtime in O(logN) in the sorted names array, and read the offset we need in the other.
		// TODO: Use uint64 for both merged in together
		const int32 NumMappings = ACLDatabaseMappings.Num();
		CookedAnimSequenceNames.Empty(NumMappings);
		CookedAnimSequenceOffsets.Empty(NumMappings);
		for (int32 MappingIndex = 0; MappingIndex < NumMappings; ++MappingIndex)
		{
			UAnimSequence* AnimSeq = CookedSequences[MappingIndex];
			const acl::database_merge_mapping& Mapping = ACLDatabaseMappings[MappingIndex];

			// Align our sequence to 16 bytes
			CompressedSequenceOffset = acl::align_to(CompressedSequenceOffset, 16);

			// Add our mapping
			CookedAnimSequenceNames.Add(GetTypeHash(AnimSeq->GetFName()));
			CookedAnimSequenceOffsets.Add(CompressedSequenceOffset);

			// Increment our offset but don't align since we don't want to add unnecesary padding at the end of the last sequence
			CompressedSequenceOffset += Mapping.tracks->get_size();
		}

		// Our full buffer size is our resulting offset
		const uint32 CompressedBytesSize = CompressedSequenceOffset;

		// Copy our database
		CompressedBytes.Empty(CompressedBytesSize);
		CompressedBytes.AddUninitialized(CompressedBytesSize);
		FMemory::Memcpy(CompressedBytes.GetData(), MergedDB, CompressedDatabaseSize);

		// Copy our compressed clips
		CompressedSequenceOffset = acl::align_to(CompressedDatabaseSize, 16);	// Reset

		for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
		{
			// Align our sequence to 16 bytes
			CompressedSequenceOffset = acl::align_to(CompressedSequenceOffset, 16);

			// Copy our data
			FMemory::Memcpy(CompressedBytes.GetData() + CompressedSequenceOffset, Mapping.tracks, Mapping.tracks->get_size());

			// Increment our offset but don't align since we don't want to add unnecesary padding at the end of the last sequence
			CompressedSequenceOffset += Mapping.tracks->get_size();
		}

		// Free the instance we no longer need
		ACLAllocatorImpl.deallocate(MergedDB, CompressedDatabaseSize);

		// Free our duplicate compressed clips
		for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
		{
			GMalloc->Free(Mapping.tracks);
		}
	}
}
#endif

void UAnimationCompressionLibraryDatabase::PostLoad()
{
	Super::PostLoad();

	if (CompressedBytes.Num() != 0)
	{
		const acl::compressed_database* CompressedDatabase = acl::make_compressed_database(CompressedBytes.GetData());

		DatabaseStreamer = MakeUnique<NullDatabaseStreamer>(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

		const bool ContextInitResult = DatabaseContext.initialize(ACLAllocatorImpl, *CompressedDatabase, *DatabaseStreamer);
		checkf(ContextInitResult, TEXT("ACL failed to initialize the database context"));

		// Stream everything right away, we live in memory for now
		DatabaseContext.stream_in();
	}
}
