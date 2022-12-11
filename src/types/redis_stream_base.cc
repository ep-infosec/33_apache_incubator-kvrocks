/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "redis_stream_base.h"

#include "encoding.h"
#include "parse_util.h"
#include "time_util.h"

namespace Redis {

const char *kErrLastEntryIdReached = "last possible entry id reached";
const char *kErrInvalidEntryIdSpecified = "Invalid stream ID specified as stream command argument";
const char *kErrDecodingStreamEntryValueFailure = "failed to decode stream entry value";

rocksdb::Status IncrementStreamEntryID(StreamEntryID *id) {
  if (id->seq == UINT64_MAX) {
    if (id->ms == UINT64_MAX) {
      // special case where 'id' is the last possible entry ID
      id->ms = 0;
      id->seq = 0;
      return rocksdb::Status::InvalidArgument(kErrLastEntryIdReached);
    } else {
      id->ms++;
      id->seq = 0;
    }
  } else {
    id->seq++;
  }

  return rocksdb::Status::OK();
}

rocksdb::Status GetNextStreamEntryID(const StreamEntryID &last_id, StreamEntryID *new_id) {
  uint64_t ms = Util::GetTimeStampMS();
  if (ms > last_id.ms) {
    new_id->ms = ms;
    new_id->seq = 0;
    return rocksdb::Status::OK();
  } else {
    *new_id = last_id;
    return IncrementStreamEntryID(new_id);
  }
}

Status ParseStreamEntryID(const std::string &input, StreamEntryID *id) {
  auto pos = input.find('-');
  if (pos != std::string::npos) {
    auto ms_str = input.substr(0, pos);
    auto seq_str = input.substr(pos + 1);
    auto parse_ms = ParseInt<uint64_t>(ms_str, 10);
    auto parse_seq = ParseInt<uint64_t>(seq_str, 10);
    if (!parse_ms || !parse_seq) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_ms;
    id->seq = *parse_seq;
  } else {
    auto parse_input = ParseInt<uint64_t>(input, 10);
    if (!parse_input) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_input;
    id->seq = 0;
  }
  return Status::OK();
}

Status ParseNewStreamEntryID(const std::string &input, NewStreamEntryID *id) {
  auto pos = input.find('-');
  if (pos != std::string::npos) {
    auto ms_str = input.substr(0, pos);
    auto seq_str = input.substr(pos + 1);
    auto parse_ms = ParseInt<uint64_t>(ms_str, 10);
    if (!parse_ms) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_ms;

    if (seq_str == "*") {
      id->any_seq_number = true;
    } else {
      auto parse_seq = ParseInt<uint64_t>(seq_str, 10);
      if (!parse_seq) {
        return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
      }
      id->seq = *parse_seq;
    }
  } else {
    auto parse_input = ParseInt<uint64_t>(input, 10);
    if (!parse_input) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_input;
    id->seq = 0;
  }

  return Status::OK();
}

Status ParseRangeStart(const std::string &input, StreamEntryID *id) { return ParseStreamEntryID(input, id); }

Status ParseRangeEnd(const std::string &input, StreamEntryID *id) {
  auto pos = input.find('-');
  if (pos != std::string::npos) {
    auto ms_str = input.substr(0, pos);
    auto seq_str = input.substr(pos + 1);
    auto parse_ms = ParseInt<uint64_t>(ms_str, 10);
    auto parse_seq = ParseInt<uint64_t>(seq_str, 10);
    if (!parse_ms || !parse_seq) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_ms;
    id->seq = *parse_seq;
  } else {
    auto parse_input = ParseInt<uint64_t>(input, 10);
    if (!parse_input) {
      return Status(Status::RedisParseErr, kErrInvalidEntryIdSpecified);
    }
    id->ms = *parse_input;
    id->seq = UINT64_MAX;
  }

  return Status::OK();
}

std::string EncodeStreamEntryValue(const std::vector<std::string> &args) {
  std::string dst;
  for (auto const &v : args) {
    PutVarint32(&dst, v.size());
    dst.append(v);
  }
  return dst;
}

Status DecodeRawStreamEntryValue(const std::string &value, std::vector<std::string> *result) {
  result->clear();
  rocksdb::Slice s(value);

  while (!s.empty()) {
    uint32_t len = 0;
    if (!GetVarint32(&s, &len)) {
      return Status(Status::RedisParseErr, kErrDecodingStreamEntryValueFailure);
    }
    result->emplace_back(s.data(), len);
    s.remove_prefix(len);
  }

  return Status::OK();
}

}  // namespace Redis
