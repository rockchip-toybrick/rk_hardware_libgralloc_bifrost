/*
 * Copyright (C) 2019 Arm Limited.
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

#ifndef ARM_GRAPHICS_PRIVATEBUFFER_V1_0_ACCESSOR_H
#define ARM_GRAPHICS_PRIVATEBUFFER_V1_0_ACCESSOR_H

#include <arm/graphics/privatebuffer/1.0/IAttributeAccessor.h>
#include <arm/graphics/privatebuffer/1.0/IAccessor.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace arm {
namespace graphics {
namespace privatebuffer {
namespace V1_0 {
namespace implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_bitfield;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct Accessor : public IAccessor {
    // Methods from ::arm::graphics::privatebuffer::V1_0::IAccessor follow.
    Return<void> getAllocation(const hidl_handle& bufferHandle, getAllocation_cb _hidl_cb) override;
    Return<void> getAllocatedFormat(const hidl_handle& buffer_handle, getAllocatedFormat_cb _hidl_cb) override;
    Return<void> getRequestedDimensions(const hidl_handle& bufferHandle, getRequestedDimensions_cb _hidl_cb) override;
    Return<void> getRequestedFormat(const hidl_handle& bufferHandle, getRequestedFormat_cb _hidl_cb) override;
    Return<void> getUsage(const hidl_handle& bufferHandle, getUsage_cb _hidl_cb) override;
    Return<void> getLayerCount(const hidl_handle& bufferHandle, getLayerCount_cb _hidl_cb) override;
    Return<void> getPlaneLayout(const hidl_handle& bufferHandle, getPlaneLayout_cb _hidl_cb) override;
    Return<void> getAttributeAccessor(const hidl_handle& bufferHandle, getAttributeAccessor_cb _hidl_cb) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.
};

extern "C" IAccessor* HIDL_FETCH_IAccessor(const char* name);

}  // namespace implementation
}  // namespace V1_0
}  // namespace privatebuffer
}  // namespace graphics
}  // namespace arm

#endif  // ARM_GRAPHICS_PRIVATEBUFFER_V1_0_ACCESSOR_H
