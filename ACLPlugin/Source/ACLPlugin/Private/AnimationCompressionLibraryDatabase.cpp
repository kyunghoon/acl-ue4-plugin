// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AnimationCompressionLibraryDatabase.h"
#include "AnimBoneCompressionCodec_ACLDatabase.h"
#include "UE4DatabaseStreamer.h"

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

	// Clear any stale data we might have
	CompressedBytes.Empty(0);
	CookedAnimSequenceMappings.Empty(0);
	StreamableBulkData.RemoveBulkData();

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

		acl::compression_database_settings Settings;	// Use defaults

		acl::compressed_database* MergedDB = nullptr;
		const acl::error_result MergeResult = acl::merge_compressed_databases(ACLAllocatorImpl, Settings, ACLDatabaseMappings.GetData(), ACLDatabaseMappings.Num(), MergedDB);

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

#if DO_GUARD_SLOW
		// Sanity check that the database is properly constructed
		{
			acl::database_context<UE4DefaultDatabaseSettings> DebugDatabaseContext;
			const bool ContextInitResult = DebugDatabaseContext.initialize(ACLAllocatorImpl, *MergedDB);
			checkf(ContextInitResult, TEXT("ACL failed to initialize the database context"));

			for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
			{
				check(MergedDB->contains(*Mapping.tracks));
				check(DebugDatabaseContext.contains(*Mapping.tracks));
			}
		}
#endif

		// Split our database to serialize the bulk data separately
		acl::compressed_database* SplitDB = nullptr;
		uint8* SplitDBBulkData = nullptr;
		const acl::error_result SplitResult = acl::split_compressed_database_bulk_data(ACLAllocatorImpl, *MergedDB, SplitDB, SplitDBBulkData);

		// Free the merged instance we no longer need
		ACLAllocatorImpl.deallocate(MergedDB, MergedDB->get_size());

		if (!SplitResult.empty())
		{
			// Free our duplicate compressed clips
			for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
			{
				GMalloc->Free(Mapping.tracks);
			}

			UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed to split database: %s"), ANSI_TO_TCHAR(SplitResult.c_str()));
			return;
		}

		checkSlow(SplitDB->is_valid(true).empty());

		const uint32 CompressedDatabaseSize = SplitDB->get_size();

		// Our compressed sequences follow the database in memory, aligned to 16 bytes
		uint32 CompressedSequenceOffset = acl::align_to(CompressedDatabaseSize, 16);

		// Write our our cooked offset mappings
		// We use an array for simplicity. UE4 doesn't support serializing a TMap or TSortedMap and so instead
		// we store an array of sorted FNames hashes and offsets. We'll use binary search to find our
		// index at runtime in O(logN) in the sorted names array, and read the offset we need in the other.
		// TODO: Use perfect hashing to bring it to O(1)

		const int32 NumMappings = ACLDatabaseMappings.Num();
		CookedAnimSequenceMappings.Empty(NumMappings);
		for (int32 MappingIndex = 0; MappingIndex < NumMappings; ++MappingIndex)
		{
			UAnimSequence* AnimSeq = CookedSequences[MappingIndex];
			const acl::database_merge_mapping& Mapping = ACLDatabaseMappings[MappingIndex];
			FACLDatabaseCompressedAnimData& AnimData = static_cast<FACLDatabaseCompressedAnimData&>(*AnimSeq->CompressedData.CompressedDataStructure);

			// Align our sequence to 16 bytes
			CompressedSequenceOffset = acl::align_to(CompressedSequenceOffset, 16);

			// Add our mapping
			CookedAnimSequenceMappings.Add((uint64(AnimData.SequenceNameHash) << 32) | uint64(CompressedSequenceOffset));

			// Increment our offset but don't align since we don't want to add unnecesary padding at the end of the last sequence
			CompressedSequenceOffset += Mapping.tracks->get_size();
		}

		// Make sure to sort our array, it'll be sorted by hash first since it lives in the top bits
		CookedAnimSequenceMappings.Sort();

		// Our full buffer size is our resulting offset
		const uint32 CompressedBytesSize = CompressedSequenceOffset;

		// Copy our database
		CompressedBytes.Empty(CompressedBytesSize);
		CompressedBytes.AddUninitialized(CompressedBytesSize);
		FMemory::Memcpy(CompressedBytes.GetData(), SplitDB, CompressedDatabaseSize);

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

		// Copy our bulk data
		const uint32 BulkDataSize = SplitDB->get_bulk_data_size();

		StreamableBulkData.Lock(LOCK_READ_WRITE);
		{
			void* BulkDataToSave = StreamableBulkData.Realloc(BulkDataSize);
			FMemory::Memcpy(BulkDataToSave, SplitDBBulkData, BulkDataSize);
		}
		StreamableBulkData.Unlock();

		// Free the split instance we no longer need
		ACLAllocatorImpl.deallocate(SplitDB, CompressedDatabaseSize);
		ACLAllocatorImpl.deallocate(SplitDBBulkData, BulkDataSize);

		// Free our duplicate compressed clips
		for (const acl::database_merge_mapping& Mapping : ACLDatabaseMappings)
		{
			GMalloc->Free(Mapping.tracks);
		}
	}
}
#endif

void UAnimationCompressionLibraryDatabase::BeginDestroy()
{
	Super::BeginDestroy();

	if (DatabaseStreamer)
	{
		// Wait for any pending IO requests
		UE4DatabaseStreamer* Streamer = (UE4DatabaseStreamer*)DatabaseStreamer.Release();
		Streamer->WaitForStreamingToComplete();

		// Reset our context to make sure it no longer references the streamer
		DatabaseContext.reset();

		// Free our streamer, it is no longer needed
		delete Streamer;
	}
}

