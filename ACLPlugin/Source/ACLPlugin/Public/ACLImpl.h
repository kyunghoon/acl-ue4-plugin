#pragma once

// Copyright 2018 Nicholas Frechette. All Rights Reserved.

#include "CoreMinimal.h"

#if DO_GUARD_SLOW
	// ACL has a lot of asserts, only enabled in Debug
	// This decision has been made because the extensive asserts add considerable overhead and most
	// developers use a Development configuration for the editor. The ACL library runs extensive
	// unit and regression tests on a very large number of clips which minimizes the risk of
	// having a legitimate assert fire.
	#define ACL_ON_ASSERT_CUSTOM
	#define ACL_ASSERT(expression, format, ...) checkf(expression, TEXT(format), #__VA_ARGS__)
#endif

#if PLATFORM_PS4
	// Enable usage of popcount instruction
	#define ACL_USE_POPCOUNT
#endif

#include <acl/core/error.h>
#include <acl/core/iallocator.h>
#include <acl/database/idatabase_streamer.h>	// TODO: Remove me once the stuff below is moved out
#include <acl/decompression/decompress.h>

#include <rtm/quatf.h>
#include <rtm/vector4f.h>
#include <rtm/qvvf.h>

/** The ACL allocator implementation simply forwards to the default heap allocator. */
class ACLAllocator final : public acl::iallocator
{
public:
	virtual void* allocate(size_t size, size_t alignment = acl::iallocator::k_default_alignment)
	{
		return GMalloc->Malloc(size, alignment);
	}

	virtual void deallocate(void* ptr, size_t size)
	{
		GMalloc->Free(ptr);
	}
};

extern ACLAllocator ACLAllocatorImpl;

/** RTM <-> UE4 conversion utilities */
inline rtm::vector4f RTM_SIMD_CALL VectorCast(const FVector& Input) { return rtm::vector_set(Input.X, Input.Y, Input.Z); }
inline FVector RTM_SIMD_CALL VectorCast(rtm::vector4f_arg0 Input) { return FVector(rtm::vector_get_x(Input), rtm::vector_get_y(Input), rtm::vector_get_z(Input)); }
inline rtm::quatf RTM_SIMD_CALL QuatCast(const FQuat& Input) { return rtm::quat_set(Input.X, Input.Y, Input.Z, Input.W); }
inline FQuat RTM_SIMD_CALL QuatCast(rtm::quatf_arg0 Input) { return FQuat(rtm::quat_get_x(Input), rtm::quat_get_y(Input), rtm::quat_get_z(Input), rtm::quat_get_w(Input)); }
inline rtm::qvvf RTM_SIMD_CALL TransformCast(const FTransform& Input) { return rtm::qvv_set(QuatCast(Input.GetRotation()), VectorCast(Input.GetTranslation()), VectorCast(Input.GetScale3D())); }
inline FTransform RTM_SIMD_CALL TransformCast(rtm::qvvf_arg0 Input) { return FTransform(QuatCast(Input.rotation), VectorCast(Input.translation), VectorCast(Input.scale)); }

/** The decompression settings used by ACL */
using UE4DefaultDecompressionSettings = acl::default_transform_decompression_settings;
using UE4CustomDecompressionSettings = acl::debug_transform_decompression_settings;

struct UE4SafeDecompressionSettings final : public UE4DefaultDecompressionSettings
{
	static constexpr bool is_rotation_format_supported(acl::rotation_format8 format) { return format == acl::rotation_format8::quatf_full; }
	static constexpr acl::rotation_format8 get_rotation_format(acl::rotation_format8 /*format*/) { return acl::rotation_format8::quatf_full; }
};

using UE4DefaultDatabaseSettings = acl::default_database_settings;

struct UE4DefaultDBDecompressionSettings final : public UE4DefaultDecompressionSettings
{
	using database_settings_type = UE4DefaultDatabaseSettings;
};

using UE4DebugDatabaseSettings = acl::debug_database_settings;

struct UE4DebugDBDecompressionSettings final : public acl::debug_transform_decompression_settings
{
	using database_settings_type = UE4DebugDatabaseSettings;
};

/** UE4 equivalents for some ACL enums */
/** An enum for ACL rotation formats. */
UENUM()
enum ACLRotationFormat
{
	ACLRF_Quat_128 UMETA(DisplayName = "Quat Full Bit Rate"),
	ACLRF_QuatDropW_96 UMETA(DisplayName = "Quat Drop W Full Bit Rate"),
	ACLRF_QuatDropW_Variable UMETA(DisplayName = "Quat Drop W Variable Bit Rate"),
};

/** An enum for ACL Vector3 formats. */
UENUM()
enum ACLVectorFormat
{
	ACLVF_Vector3_96 UMETA(DisplayName = "Vector3 Full Bit Rate"),
	ACLVF_Vector3_Variable UMETA(DisplayName = "Vector3 Variable Bit Rate"),
};

/** An enum for ACL compression levels. */
UENUM()
enum ACLCompressionLevel
{
	ACLCL_Lowest UMETA(DisplayName = "Lowest"),
	ACLCL_Low UMETA(DisplayName = "Low"),
	ACLCL_Medium UMETA(DisplayName = "Medium"),
	ACLCL_High UMETA(DisplayName = "High"),
	ACLCL_Highest UMETA(DisplayName = "Highest"),
};

/** Editor only utilities */
#if WITH_EDITOR
#include <acl/compression/track_array.h>
#include <acl/compression/compression_level.h>

acl::rotation_format8 GetRotationFormat(ACLRotationFormat Format);
acl::vector_format8 GetVectorFormat(ACLVectorFormat Format);
acl::compression_level8 GetCompressionLevel(ACLCompressionLevel Level);

acl::track_array_qvvf BuildACLTransformTrackArray(ACLAllocator& AllocatorImpl, const FCompressibleAnimData& CompressibleAnimData, float DefaultVirtualVertexDistance, float SafeVirtualVertexDistance, bool bBuildAdditiveBase);
#endif // WITH_EDITOR
