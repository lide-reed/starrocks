// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/http/action/checksum_action.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef STARROCKS_BE_SRC_HTTP_CHECKSUM_ACTION_H
#define STARROCKS_BE_SRC_HTTP_CHECKSUM_ACTION_H

#include <cstdint>

#include "http/http_handler.h"

namespace starrocks {

class ExecEnv;

class ChecksumAction : public HttpHandler {
public:
    explicit ChecksumAction(ExecEnv* exec_env);

    ~ChecksumAction() override = default;

    void handle(HttpRequest* req) override;

private:
    std::int64_t do_checksum(std::int64_t tablet_id, std::int64_t version, std::int32_t schema_hash, HttpRequest* req);

    ExecEnv* _exec_env;

}; // end class ChecksumAction

} // end namespace starrocks
#endif // STARROCKS_BE_SRC_COMMON_UTIL_DOWNLOAD_ACTION_H
