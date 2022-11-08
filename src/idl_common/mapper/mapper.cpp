/*
 * Copyright (C) 2020-2022 ARM Limited. All rights reserved.
 *
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <sync/sync.h>
#include <unistd.h>

#include <android-base/scopeguard.h>

#include "registered_handle_pool.h"
#include "mapper/mapper.h"
#include "idl_common/descriptor.h"
#include "core/buffer_allocation.h"
#include "core/buffer_descriptor.h"
#include "core/buffer_access.h"
#include "core/format_info.h"
#include "allocator/allocator.h"
#include "core/buffer.h"
#include "log.h"
#include "gralloc/formats.h"
#include "usages.h"

/* For error codes. */
#include <hardware/gralloc1.h>

#include "mapper/mapper_metadata.h"
#include "idl_common/shared_metadata.h"
#include "allocator/shared_memory/shared_memory.h"

/* GraphicBufferMapper is expected to be valid (and leaked) during process
 * termination. IMapper, and in turn, gRegisteredHandles must be valid as
 * well. Create the registered handle pool on the heap, and let
 * it leak for simplicity.
 *
 * However, there is no way to make sure gralloc0/gralloc1 are valid. Any use
 * of static/global object in gralloc0/gralloc1 that may have been destructed
 * is potentially broken.
 */
RegisteredHandlePool *gRegisteredHandles = new RegisteredHandlePool;