void UAnimationCompressionLibraryDatabase::PostLoad()
{
	Super::PostLoad();

	if (CompressedBytes.Num() != 0)
	{
		const acl::compressed_database* CompressedDatabase = acl::make_compressed_database(CompressedBytes.GetData());
		check(CompressedDatabase != nullptr && CompressedDatabase->is_valid(false).empty());

		DatabaseStreamer = MakeUnique<UE4DatabaseStreamer>(StreamableBulkData, CompressedDatabase->get_bulk_data_size());

		const bool ContextInitResult = DatabaseContext.initialize(ACLAllocatorImpl, *CompressedDatabase, *DatabaseStreamer);
		checkf(ContextInitResult, TEXT("ACL failed to initialize the database context"));
	}
}

void UAnimationCompressionLibraryDatabase::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	bool bCooked = Ar.IsCooking();
	Ar << bCooked;

	if (bCooked)
	{
		StreamableBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
		StreamableBulkData.Serialize(Ar, this, INDEX_NONE, false);
	}
}

void UAnimationCompressionLibraryDatabase::StreamDatabaseIn(UAnimationCompressionLibraryDatabase* DatabaseAsset)
{
	// Must execute on the main thread but can do so at any point, even while animations are updating
	check(IsInGameThread());

	if (DatabaseAsset->DatabaseContext.is_initialized())
	{
		// The database context is used, our data has been cooked
		const acl::database_stream_request_result Result = DatabaseAsset->DatabaseContext.stream_in();
		switch (Result)
		{
		default:
			UE_LOG(LogAnimationCompression, Log, TEXT("Unknown ACL database stream request result: %u [%s]"), uint32(Result), *DatabaseAsset->GetPathName());
			break;
		case acl::database_stream_request_result::not_initialized:
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL database context not initialized [%s]"), *DatabaseAsset->GetPathName());
			break;
		case acl::database_stream_request_result::streaming:
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming is already in progress [%s]"), *DatabaseAsset->GetPathName());
			break;
		case acl::database_stream_request_result::dispatched:
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming request has been dispatched [%s]"), *DatabaseAsset->GetPathName());
			break;
		case acl::database_stream_request_result::done:
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming is done [%s]"), *DatabaseAsset->GetPathName());
			break;
		}
	}
	else
	{
#if WITH_EDITORONLY_DATA
		// Context isn't used, everything is in memory in the editor, just update the preview tier
		switch (DatabaseAsset->PreviewState)
		{
		default:
		case ACLDBPreviewState::None:
			// No preview state means we want to see the highest quality
			DatabaseAsset->PreviewState = ACLDBPreviewState::HighQuality;
			break;
		case ACLDBPreviewState::HighQuality:
			// Already showing the highest quality, nothing to do
			break;
		case ACLDBPreviewState::LowQuality:
			// Stream in the high quality data
			DatabaseAsset->PreviewState = ACLDBPreviewState::HighQuality;
			break;
		}
#endif
	}
}

void UAnimationCompressionLibraryDatabase::StreamDatabaseOut(UAnimationCompressionLibraryDatabase* DatabaseAsset)
{
	// Must execute on the main thread but must do so while animations aren't updating
	check(IsInGameThread());

	TFunction<bool(float)> StreamOutFun = [DatabaseAsset](float DeltaTime)
		{
			if (DatabaseAsset->DatabaseContext.is_initialized())
			{
				// The database context is used, our data has been cooked
				const acl::database_stream_request_result Result = DatabaseAsset->DatabaseContext.stream_out();
				switch (Result)
				{
				default:
					UE_LOG(LogAnimationCompression, Log, TEXT("Unknown ACL database stream request result: %u [%s]"), uint32(Result), *DatabaseAsset->GetPathName());
					break;
				case acl::database_stream_request_result::not_initialized:
					UE_LOG(LogAnimationCompression, Log, TEXT("ACL database context not initialized [%s]"), *DatabaseAsset->GetPathName());
					break;
				case acl::database_stream_request_result::streaming:
					UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming is already in progress [%s]"), *DatabaseAsset->GetPathName());
					break;
				case acl::database_stream_request_result::dispatched:
					UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming request has been dispatched [%s]"), *DatabaseAsset->GetPathName());
					break;
				case acl::database_stream_request_result::done:
					UE_LOG(LogAnimationCompression, Log, TEXT("ACL database streaming is done [%s]"), *DatabaseAsset->GetPathName());
					break;
				}
			}
			else
			{
	#if WITH_EDITORONLY_DATA
				// Context isn't used, everything is in memory in the editor, just update the preview tier
				switch (DatabaseAsset->PreviewState)
				{
				default:
				case ACLDBPreviewState::None:
					// No preview state means we want to see the lowest quality
					DatabaseAsset->PreviewState = ACLDBPreviewState::LowQuality;
					break;
				case ACLDBPreviewState::HighQuality:
					// Stream out the high quality data
					DatabaseAsset->PreviewState = ACLDBPreviewState::LowQuality;
					break;
				case ACLDBPreviewState::LowQuality:
					// Already showing the lowest quality, nothing to do
					break;
				}
	#endif
			}

			return false;
		};

	// Run later, once animations are done updating for sure
	FTicker::GetCoreTicker().AddTicker(TEXT("ACLDBStreamOut"), 0.0F, StreamOutFun);
}
