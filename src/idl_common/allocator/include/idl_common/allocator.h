/*
 * Copyright (C) 2020, 2022 ARM Limited. All rights reserved.
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
#pragma once

#include <utility>
#include <vector>

#include <utils/Errors.h>

#include "core/buffer_descriptor.h"

namespace arm
{
namespace allocator
{
namespace common
{

/**
 * Allocates buffers with the properties specified by the descriptor
 *
 * @param[in, out]  descriptor  Specifies the properties of the buffers to allocate
 * @param           count       Number of buffers to allocate.
 *
 * @return Pair of:
 *              1. Error code:
 *                  NONE upon success. Otherwise,
 *                  NO_MEMORY when the allocation cannot be fulfilled
 *                  BAD_VALUE when any of the property encoded in the descriptor is not supported
 *              2. Array of raw handles to newly allocated buffers.
 */
std::pair<android::status_t, std::vector<const native_handle_t *>> allocate(buffer_descriptor_t *descriptor,
                                                                            uint32_t count);

} // namespace common
} // namespace allocator
} // namespace arm
