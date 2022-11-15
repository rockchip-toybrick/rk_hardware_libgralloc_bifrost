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

#include "../custom_log.h"
#include <inttypes.h>

#include "idl_common/allocator.h"
#include "idl_common/shared_metadata.h"

#include "core/buffer_allocation.h"
#include "core/buffer_descriptor.h"
#include "core/format_info.h"
#include "allocator/shared_memory/shared_memory.h"

namespace arm
{
namespace allocator
{
namespace common
{

using aidl::android::hardware::graphics::common::ExtendableType;

/* Get the default chroma siting to use based on the format. */
static void get_format_default_chroma_siting(internal_format_t format, ExtendableType *chroma_siting)
{
	*chroma_siting = android::gralloc4::ChromaSiting_Unknown;
	const auto *format_info = format.get_base_info();
	if (format_info == nullptr)
	{
		return;
	}

	if (format_info->is_yuv)
	{
		/* Default chroma siting values based on format */
		switch (format.get_base())
		{
		case MALI_GRALLOC_FORMAT_INTERNAL_NV12:
		case MALI_GRALLOC_FORMAT_INTERNAL_NV15:
		case MALI_GRALLOC_FORMAT_INTERNAL_NV21:
		case MALI_GRALLOC_FORMAT_INTERNAL_P010:
		case MALI_GRALLOC_FORMAT_INTERNAL_YUV420_8BIT_I:
		case MALI_GRALLOC_FORMAT_INTERNAL_YUV420_10BIT_I:
		case MALI_GRALLOC_FORMAT_INTERNAL_Y0L2:
			*chroma_siting = android::gralloc4::ChromaSiting_SitedInterstitial;
			break;
		case MALI_GRALLOC_FORMAT_INTERNAL_Y210:
		case MALI_GRALLOC_FORMAT_INTERNAL_P210:
			*chroma_siting = android::gralloc4::ChromaSiting_CositedHorizontal;
			break;
		case MALI_GRALLOC_FORMAT_INTERNAL_NV16:
		case MALI_GRALLOC_FORMAT_INTERNAL_Y410:
		case MALI_GRALLOC_FORMAT_INTERNAL_YUV444:
		case MALI_GRALLOC_FORMAT_INTERNAL_Q410:
		case MALI_GRALLOC_FORMAT_INTERNAL_Q401:
		case MALI_GRALLOC_FORMAT_INTERNAL_YUV422_8BIT:
			*chroma_siting = arm::mapper::common::ChromaSiting_CositedBoth;
			break;
		default:
			MALI_GRALLOC_LOG(WARNING) << "No default Chroma Siting found for format " << format;
		}
	}
	else if (format_info->is_rgb)
	{
		*chroma_siting = android::gralloc4::ChromaSiting_None;
	}
}

std::pair<android::status_t, std::vector<const native_handle_t *>> allocate(buffer_descriptor_t *buffer_descriptor,
                                                                            uint32_t count)
{
	android::status_t error = android::NO_ERROR;
	int stride = 0;
	std::vector<const native_handle_t *> gralloc_buffers;

	gralloc_buffers.reserve(count);

	for (uint32_t i = 0; i < count; i++)
	{
		private_handle_t *hnd = nullptr;
		if (mali_gralloc_buffer_allocate(buffer_descriptor, &hnd) != 0)
		{
			MALI_GRALLOC_LOGE("%s, buffer allocation failed with %d", __func__, errno);
			error = android::NO_MEMORY;
			break;
		}

		hnd->reserved_region_size = buffer_descriptor->reserved_size;
		hnd->attr_size = mapper::common::shared_metadata_size() + hnd->reserved_region_size;
		std::tie(hnd->share_attr_fd, hnd->attr_base) =
		    gralloc_shared_memory_allocate("gralloc_shared_memory", hnd->attr_size);
		if (hnd->share_attr_fd < 0 || hnd->attr_base == MAP_FAILED)
		{
			MALI_GRALLOC_LOGE("%s, shared memory allocation failed with errno %d", __func__, errno);
			mali_gralloc_buffer_free(hnd);
			native_handle_delete(hnd);
			error = android::BAD_VALUE;
			break;
		}

		mapper::common::shared_metadata_init(hnd->attr_base, buffer_descriptor->name);
		const auto internal_format = buffer_descriptor->alloc_format;
		const uint64_t usage = buffer_descriptor->consumer_usage | buffer_descriptor->producer_usage;
		android_dataspace_t dataspace;
		const auto *format_info = internal_format.get_base_info();
		get_format_dataspace(format_info, usage, hnd->width, hnd->height, &dataspace);

		ExtendableType chroma_siting;
		get_format_default_chroma_siting(internal_format, &chroma_siting);

		mapper::common::set_dataspace(hnd, static_cast<mapper::common::Dataspace>(dataspace));
		mapper::common::set_chroma_siting(hnd, chroma_siting);

		/*
		 * We need to set attr_base to MAP_FAILED before passing it to the client process to avoid sending an
		 * invalid pointer.
		 */
		munmap(hnd->attr_base, hnd->attr_size);
		hnd->attr_base = MAP_FAILED;

#ifdef ENABLE_DEBUG_LOG
        {
            buffer_descriptor_t* bufDescriptor = buffer_descriptor;
			const auto internal_format = hnd->get_alloc_format();
			const auto alloc_format = internal_format.get_base();

            ALOGD("got new private_handle_t instance @%p for buffer '%s'. share_fd : %d, share_attr_fd : %d, "
                "width : %d, height : %d, "
                "req_format : 0x%x, producer_usage : 0x%" PRIx64 ", consumer_usage : 0x%" PRIx64 ", "
                ", stride : %d, "
                "alloc_format : %d, size : %d, layer_count : %u",
                hnd, (bufDescriptor->name).c_str() == nullptr ? "unset" : (bufDescriptor->name).c_str(),
              hnd->share_fd, hnd->share_attr_fd, hnd->width, hnd->height,
              hnd->req_format, hnd->producer_usage, hnd->consumer_usage,
              hnd->stride,
              alloc_format, hnd->size, hnd->layer_count);
            ALOGD("plane_info[0]: offset : %u, byte_stride : %u, alloc_width : %u, alloc_height : %u",
                    (hnd->plane_info)[0].offset,
                    (hnd->plane_info)[0].byte_stride,
                    (hnd->plane_info)[0].alloc_width,
                    (hnd->plane_info)[0].alloc_height);
            ALOGD("plane_info[1]: offset : %u, byte_stride : %u, alloc_width : %u, alloc_height : %u",
                    (hnd->plane_info)[1].offset,
                    (hnd->plane_info)[1].byte_stride,
                    (hnd->plane_info)[1].alloc_width,
                    (hnd->plane_info)[1].alloc_height);
        }
#endif

		int tmp_stride = buffer_descriptor->pixel_stride;

		if (stride == 0)
		{
			stride = tmp_stride;
		}
		else if (stride != tmp_stride)
		{
			/* Stride must be the same for all allocations */
			mali_gralloc_buffer_free(hnd);
			native_handle_delete(hnd);
			stride = 0;
			error = android::BAD_VALUE;
			break;
		}
		gralloc_buffers.emplace_back(hnd);
	}

	if (error != android::NO_ERROR)
	{
		for (auto &buffer : gralloc_buffers)
		{
			mali_gralloc_buffer_free(private_handle_t::downcast(buffer));
			native_handle_delete(const_cast<native_handle_t *>(buffer));
		}
		gralloc_buffers.clear();
	}

	return std::make_pair(error, gralloc_buffers);
}

} // namespace common
} // namespace allocator
} // namespace arm