namespace arm
{
namespace mapper
{
namespace common
{

/*
 * Retrieves the file descriptor referring to a sync fence object
 *
 * @param fenceHandle [in]  HIDL fence handle
 * @param outFenceFd  [out] Fence file descriptor. '-1' indicates no fence
 *
 * @return false, for an invalid HIDL fence handle
 *         true, otherwise
 */
static bool getFenceFd(const hidl_handle &fenceHandle, int *outFenceFd)
{
	auto const handle = fenceHandle.getNativeHandle();
	if (handle && handle->numFds > 1)
	{
		MALI_GRALLOC_LOGE("Invalid fence handle with %d fds", handle->numFds);
		return false;
	}

	*outFenceFd = (handle && handle->numFds == 1) ? handle->data[0] : -1;
	return true;
}

/*
 * Populates the HIDL fence handle for the given fence object
 *
 * @param fenceFd       [in] Fence file descriptor
 * @param handleStorage [in] HIDL handle storage for fence
 *
 * @return HIDL fence handle
 */
static hidl_handle getFenceHandle(int fenceFd, char *handleStorage)
{
	native_handle_t *handle = nullptr;
	if (fenceFd >= 0)
	{
		handle = native_handle_init(handleStorage, 1, 0);
		handle->data[0] = fenceFd;
	}

	return hidl_handle(handle);
}

/*
 * Locks the given buffer for the specified CPU usage.
 *
 * @param bufferHandle [in]  Buffer to lock.
 * @param cpuUsage     [in]  Specifies one or more CPU usage flags to request
 * @param accessRegion [in]  Portion of the buffer that the client intends to access.
 * @param fenceFd      [in]  Fence file descriptor
 * @param outData      [out] CPU accessible buffer address
 *
 * @return Error::BAD_BUFFER for an invalid buffer
 *         Error::NO_RESOURCES when unable to duplicate fence
 *         Error::BAD_VALUE when locking fails
 *         Error::NONE on successful buffer lock
 */
static Error lockBuffer(buffer_handle_t bufferHandle, uint64_t cpuUsage, const IMapper::Rect &accessRegion, int fenceFd,
                        void **outData)
{
	/* dup fenceFd as it is going to be owned by gralloc. Note that it is
	 * gralloc's responsibility to close it, even on locking errors.
	 */
	if (fenceFd >= 0)
	{
		fenceFd = dup(fenceFd);
		if (fenceFd < 0)
		{
			MALI_GRALLOC_LOGE("Error encountered while duplicating fence file descriptor");
			return Error::NO_RESOURCES;
		}
	}

	if (private_handle_t::validate(bufferHandle) < 0)
	{
		if (fenceFd >= 0)
		{
			close(fenceFd);
		}
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = private_handle_t::downcast(bufferHandle);
	const auto format = private_handle->get_alloc_format();
	if (private_handle->cpu_write != 0 && (cpuUsage & BufferUsage::CPU_WRITE_MASK) &&
	    format.get_base() != MALI_GRALLOC_FORMAT_INTERNAL_BLOB)
	{
		if (fenceFd >= 0)
		{
			close(fenceFd);
		}
		MALI_GRALLOC_LOGE("Attempt to call lock*() for writing on an already locked buffer (%p)", bufferHandle);
		return Error::BAD_BUFFER;
	}

	void *data = nullptr;
	if (fenceFd >= 0)
	{
		sync_wait(fenceFd, -1);
		close(fenceFd);
	}

	auto result = mali_gralloc_lock(bufferHandle, cpuUsage, accessRegion.left, accessRegion.top, accessRegion.width,
	                                accessRegion.height, &data);
	if (result != 0)
	{
		MALI_GRALLOC_LOGE("Locking buffer failed with error: %d", result);
		if (result == GRALLOC1_ERROR_UNSUPPORTED)
		{
			return Error::BAD_BUFFER;
		}

		return result == -EINVAL ? Error::BAD_VALUE : Error::NO_RESOURCES;
	}

	*outData = data;
	return Error::NONE;
}

/*
 * Unlocks a buffer to indicate all CPU accesses to the buffer have completed
 *
 * @param bufferHandle [in]  Buffer to lock.
 * @param outFenceFd   [out] Fence file descriptor
 *
 * @return Error::BAD_BUFFER for an invalid buffer
 *         Error::BAD_VALUE when unlocking failed
 *         Error::NONE on successful buffer unlock
 */
static Error unlockBuffer(buffer_handle_t bufferHandle, int *outFenceFd)
{
	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = private_handle_t::downcast(bufferHandle);
	if (private_handle->lock_count == 0)
	{
		MALI_GRALLOC_LOGE("Attempt to call unlock*() on an unlocked buffer (%p)", bufferHandle);
		return Error::BAD_BUFFER;
	}

	const int result = mali_gralloc_unlock(bufferHandle);
	if (result)
	{
		MALI_GRALLOC_LOGE("Unlocking failed with error: %d", result);
		return Error::BAD_VALUE;
	}

	*outFenceFd = -1;

	return Error::NONE;
}

void importBuffer(const hidl_handle &raw_handle, IMapper::importBuffer_cb hidl_cb)
{
	if (!raw_handle.getNativeHandle())
	{
		MALI_GRALLOC_LOGE("Invalid buffer handle to import");
		hidl_cb(Error::BAD_BUFFER, nullptr);
		return;
	}

	native_handle_t *new_handle = native_handle_clone(raw_handle.getNativeHandle());
	if (new_handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Failed to clone buffer handle");
		hidl_cb(Error::NO_RESOURCES, nullptr);
		return;
	}

	auto close_handle = android::base::make_scope_guard([&new_handle]() {
		native_handle_close(new_handle);
		native_handle_delete(new_handle);
	});

	if (private_handle_t::validate(new_handle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", new_handle);
		hidl_cb(Error::BAD_BUFFER, nullptr);
		return;
	}

	auto *private_handle = private_handle_t::downcast(new_handle);
	private_handle->attr_base =
	    mmap(nullptr, private_handle->attr_size, PROT_READ | PROT_WRITE, MAP_SHARED, private_handle->share_attr_fd, 0);
	if (private_handle->attr_base == MAP_FAILED)
	{
		hidl_cb(Error::NO_RESOURCES, nullptr);
		return;
	}

	private_handle->import_pid = getpid();

	/* Cloned buffers don't share the same buffer mapping */
	private_handle->base = nullptr;
	private_handle->cpu_write = 0;

	if (!gRegisteredHandles->add(new_handle))
	{
		MALI_GRALLOC_LOGE("Handle %p has already been imported; potential fd leaking", new_handle);
		hidl_cb(Error::NO_RESOURCES, nullptr);
		return;
	}

	close_handle.Disable();
	hidl_cb(Error::NONE, new_handle);
}

Error freeBuffer(void *incoming_handle)
{
	if (gRegisteredHandles->remove(incoming_handle) == nullptr)
	{
		MALI_GRALLOC_LOGE("Invalid buffer handle to freeBuffer");
		return Error::BAD_BUFFER;
	}

	auto *handle = static_cast<native_handle *>(incoming_handle);

	auto *private_handle = private_handle_t::downcast(handle);
	if (private_handle->import_pid == getpid())
	{
		mali_unmap_buffer(private_handle);
		gralloc_shared_memory_free(private_handle->share_attr_fd, private_handle->attr_base, private_handle->attr_size);
	}

	private_handle->share_attr_fd = -1;
	private_handle->attr_base = MAP_FAILED;
	private_handle->import_pid = -1;

	native_handle_close(handle);
	native_handle_delete(handle);

	return Error::NONE;
}

void lock(void *buffer, uint64_t cpuUsage, const IMapper::Rect &accessRegion, const hidl_handle &acquireFence,
          IMapper::lock_cb hidl_cb)
{
	buffer_handle_t bufferHandle = gRegisteredHandles->get(buffer);
	if (!bufferHandle || private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer to lock: %p is not valid", buffer);
		hidl_cb(Error::BAD_BUFFER, nullptr);
		return;
	}

	int fenceFd;
	if (!getFenceFd(acquireFence, &fenceFd))
	{
		hidl_cb(Error::BAD_VALUE, nullptr);
		return;
	}

	void *data = nullptr;
	const Error error = lockBuffer(bufferHandle, cpuUsage, accessRegion, fenceFd, &data);

	hidl_cb(error, data);
}

void unlock(void *buffer, IMapper::unlock_cb hidl_cb)
{
	buffer_handle_t bufferHandle = gRegisteredHandles->get(buffer);
	if (!bufferHandle)
	{
		MALI_GRALLOC_LOGE("Buffer to unlock: %p has not been registered with Gralloc", buffer);
		hidl_cb(Error::BAD_BUFFER, nullptr);
		return;
	}

	int fenceFd;
	const Error error = unlockBuffer(bufferHandle, &fenceFd);
	if (error == Error::NONE)
	{
		NATIVE_HANDLE_DECLARE_STORAGE(fenceStorage, 1, 0);
		hidl_cb(error, getFenceHandle(fenceFd, fenceStorage));

		if (fenceFd >= 0)
		{
			close(fenceFd);
		}
	}
	else
	{
		hidl_cb(error, nullptr);
	}
}

Error validateBufferSize(void *buffer, const IMapper::BufferDescriptorInfo &descriptorInfo, uint32_t in_stride)
{
	/* The buffer must have been allocated by Gralloc */
	buffer_handle_t buffer_handle = gRegisteredHandles->get(buffer);
	if (!buffer_handle)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}

	private_handle_t *gralloc_buffer = private_handle_t::downcast(buffer_handle);
	if (gralloc_buffer == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", buffer_handle);
		return Error::BAD_BUFFER;
	}

	/* Validate the buffer parameters against descriptor info */

	/* The descriptor dimensions must match the buffer */
	if (static_cast<uint32_t>(gralloc_buffer->width) != descriptorInfo.width)
	{
		MALI_GRALLOC_LOGE("Width mismatch. Buffer width = %u, Descriptor width = %u", gralloc_buffer->width,
		                  descriptorInfo.width);
		return Error::BAD_VALUE;
	}

	if (static_cast<uint32_t>(gralloc_buffer->height) != descriptorInfo.height)
	{
		MALI_GRALLOC_LOGE("Height mismatch. Buffer height = %u, Descriptor height = %u", gralloc_buffer->height,
		                  descriptorInfo.height);
		return Error::BAD_VALUE;
	}

	if (gralloc_buffer->layer_count != descriptorInfo.layerCount)
	{
		MALI_GRALLOC_LOGE("Layer Count mismatch. Buffer layer_count = %u, Descriptor layer_count = %u",
		                  gralloc_buffer->layer_count, descriptorInfo.layerCount);
		return Error::BAD_VALUE;
	}

	/* Some usages need to match and the rest of the usage must be a subset of the buffer's usages */
	uint64_t must_match_mask = GRALLOC_USAGE_PROTECTED;
	uint64_t descriptor_usage = static_cast<uint64_t>(descriptorInfo.usage);
	uint64_t buffer_usage = gralloc_buffer->producer_usage | gralloc_buffer->consumer_usage;

	if ((buffer_usage & descriptor_usage) != descriptor_usage)
	{
		MALI_GRALLOC_LOGE("Usage not a subset. Buffer usage = %#" PRIx64 ", Descriptor usage = %#" PRIx64, buffer_usage,
		                  descriptor_usage);
		return Error::BAD_VALUE;
	}

	if ((buffer_usage & must_match_mask) != (descriptor_usage & must_match_mask))
	{
		MALI_GRALLOC_LOGE("Usage mismatch. Buffer usage = %#" PRIx64 ", Descriptor usage = %#" PRIx64, buffer_usage,
		                  descriptor_usage);
		return Error::BAD_VALUE;
	}

	/* The stride used should match the stride returned on buffer allocation. */
	if (in_stride != 0 && static_cast<uint32_t>(gralloc_buffer->stride) != in_stride)
	{
		MALI_GRALLOC_LOGE("Stride mismatch. Expected stride = %d, Buffer stride = %d", in_stride,
		                  gralloc_buffer->stride);
		return Error::BAD_VALUE;
	}

	/* The requested format must match. It may be possible for some formats to be compatible but there are no compelling
	 * use cases for a more complex check.
	 */
	int descriptor_format = static_cast<int>(descriptorInfo.format);
	if (gralloc_buffer->req_format != descriptor_format)
	{
		MALI_GRALLOC_LOGE("Buffer requested format: %#x does not match descriptor format: %#x",
		                  gralloc_buffer->req_format, descriptor_format);
		return Error::BAD_VALUE;
	}

	return Error::NONE;
}

void getTransportSize(void *buffer, IMapper::getTransportSize_cb hidl_cb)
{
	/* The buffer must have been allocated by Gralloc */
	buffer_handle_t bufferHandle = gRegisteredHandles->get(buffer);
	if (!bufferHandle)
	{
		MALI_GRALLOC_LOGE("Buffer %p is not registered with Gralloc", bufferHandle);
		hidl_cb(Error::BAD_BUFFER, -1, -1);
		return;
	}

	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer %p is corrupted", buffer);
		hidl_cb(Error::BAD_BUFFER, -1, -1);
		return;
	}
	hidl_cb(Error::NONE, bufferHandle->numFds, bufferHandle->numInts);
}

void isSupported(const IMapper::BufferDescriptorInfo &description, IMapper::isSupported_cb hidl_cb)
{
	buffer_descriptor_t grallocDescriptor;
	grallocDescriptor.width = description.width;
	grallocDescriptor.height = description.height;
	grallocDescriptor.layer_count = description.layerCount;
	grallocDescriptor.hal_format = static_cast<uint64_t>(description.format);
	grallocDescriptor.producer_usage = static_cast<uint64_t>(description.usage);
	grallocDescriptor.consumer_usage = grallocDescriptor.producer_usage;

	grallocDescriptor.flags |= DESCRIPTOR_ALLOCATOR_FLAGS;

	/* Check if it is possible to allocate a buffer for the given description */
	const int result = mali_gralloc_derive_format_and_size(&grallocDescriptor);
	if (result != 0)
	{
		MALI_GRALLOC_LOGV("Allocation for the given description will not succeed. error: %d", result);
	}
	hidl_cb(Error::NONE, result == 0);
}

void flushLockedBuffer(void *buffer, IMapper::flushLockedBuffer_cb hidl_cb)
{
	buffer_handle_t handle = gRegisteredHandles->get(buffer);
	if (private_handle_t::validate(handle) < 0)
	{
		MALI_GRALLOC_LOGE("Bandle: %p is corrupted", handle);
		hidl_cb(Error::BAD_BUFFER, hidl_handle{});
		return;
	}

	auto private_handle = static_cast<const private_handle_t *>(handle);
	if (private_handle->lock_count == 0)
	{
		MALI_GRALLOC_LOGE("Attempt to call flushLockedBuffer() on an unlocked buffer (%p)", handle);
		hidl_cb(Error::BAD_BUFFER, hidl_handle{});
		return;
	}

	allocator_sync_end(private_handle, false, true);
	hidl_cb(Error::NONE, hidl_handle{});
}

Error rereadLockedBuffer(void *buffer)
{
	buffer_handle_t handle = gRegisteredHandles->get(buffer);
	if (private_handle_t::validate(handle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", handle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = static_cast<const private_handle_t *>(handle);
	if (private_handle->lock_count == 0)
	{
		MALI_GRALLOC_LOGE("Attempt to call rereadLockedBuffer() on an unlocked buffer (%p)", handle);
		return Error::BAD_BUFFER;
	}

	allocator_sync_start(private_handle, true, false);
	return Error::NONE;
}

void get(void *buffer, const IMapper::MetadataType &metadataType, IMapper::get_cb hidl_cb)
{
	/* The buffer must have been allocated by Gralloc */
	const private_handle_t *handle = static_cast<const private_handle_t *>(gRegisteredHandles->get(buffer));
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		hidl_cb(Error::BAD_BUFFER, hidl_vec<uint8_t>());
		return;
	}
	get_metadata(handle, metadataType, hidl_cb);
}

Error set(void *buffer, const IMapper::MetadataType &metadataType, const hidl_vec<uint8_t> &metadata)
{
	/* The buffer must have been allocated by Gralloc */
	const private_handle_t *handle = static_cast<const private_handle_t *>(gRegisteredHandles->get(buffer));
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}
	return set_metadata(handle, metadataType, metadata);
}

void listSupportedMetadataTypes(IMapper::listSupportedMetadataTypes_cb hidl_cb)
{
	/* Returns a vector of {metadata type, description, isGettable, isSettable}
	 *  Only non-standardMetadataTypes require a description.
	 */
	hidl_vec<IMapper::MetadataTypeDescription> descriptions = {
		{ android::gralloc4::MetadataType_BufferId, "", true, false },
		{ android::gralloc4::MetadataType_Name, "", true, false },
		{ android::gralloc4::MetadataType_Width, "", true, false },
		{ android::gralloc4::MetadataType_Height, "", true, false },
		{ android::gralloc4::MetadataType_LayerCount, "", true, false },
		{ android::gralloc4::MetadataType_PixelFormatRequested, "", true, false },
		{ android::gralloc4::MetadataType_PixelFormatFourCC, "", true, false },
		{ android::gralloc4::MetadataType_PixelFormatModifier, "", true, false },
		{ android::gralloc4::MetadataType_Usage, "", true, false },
		{ android::gralloc4::MetadataType_AllocationSize, "", true, false },
		{ android::gralloc4::MetadataType_ProtectedContent, "", true, false },
		{ android::gralloc4::MetadataType_Compression, "", true, false },
		{ android::gralloc4::MetadataType_Interlaced, "", true, false },
		{ android::gralloc4::MetadataType_ChromaSiting, "", true, true },
		{ android::gralloc4::MetadataType_PlaneLayouts, "", true, false },
		{ android::gralloc4::MetadataType_Dataspace, "", true, true },
		{ android::gralloc4::MetadataType_BlendMode, "", true, true },
		{ android::gralloc4::MetadataType_Smpte2086, "", true, true },
		{ android::gralloc4::MetadataType_Cta861_3, "", true, true },
		{ android::gralloc4::MetadataType_Smpte2094_40, "", true, true },
		{ android::gralloc4::MetadataType_Crop, "", true, true },
		/* Arm vendor metadata */
		{ ArmMetadataType_PLANE_FDS, "Vector of file descriptors of each plane", true, false },
	};
	hidl_cb(Error::NONE, descriptions);
	return;
}

static hidl_vec<IMapper::MetadataDump> dumpBufferHelper(const private_handle_t *handle)
{
	hidl_vec<IMapper::MetadataType> standardMetadataTypes = {
		android::gralloc4::MetadataType_BufferId,
		android::gralloc4::MetadataType_Name,
		android::gralloc4::MetadataType_Width,
		android::gralloc4::MetadataType_Height,
		android::gralloc4::MetadataType_LayerCount,
		android::gralloc4::MetadataType_PixelFormatRequested,
		android::gralloc4::MetadataType_PixelFormatFourCC,
		android::gralloc4::MetadataType_PixelFormatModifier,
		android::gralloc4::MetadataType_Usage,
		android::gralloc4::MetadataType_AllocationSize,
		android::gralloc4::MetadataType_ProtectedContent,
		android::gralloc4::MetadataType_Compression,
		android::gralloc4::MetadataType_Interlaced,
		android::gralloc4::MetadataType_ChromaSiting,
		android::gralloc4::MetadataType_PlaneLayouts,
		android::gralloc4::MetadataType_Dataspace,
		android::gralloc4::MetadataType_BlendMode,
		android::gralloc4::MetadataType_Smpte2086,
		android::gralloc4::MetadataType_Cta861_3,
		android::gralloc4::MetadataType_Smpte2094_40,
		android::gralloc4::MetadataType_Crop,
	};

	std::vector<IMapper::MetadataDump> metadataDumps;
	for (const auto &metadataType : standardMetadataTypes)
	{
		get_metadata(handle, metadataType, [&metadataDumps, &metadataType](Error error, hidl_vec<uint8_t> metadata) {
			switch (error)
			{
			case Error::NONE:
				metadataDumps.push_back({ metadataType, metadata });
				break;
			case Error::UNSUPPORTED:
			default:
				return;
			}
		});
	}
	return hidl_vec<IMapper::MetadataDump>(metadataDumps);
}

void dumpBuffer(void *buffer, IMapper::dumpBuffer_cb hidl_cb)
{
	IMapper::BufferDump bufferDump{};
	auto handle = static_cast<const private_handle_t *>(gRegisteredHandles->get(buffer));
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		hidl_cb(Error::BAD_BUFFER, bufferDump);
		return;
	}

