/*
 * Copyright (C) 2022 Arm Limited.
 * SPDX-License-Identifier: Apache-2.0
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

#include "allocator.h"
#include "idl_common/descriptor.h"
#include "idl_common/allocator.h"

#include <aidlcommonsupport/NativeHandle.h>
#include <aidl/android/hardware/graphics/allocator/AllocationError.h>

namespace aidl::android::hardware::graphics::allocator::impl::arm
{

using ::android::hardware::hidl_vec;

ndk::ScopedAStatus allocator::allocate(const std::vector<uint8_t> &in_descriptor, int32_t in_count,
                                       AllocationResult *out_result)
{
	buffer_descriptor_t buffer_descriptor;
	hidl_vec<uint8_t> hidl_descriptor(in_descriptor);
	if (!::arm::mapper::common::grallocDecodeBufferDescriptor(hidl_descriptor, buffer_descriptor))
	{
		return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(AllocationError::BAD_DESCRIPTOR));
	}

	buffer_descriptor.flags |= GPU_DATA_BUFFER_WITH_ANY_FORMAT | SUPPORTS_R8 | USE_AIDL_FRONTBUFFER_USAGE;
	CHECK_EQ(buffer_descriptor.flags, ::arm::mapper::common::DESCRIPTOR_ALLOCATOR_FLAGS);

	auto result = ::arm::allocator::common::allocate(&buffer_descriptor, in_count);

	switch (result.first)
	{
	case ::android::NO_ERROR:
		break;
	case ::android::NO_MEMORY:
		return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(AllocationError::NO_RESOURCES));
	case ::android::BAD_VALUE:
		return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(AllocationError::UNSUPPORTED));
	default:
		MALI_GRALLOC_LOGE("Unknown allocation error %d\n", result.first);
		return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(AllocationError::UNSUPPORTED));
	}

	CHECK_EQ(in_count, result.second.size());

	out_result->stride = buffer_descriptor.pixel_stride;
	out_result->buffers.reserve(in_count);
	/* Pass ownership when returning the created handles. */
	for (const auto &native_handle : result.second)
	{
		out_result->buffers.emplace_back(::android::makeToAidl(native_handle));
	}

	return ndk::ScopedAStatus::ok();
}

} // namespace aidl::android::hardware::graphics::allocator::impl::arm
