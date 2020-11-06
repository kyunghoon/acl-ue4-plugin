// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AnimBoneCompressionCodec_ACLDatabase.h"

#include "Algo/BinarySearch.h"

#if WITH_EDITORONLY_DATA
#include "Animation/AnimBoneCompressionSettings.h"
#include "Rendering/SkeletalMeshModel.h"

#include "ACLImpl.h"

#include <acl/compression/compress.h>
#include <acl/compression/track_error.h>
#include <acl/decompression/decompress.h>
#endif	// WITH_EDITORONLY_DATA

#include "ACLDecompressionImpl.h"

void FACLDatabaseCompressedAnimData::SerializeCompressedData(FArchive& Ar)
{
	ICompressedAnimData::SerializeCompressedData(Ar);

	Ar << SequenceNameHash;

#if WITH_EDITORONLY_DATA
	if (!Ar.IsFilterEditorOnly())
	{
		Ar << CompressedClip;
		Ar << CompressedDatabase;
	}
#endif
}

void FACLDatabaseCompressedAnimData::Bind(const TArrayView<uint8> BulkData)
{
	check(BulkData.Num() == 0);	// Should always be empty

#if !WITH_EDITORONLY_DATA
	// In a cooked build, we lookup our anim sequence and database from the database asset
	// We search by the sequence hash which lives in the top 32 bits of each entry
	const int32 SequenceIndex = Algo::BinarySearchBy(Codec->DatabaseAsset->CookedAnimSequenceMappings, SequenceNameHash, [](uint64 InValue) { return uint32(InValue >> 32); });
	if (SequenceIndex != INDEX_NONE)
	{
		const uint32 CompressedClipOffset = uint32(Codec->DatabaseAsset->CookedAnimSequenceMappings[SequenceIndex]);	// Truncate top 32 bits
		uint8* CompressedBytes = Codec->DatabaseAsset->CompressedBytes.GetData() + CompressedClipOffset;

		const acl::compressed_tracks* CompressedClipData = acl::make_compressed_tracks(CompressedBytes);
		check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

		const uint32 CompressedSize = CompressedClipData->get_size();

		CompressedByteStream = TArrayView<uint8>(CompressedBytes, CompressedSize);
		DatabaseContext = &Codec->DatabaseAsset->DatabaseContext;
	}
	else
	{
		// This sequence doesn't live in the database, the mapping must be stale
		UE_LOG(LogAnimationCompression, Warning, TEXT("ACL Database mapping is stale. [0x%X] should be contained but isn't."), SequenceNameHash);

		// Since we have no sequence data, decompression will yield a T-pose
	}
#endif
}

int64 FACLDatabaseCompressedAnimData::GetApproxCompressedSize() const
{
#if WITH_EDITORONLY_DATA
	return CompressedClip.Num();
#else
	return CompressedByteStream.Num();
#endif
}

bool FACLDatabaseCompressedAnimData::IsValid() const
{
	if (CompressedByteStream.Num() == 0)
	{
		return false;
	}

	const acl::compressed_tracks* CompressedClipData = acl::make_compressed_tracks(CompressedByteStream.GetData());
	if (CompressedClipData == nullptr || CompressedClipData->is_valid(false).any())
	{
		return false;
	}

#if WITH_EDITORONLY_DATA
	if (CompressedDatabase.Num() == 0)
	{
		return false;
	}

	const acl::compressed_database* CompressedDatabaseData = acl::make_compressed_database(CompressedDatabase.GetData());
	if (CompressedDatabaseData == nullptr || CompressedDatabaseData->is_valid(false).any())
	{
		return false;
	}
#else
	if (DatabaseContext == nullptr)
	{
		return false;
	}
#endif

	return true;
}

UAnimBoneCompressionCodec_ACLDatabase::UAnimBoneCompressionCodec_ACLDatabase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, DatabaseAsset(nullptr)
{
}

#if WITH_EDITORONLY_DATA
void UAnimBoneCompressionCodec_ACLDatabase::GetPreloadDependencies(TArray<UObject*>& OutDeps)
{
	Super::GetPreloadDependencies(OutDeps);

	// We preload the database asset because we need it loaded during Serialize to lookup the proper sequence data
	if (DatabaseAsset != nullptr)
	{
		OutDeps.Add(DatabaseAsset);
	}
}

