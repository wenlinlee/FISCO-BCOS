/**
 *  Copyright (C) 2024 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @file Common.h
 * @author: kyonGuo
 * @date 2024/3/29
 */

#pragma once

#include <bcos-rpc/Common.h>
#include <string_view>

namespace bcos::rpc
{
constexpr const uint64_t LowestGasPrice{21000};
constexpr const std::string_view EarliestBlock{"earliest"};
constexpr const std::string_view LatestBlock{"latest"};
constexpr const std::string_view PendingBlock{"pending"};
constexpr const std::string_view SafeBlock{"safe"};
constexpr const std::string_view FinalizedBlock{"finalized"};
}  // namespace bcos::rpc