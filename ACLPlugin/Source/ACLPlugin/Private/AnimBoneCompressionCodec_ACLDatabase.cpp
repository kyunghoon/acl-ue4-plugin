// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AnimBoneCompressionCodec_ACLDatabase.h"

#if WITH_EDITORONLY_DATA
#include "Rendering/SkeletalMeshModel.h"

#include "ACLImpl.h"

#include <acl/compression/track_error.h>
#include <acl/decompression/decompress.h>
#endif	// WITH_EDITORONLY_DATA

#include "ACLDecompressionImpl.h"

void FACLDatabaseCompressedAnimData::SerializeCompressedData(FArchive& Ar)
{
	ICompressedAnimData::SerializeCompressedData(Ar);

#if WITH_EDITORONLY_DATA
	if (!Ar.IsFilterEditorOnly())
	{
		Ar << CompressedDatabase;
	}
#endif
}

bool FACLDatabaseCompressedAnimData::IsValid() const
{
	if (CompressedByteStream.Num() == 0 || CompressedDatabase.Num() == 0)
	{
		return false;
	}

	const acl::compressed_tracks* CompressedClipData = acl::make_compressed_tracks(CompressedByteStream.GetData());
	if (CompressedClipData == nullptr || CompressedClipData->is_valid(false).any())
	{
		return false;
	}

	const acl::compressed_database* CompressedDatabaseData = acl::make_compressed_database(CompressedDatabase.GetData());
	if (CompressedDatabaseData == nullptr || CompressedDatabaseData->is_valid(false).any())
	{
		return false;
	}

	return true;
}

// TODO:
// On anim data destruction, if we contain data and a codec pointer, mark the database as stale
// On database registration, mark the database and package as stale
// In editor, if the database is built, use it, if it is stale, use the individual one
// In editor, we can rebuild the database by saving the codec
// On cook, we serialize the database (and build it if we need to)
// In a cooked build, the individual database is stripped, only the baked one is present
// In non-shipping builds, check if the baked database contains our clip after the anim data is bound
// In editor, register our database when bound and mark as stale as appropriate

UAnimBoneCompressionCodec_ACLDatabase::UAnimBoneCompressionCodec_ACLDatabase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITORONLY_DATA
	PreviewTier = -1;
#endif	// WITH_EDITORONLY_DATA
}

#if WITH_EDITORONLY_DATA
void UAnimBoneCompressionCodec_ACLDatabase::RegisterDatabase(const FCompressibleAnimData& CompressibleAnimData, acl::compressed_database* CompressedDatabase, FCompressibleAnimDataResult& OutResult)
{
	check(CompressedDatabase != nullptr && CompressedDatabase->is_valid(false).empty());

	FACLDatabaseCompressedAnimData& AnimData = static_cast<FACLDatabaseCompressedAnimData&>(*OutResult.AnimData);

	const uint32 CompressedDatabaseSize = CompressedDatabase->get_size();

	AnimData.CompressedDatabase.Empty(CompressedDatabaseSize);
	AnimData.CompressedDatabase.AddUninitialized(CompressedDatabaseSize);
	FMemory::Memcpy(AnimData.CompressedDatabase.GetData(), CompressedDatabase, CompressedDatabaseSize);

#if 0
	acl::compressed_database** OldCompressedDatabasePtr = SequenceToDatabaseMap.Find(CompressibleAnimData.AnimFName);
	if (OldCompressedDatabasePtr != nullptr)
	{
		// We have stale data, free it and replace it with the new data
		acl::compressed_database* OldCompressedDatabase = *OldCompressedDatabasePtr;

		ACLAllocator AllocatorImpl;
		AllocatorImpl.deallocate(OldCompressedDatabase, OldCompressedDatabase->get_size());

		SequenceToDatabaseMap[CompressibleAnimData.AnimFName] = CompressedDatabase;
	}
	else
	{
		// New data
		SequenceToDatabaseMap.Add(CompressibleAnimData.AnimFName, CompressedDatabase);
	}

	MarkPackageDirty();
#endif
}

void UAnimBoneCompressionCodec_ACLDatabase::GetCompressionSettings(acl::compression_settings& OutSettings) const
{
	OutSettings = acl::get_default_compression_settings();

	OutSettings.level = GetCompressionLevel(CompressionLevel);
}

void UAnimBoneCompressionCodec_ACLDatabase::BeginDestroy()
{
	Super::BeginDestroy();

	ACLAllocator AllocatorImpl;

	for (TPair<FName, acl::compressed_database*>& Pair : SequenceToDatabaseMap)
	{
		//AllocatorImpl.deallocate(Pair.Value, Pair.Value->get_size());
	}
}

void UAnimBoneCompressionCodec_ACLDatabase::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