void UAnimBoneCompressionCodec_ACLDatabase::RegisterWithDatabase(const FCompressibleAnimData& CompressibleAnimData, acl::compressed_database* CompressedDatabase, FCompressibleAnimDataResult& OutResult)
{
	// After we are done compressing our animation sequence, it generated a database that contains only that single sequence.
	// In the editor, we'll be able to use that database for playback.
	// During cooking, the anim data serialization function will check if the sequence is contained within the database
	// mapping.
	//
	// If it is contained, we will not save any compressed data, instead we'll save the mapping key.
	// When the anim sequence will load in the cooked build, we'll use the mapping key to find the compressed sequence
	// data and use it for decompression. The database asset will thus contain both the shared compressed database and the
	// compressed data for every anim sequence. This is required because generating the database requires us to modify the
	// compressed anim sequence data and we cannot do so after a sequence has been cooked.
	//
	// If the anim sequence isn't contained within the database mapping, it means that we assiged the codec but we haven't
	// rebuilt the mapping yet. We'll save the compressed data and the database data for that one anim sequence. We'll be
	// able to use it in the cooked build but it will not support streaming since it will fully live in memory.
	// A warning will be emitted that the mapping is stale and needs to be rebuilt.

	check(CompressedDatabase != nullptr && CompressedDatabase->is_valid(false).empty());

	FACLDatabaseCompressedAnimData& AnimData = static_cast<FACLDatabaseCompressedAnimData&>(*OutResult.AnimData);

	// Store the sequence name hash since we need it in cooked builds to find our data
	AnimData.SequenceNameHash = GetTypeHash(CompressibleAnimData.AnimFName);

	// Copy the sequence data
	AnimData.CompressedClip = OutResult.CompressedByteStream;

	// When we have a database, the compressed sequence data lives in the database, zero out the compressed byte buffer
	// since we handle the data manually ourself
	OutResult.CompressedByteStream.Empty(0);

	// Copy the database data
	const uint32 CompressedDatabaseSize = CompressedDatabase->get_size();

	AnimData.CompressedDatabase.Empty(CompressedDatabaseSize);
	AnimData.CompressedDatabase.AddUninitialized(CompressedDatabaseSize);
	FMemory::Memcpy(AnimData.CompressedDatabase.GetData(), CompressedDatabase, CompressedDatabaseSize);
}

void UAnimBoneCompressionCodec_ACLDatabase::GetCompressionSettings(acl::compression_settings& OutSettings) const
{
	OutSettings = acl::get_default_compression_settings();

	OutSettings.level = GetCompressionLevel(CompressionLevel);
}

void UAnimBoneCompressionCodec_ACLDatabase::PopulateDDCKey(FArchive& Ar)
{
	Super::PopulateDDCKey(Ar);

	acl::compression_settings Settings;
	GetCompressionSettings(Settings);

	uint32 ForceRebuildVersion = 2;
	uint32 SettingsHash = Settings.get_hash();

	Ar	<< ForceRebuildVersion << SettingsHash;

	for (USkeletalMesh* SkelMesh : OptimizationTargets)
	{
		FSkeletalMeshModel* MeshModel = SkelMesh != nullptr ? SkelMesh->GetImportedModel() : nullptr;
		if (MeshModel != nullptr)
		{
			Ar << MeshModel->SkeletalMeshModelGUID;
		}
	}
}
#endif // WITH_EDITORONLY_DATA

TUniquePtr<ICompressedAnimData> UAnimBoneCompressionCodec_ACLDatabase::AllocateAnimData() const
{
	TUniquePtr<FACLDatabaseCompressedAnimData> AnimData = MakeUnique<FACLDatabaseCompressedAnimData>();

	AnimData->Codec = const_cast<UAnimBoneCompressionCodec_ACLDatabase*>(this);

	return AnimData;
}

void UAnimBoneCompressionCodec_ACLDatabase::ByteSwapIn(ICompressedAnimData& AnimData, TArrayView<uint8> CompressedData, FMemoryReader& MemoryStream) const
{
#if !PLATFORM_LITTLE_ENDIAN
#error "ACL does not currently support big-endian platforms"
#endif

	// TODO: ACL does not support byte swapping
	FACLDatabaseCompressedAnimData& ACLAnimData = static_cast<FACLDatabaseCompressedAnimData&>(AnimData);
	MemoryStream.Serialize(ACLAnimData.CompressedByteStream.GetData(), ACLAnimData.CompressedByteStream.Num());
}

void UAnimBoneCompressionCodec_ACLDatabase::ByteSwapOut(ICompressedAnimData& AnimData, TArrayView<uint8> CompressedData, FMemoryWriter& MemoryStream) const
{
#if !PLATFORM_LITTLE_ENDIAN
#error "ACL does not currently support big-endian platforms"
#endif

	// TODO: ACL does not support byte swapping
	FACLDatabaseCompressedAnimData& ACLAnimData = static_cast<FACLDatabaseCompressedAnimData&>(AnimData);
	MemoryStream.Serialize(ACLAnimData.CompressedByteStream.GetData(), ACLAnimData.CompressedByteStream.Num());
}

