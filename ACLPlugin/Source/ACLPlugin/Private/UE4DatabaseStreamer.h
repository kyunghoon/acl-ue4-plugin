#pragma once

// Copyright 2020 Nicholas Frechette. All Rights Reserved.

#include "AnimationCompression.h"
#include "CoreMinimal.h"
#include "HAL/UnrealMemory.h"

#include "ACLImpl.h"

#include <acl/database/idatabase_streamer.h>

// UE 4.25 doesn't expose its virtual memory management, see FPlatformMemory::FPlatformVirtualMemoryBlock
#define WITH_VMEM_MANAGEMENT 0

/** A simple async UE4 streamer. Memory is allocated on the first stream in request and deallocated on the last stream out request. */
class UE4DatabaseStreamer final : public acl::idatabase_streamer
{
public:
	UE4DatabaseStreamer(FByteBulkData& StreamableBulkData_, uint32 BulkDataSize_)
		: StreamableBulkData(StreamableBulkData_)
		, BulkDataPtr(nullptr)
		, PendingIORequest(nullptr)
		, BulkDataSize(BulkDataSize_)
	{
#if WITH_VMEM_MANAGEMENT
		// Allocate but don't commit the memory until we need it
		// TODO: Commit right away if requested
		StreamedBulkDataBlock = FPlatformMemory::FPlatformVirtualMemoryBlock::AllocateVirtual(BulkDataSize_);
		BulkDataPtr = static_cast<uint8*>(StreamedBulkDataBlock.GetVirtualPointer());
		bIsBulkDataCommited = false;
#endif
	}

	virtual ~UE4DatabaseStreamer()
	{
		// If we have a stream in request, wait for it to complete and clear it
		WaitForStreamingToComplete();

#if WITH_VMEM_MANAGEMENT
		StreamedBulkDataBlock.FreeVirtual();
#else
		delete[] BulkDataPtr;
#endif
	}

	virtual bool is_initialized() const override { return true; }

	virtual const uint8_t* get_bulk_data() const override { return BulkDataPtr; }

	virtual void stream_in(uint32_t Offset, uint32_t Size, bool CanAllocateBulkData, const std::function<void(bool Success)>& Continuation) override
	{
		checkf(Offset < BulkDataSize, TEXT("Steam offset is outside of the bulk data range"));
		checkf(Size <= BulkDataSize, TEXT("Stream size is larger than the bulk data size"));
		checkf(uint64(Offset) + uint64(Size) <= uint64(BulkDataSize), TEXT("Streaming request is outside of the bulk data range"));

		// If we already did a stream in request, wait for it to complete and clear it
		WaitForStreamingToComplete();

		FBulkDataIORequestCallBack AsyncFileCallBack = [this, Continuation](bool bWasCancelled, IBulkDataIORequest* Req)
		{
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL completed the stream in request!"));

			// Tell ACL whether the streaming request was a success or not, this is thread safe
			Continuation(!bWasCancelled);
		};

		UE_LOG(LogAnimationCompression, Log, TEXT("ACL starting a new stream in request!"));

		// Allocate our bulk data buffer on the first stream in request
		if (CanAllocateBulkData)
		{
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL is allocating the database bulk data!"));

#if WITH_VMEM_MANAGEMENT
			check(!bIsBulkDataCommited);
			StreamedBulkDataBlock.Commit();
			bIsBulkDataCommited = true;
#else
			check(BulkDataPtr == nullptr);
			BulkDataPtr = new uint8[BulkDataSize];
#endif
		}

		// Fire off our async streaming request
		PendingIORequest = StreamableBulkData.CreateStreamingRequest(Offset, Size, AIOP_Low, &AsyncFileCallBack, BulkDataPtr);
		if (PendingIORequest == nullptr)
		{
			UE_LOG(LogAnimationCompression, Warning, TEXT("ACL failed to initiate database stream in request!"));
			Continuation(false);
		}
	}

	virtual void stream_out(uint32_t Offset, uint32_t Size, bool CanDeallocateBulkData, const std::function<void()>& Continuation) override
	{
		checkf(Offset < BulkDataSize, TEXT("Steam offset is outside of the bulk data range"));
		checkf(Size <= BulkDataSize, TEXT("Stream size is larger than the bulk data size"));
		checkf(uint64(Offset) + uint64(Size) <= uint64(BulkDataSize), TEXT("Streaming request is outside of the bulk data range"));

		// If we already did a stream in request, wait for it to complete and clear it
		WaitForStreamingToComplete();

		UE_LOG(LogAnimationCompression, Log, TEXT("ACL is streaming out a database!"));

		// Free our bulk data on the last stream out request
		if (CanDeallocateBulkData)
		{
			// TODO: Make this optional?
			UE_LOG(LogAnimationCompression, Log, TEXT("ACL is deallocating the database bulk data!"));

#if WITH_VMEM_MANAGEMENT
			check(bIsBulkDataCommited);
			StreamedBulkDataBlock.Decommit();
			bIsBulkDataCommited = false;
#else
			check(BulkDataPtr != nullptr);
			delete[] BulkDataPtr;
			BulkDataPtr = nullptr;
#endif
		}

		// Notify ACL that we streamed out the data, this is not thread safe and cannot run while animations are decompressing
		Continuation();
	}

	void WaitForStreamingToComplete()
	{
		if (PendingIORequest != nullptr)
		{
			verify(PendingIORequest->WaitCompletion());
			delete PendingIORequest;
			PendingIORequest = nullptr;
		}
	}

private:
	UE4DatabaseStreamer(const UE4DatabaseStreamer&) = delete;
	UE4DatabaseStreamer& operator=(const UE4DatabaseStreamer&) = delete;

	FByteBulkData& StreamableBulkData;
	uint8* BulkDataPtr;

	IBulkDataIORequest* PendingIORequest;

#if WITH_VMEM_MANAGEMENT
	FPlatformMemory::FPlatformVirtualMemoryBlock StreamedBulkDataBlock;
	bool bIsBulkDataCommited;
#endif

	uint32 BulkDataSize;
};
