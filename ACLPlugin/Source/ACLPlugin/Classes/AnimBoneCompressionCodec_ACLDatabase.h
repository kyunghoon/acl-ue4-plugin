#pragma once

// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include <acl/database/database.h>
#include <acl/database/idatabase_streamer.h>

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AnimBoneCompressionCodec_ACLBase.h"
#include "AnimBoneCompressionCodec_ACLDatabase.generated.h"

using UE4DefaultDatabaseSettings = acl::default_database_settings;

struct FACLDatabaseCompressedAnimData final : public ICompressedAnimData
{
	/** Holds the compressed_tracks instance */
	TArrayView<uint8> CompressedByteStream;

#if WITH_EDITORONLY_DATA
	/** Holds the compressed_database instance */
	TArray<uint8> CompressedDatabase;

	/** The codec instance that owns us */
	class UAnimBoneCompressionCodec_ACLDatabase* Codec = nullptr;
#endif

	const acl::compressed_tracks* GetCompressedTracks() const { return acl::make_compressed_tracks(CompressedByteStream.GetData()); }

#if WITH_EDITORONLY_DATA
	const acl::compressed_database* GetCompressedDatabase() const { return acl::make_compressed_database(CompressedDatabase.GetData()); }
#endif

	// ICompressedAnimData implementation
	virtual void SerializeCompressedData(FArchive& Ar);
	virtual void Bind(const TArrayView<uint8> BulkData) override { CompressedByteStream = BulkData; }
	virtual int64 GetApproxCompressedSize() const override { return CompressedByteStream.Num(); }
	virtual bool IsValid() const override;
};

/** The default database codec implementation for ACL support with the minimal set of exposed features for ease of use. */
UCLASS(MinimalAPI, config = Engine, meta = (DisplayName = "Anim Compress ACL Database"))
class UAnimBoneCompressionCodec_ACLDatabase : public UAnimBoneCompressionCodec_ACLBase
{
	GENERATED_UCLASS_BODY()

	/** The database asset that will hold the compressed animation data. */
	UPROPERTY(EditAnywhere, Category = "ACL Options")
	class UAnimationCompressionLibraryDatabase* DatabaseAsset;

#if WITH_EDITORONLY_DATA
	/** The skeletal meshes used to estimate the skinning deformation during compression. */
	UPROPERTY(EditAnywhere, Category = "ACL Options")
	TArray<class USkeletalMesh*> OptimizationTargets;

	/** The database tier to use when decompressing. Must be -1, 0, 1, or 2. */
	UPROPERTY(EditAnywhere, Category = "ACL Debug Options", meta = (ClampMin = "-1", ClampMax = "2"))
	int32 PreviewTier;	// TODO: Make this transient, we don't need to serialize this, make it an enum too

	//////////////////////////////////////////////////////////////////////////
	// UAnimBoneCompressionCodec implementation
	virtual void PopulateDDCKey(FArchive& Ar) override;

	// UAnimBoneCompressionCodec_ACLBase implementation
	virtual bool UseDatabase() const override { return true; }
	virtual void RegisterDatabase(const FCompressibleAnimData& CompressibleAnimData, acl::compressed_database* CompressedDatabase, FCompressibleAnimDataResult& OutResult) override;
	virtual void GetCompressionSettings(acl::compression_settings& OutSettings) const override;
	virtual TArray<class USkeletalMesh*> GetOptimizationTargets() const override { return OptimizationTargets; }
#endif

	// UAnimBoneCompressionCodec implementation
	virtual TUniquePtr<ICompressedAnimData> AllocateAnimData() const override;
	virtual void ByteSwapIn(ICompressedAnimData& AnimData, TArrayView<uint8> CompressedData, FMemoryReader& MemoryStream) const override;
	virtual void ByteSwapOut(ICompressedAnimData& AnimData, TArrayView<uint8> CompressedData, FMemoryWriter& MemoryStream) const override;
	virtual void DecompressPose(FAnimSequenceDecompressionContext& DecompContext, const BoneTrackArray& RotationPairs, const BoneTrackArray& TranslationPairs, const BoneTrackArray& ScalePairs, TArrayView<FTransform>& OutAtoms) const override;
	virtual void DecompressBone(FAnimSequenceDecompressionContext& DecompContext, int32 TrackIndex, FTransform& OutAtom) const override;
};
