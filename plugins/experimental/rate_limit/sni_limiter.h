/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "limiter.h"
#include "ts/ts.h"

int sni_limit_cont(TSCont contp, TSEvent event, void *edata);

///////////////////////////////////////////////////////////////////////////////
// SNI based limiters, for global (pligin.config) instance(s).
//
class SniRateLimiter : public RateLimiter<TSVConn>
{
public:
  SniRateLimiter() {}

  SniRateLimiter(const SniRateLimiter &src)
  {
    limit     = src.limit;
    max_queue = src.max_queue;
    max_age   = src.max_age;
    prefix    = src.prefix;
    tag       = src.tag;
  }

  bool initialize(int argc, const char *argv[]);
};