void UAnimBoneCompressionCodec_ACLDatabase::DecompressPose(FAnimSequenceDecompressionContext& DecompContext, const BoneTrackArray& RotationPairs, const BoneTrackArray& TranslationPairs, const BoneTrackArray& ScalePairs, TArrayView<FTransform>& OutAtoms) const
{
	const FACLDatabaseCompressedAnimData& AnimData = static_cast<const FACLDatabaseCompressedAnimData&>(DecompContext.CompressedAnimData);

	acl::decompression_context<UE4DefaultDecompressionSettings, UE4DefaultDatabaseSettings> ACLContext;

#if WITH_EDITORONLY_DATA
	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	const acl::compressed_database* CompressedDatabase = AnimData.GetCompressedDatabase();

	NullDatabaseStreamer Streamer(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

	acl::database_context<UE4DefaultDatabaseSettings> SequenceDatabaseContext;
	SequenceDatabaseContext.initialize(ACLAllocatorImpl, *CompressedDatabase, Streamer);

	ACLContext.initialize(*CompressedClipData, SequenceDatabaseContext);

	const ACLDBPreviewState PreviewState = DatabaseAsset != nullptr ? DatabaseAsset->PreviewState : ACLDBPreviewState::None;
	switch (PreviewState)
	{
	default:
	case ACLDBPreviewState::None:
		// No preview state means we show the highest quality
		SequenceDatabaseContext.stream_in();
		break;
	case ACLDBPreviewState::HighQuality:
		// Stream in our high quality data
		SequenceDatabaseContext.stream_in();
		break;
	case ACLDBPreviewState::LowQuality:
		// Lowest quality means nothing is streamed in
		break;
	}
#else
	if (AnimData.CompressedByteStream.Num() == 0)
	{
		return;	// Our mapping must have been stale
	}

	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	if (AnimData.DatabaseContext == nullptr || !ACLContext.initialize(*CompressedClipData, *AnimData.DatabaseContext))
	{
		UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed initialize decompression context, database won't be used"));

		ACLContext.initialize(*CompressedClipData);
	}
#endif

	::DecompressPose(DecompContext, ACLContext, RotationPairs, TranslationPairs, ScalePairs, OutAtoms);
}

void UAnimBoneCompressionCodec_ACLDatabase::DecompressBone(FAnimSequenceDecompressionContext& DecompContext, int32 TrackIndex, FTransform& OutAtom) const
{
	const FACLDatabaseCompressedAnimData& AnimData = static_cast<const FACLDatabaseCompressedAnimData&>(DecompContext.CompressedAnimData);

	acl::decompression_context<UE4DefaultDecompressionSettings, UE4DefaultDatabaseSettings> ACLContext;

#if WITH_EDITORONLY_DATA
	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	const acl::compressed_database* CompressedDatabase = AnimData.GetCompressedDatabase();

	NullDatabaseStreamer Streamer(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

	acl::database_context<UE4DefaultDatabaseSettings> SequenceDatabaseContext;
	SequenceDatabaseContext.initialize(ACLAllocatorImpl, *CompressedDatabase, Streamer);

	ACLContext.initialize(*CompressedClipData, SequenceDatabaseContext);

	const ACLDBPreviewState PreviewState = DatabaseAsset != nullptr ? DatabaseAsset->PreviewState : ACLDBPreviewState::None;
	switch (PreviewState)
	{
	default:
	case ACLDBPreviewState::None:
		// No preview state means we show the highest quality
		SequenceDatabaseContext.stream_in();
		break;
	case ACLDBPreviewState::HighQuality:
		// Stream in our high quality data
		SequenceDatabaseContext.stream_in();
		break;
	case ACLDBPreviewState::LowQuality:
		// Lowest quality means nothing is streamed in
		break;
	}
#else
	if (AnimData.CompressedByteStream.Num() == 0)
	{
		return;	// Our mapping must have been stale
	}

	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	if (AnimData.DatabaseContext == nullptr || !ACLContext.initialize(*CompressedClipData, *AnimData.DatabaseContext))
	{
		UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed initialize decompression context, database won't be used"));

		ACLContext.initialize(*CompressedClipData);
	}
#endif

	::DecompressBone(DecompContext, ACLContext, TrackIndex, OutAtom);
}