	bufferDump.metadataDump = dumpBufferHelper(handle);
	hidl_cb(Error::NONE, bufferDump);
}

void dumpBuffers(IMapper::dumpBuffers_cb hidl_cb)
{
	std::vector<IMapper::BufferDump> bufferDumps;
	gRegisteredHandles->for_each([&bufferDumps](buffer_handle_t buffer) {
		IMapper::BufferDump bufferDump{ dumpBufferHelper(static_cast<const private_handle_t *>(buffer)) };
		bufferDumps.push_back(bufferDump);
	});
	hidl_cb(Error::NONE, hidl_vec<IMapper::BufferDump>(bufferDumps));
}

void getReservedRegion(void *buffer, IMapper::getReservedRegion_cb hidl_cb)
{
	auto handle = static_cast<const private_handle_t *>(gRegisteredHandles->get(buffer));
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		hidl_cb(Error::BAD_BUFFER, 0, 0);
		return;
	}
	else if (handle->reserved_region_size == 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has no reserved region", buffer);
		hidl_cb(Error::BAD_BUFFER, 0, 0);
		return;
	}
	void *reserved_region = static_cast<std::byte *>(handle->attr_base) + mapper::common::shared_metadata_size();
	hidl_cb(Error::NONE, reserved_region, handle->reserved_region_size);
}

} // namespace common
} // namespace mapper
} // namespace arm