#if WITH_EDITORONLY_DATA
	if (!Ar.IsCooking())
	{
#if 0
		if (Ar.IsSaving())
		{
			int32 NumEntries = SequenceToDatabaseMap.Num();
			Ar << NumEntries;

			for (TPair<FName, acl::compressed_database*>& Pair : SequenceToDatabaseMap)
			{
				Ar << Pair.Key;

				int32 DataSize = Pair.Value->get_size();
				Ar << DataSize;
				Ar.Serialize(Pair.Value, DataSize);
			}
		}
		else if (Ar.IsLoading())
		{
			ACLAllocator AllocatorImpl;

			int32 NumEntries = 0;
			Ar << NumEntries;

			for (int32 EntryIndex = 0; EntryIndex < NumEntries; ++EntryIndex)
			{
				FName Key;
				Ar << Key;

				int32 DataSize = 0;
				Ar << DataSize;

				void* Buffer = AllocatorImpl.allocate(DataSize);
				Ar.Serialize(Buffer, DataSize);

				acl::compressed_database* CompressedDatabase = acl::make_compressed_database(Buffer);
				check(CompressedDatabase != nullptr && CompressedDatabase->is_valid(false).empty());

				SequenceToDatabaseMap.Add(Key, CompressedDatabase);
			}
		}
#endif
	}
#endif
}

void UAnimBoneCompressionCodec_ACLDatabase::PopulateDDCKey(FArchive& Ar)
{
	Super::PopulateDDCKey(Ar);

	acl::compression_settings Settings;
	GetCompressionSettings(Settings);

	uint32 ForceRebuildVersion = 1;
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

class NullDatabaseStreamer final : public acl::idatabase_streamer
{
public:
	NullDatabaseStreamer(const uint8_t* bulk_data, uint32_t bulk_data_size)
		: m_bulk_data(bulk_data)
		, m_bulk_data_size(bulk_data_size)
	{
	}

	virtual bool is_initialized() const override {return m_bulk_data != nullptr; }

	virtual const uint8_t* get_bulk_data() const override { return m_bulk_data; }

	virtual void stream_in(uint32_t offset, uint32_t size, const std::function<void(bool success)>& continuation) override
	{
		continuation(true);
	}

	virtual void stream_out(uint32_t offset, uint32_t size, const std::function<void(bool success)>& continuation) override
	{
		continuation(true);
	}

private:
	NullDatabaseStreamer(const NullDatabaseStreamer&) = delete;
	NullDatabaseStreamer& operator=(const NullDatabaseStreamer&) = delete;

	const uint8_t* m_bulk_data;
	uint32_t m_bulk_data_size;
};

void UAnimBoneCompressionCodec_ACLDatabase::DecompressPose(FAnimSequenceDecompressionContext& DecompContext, const BoneTrackArray& RotationPairs, const BoneTrackArray& TranslationPairs, const BoneTrackArray& ScalePairs, TArrayView<FTransform>& OutAtoms) const
{
	ACLAllocator AllocatorImpl;

	const FACLDatabaseCompressedAnimData& AnimData = static_cast<const FACLDatabaseCompressedAnimData&>(DecompContext.CompressedAnimData);
	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	acl::decompression_context<UE4DefaultDecompressionSettings, UE4DefaultDatabaseSettings> ACLContext;
	acl::database_context<UE4DefaultDatabaseSettings> DatabaseContext;

#if WITH_EDITORONLY_DATA
	const acl::compressed_database* CompressedDatabase = AnimData.GetCompressedDatabase();

	NullDatabaseStreamer Streamer(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

	DatabaseContext.initialize(AllocatorImpl, *CompressedDatabase, Streamer);
	ACLContext.initialize(*CompressedClipData, DatabaseContext);

	if (PreviewTier == -1 || PreviewTier >= 1)
	{
		// If we don't have a preview value or if we preview everything, stream everything in
		DatabaseContext.stream_in();
	}
#else
	ACLContext.initialize(*CompressedClipData);
#endif

	::DecompressPose(DecompContext, ACLContext, RotationPairs, TranslationPairs, ScalePairs, OutAtoms);
}

void UAnimBoneCompressionCodec_ACLDatabase::DecompressBone(FAnimSequenceDecompressionContext& DecompContext, int32 TrackIndex, FTransform& OutAtom) const
{
	ACLAllocator AllocatorImpl;

	const FACLDatabaseCompressedAnimData& AnimData = static_cast<const FACLDatabaseCompressedAnimData&>(DecompContext.CompressedAnimData);
	const acl::compressed_tracks* CompressedClipData = AnimData.GetCompressedTracks();
	check(CompressedClipData != nullptr && CompressedClipData->is_valid(false).empty());

	acl::decompression_context<UE4DefaultDecompressionSettings, UE4DefaultDatabaseSettings> ACLContext;
	acl::database_context<UE4DefaultDatabaseSettings> DatabaseContext;

#if WITH_EDITORONLY_DATA
	const acl::compressed_database* CompressedDatabase = AnimData.GetCompressedDatabase();

	NullDatabaseStreamer Streamer(CompressedDatabase->get_bulk_data(), CompressedDatabase->get_bulk_data_size());

	DatabaseContext.initialize(AllocatorImpl, *CompressedDatabase, Streamer);
	ACLContext.initialize(*CompressedClipData, DatabaseContext);

	if (PreviewTier == -1 || PreviewTier >= 1)
	{
		// If we don't have a preview value or if we preview everything, stream everything in
		DatabaseContext.stream_in();
	}
#else
	ACLContext.initialize(*CompressedClipData);
#endif

	::DecompressBone(DecompContext, ACLContext, TrackIndex, OutAtom);
}
