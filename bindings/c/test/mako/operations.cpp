/*
 * operations.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "blob_granules.hpp"
#include "operations.hpp"
#include "mako.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <array>

extern thread_local mako::Logger logr;

namespace mako {

using namespace fdb;

const std::array<Operation, MAX_OP> opTable{
	{ { "GRV",
	    { { StepKind::READ,
	        [](Transaction& tx, Arguments const&, ByteString&, ByteString&, ByteString&) {
	            return tx.getReadVersion().eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString&) {
	            if (f && !f.error()) {
		            f.get<future_var::Int64>();
	            }
	        } } },
	    1,
	    false },
	  { "GET",
	    { { StepKind::READ,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString&) {
	            return tx.get(key, false /*snapshot*/).eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString& val) {
	            if (f && !f.error()) {
		            f.get<future_var::Value>();
	            }
	        } } },
	    1,
	    false },
	  { "GETRANGE",
	    { { StepKind::READ,
	        [](Transaction& tx, Arguments const& args, ByteString& begin, ByteString& end, ByteString&) {
	            return tx
	                .getRange(key_select::firstGreaterOrEqual(begin),
	                          key_select::lastLessOrEqual(end, 1),
	                          0 /*limit*/,
	                          0 /*target_bytes*/,
	                          args.streaming_mode,
	                          0 /*iteration*/,
	                          false /*snapshot*/,
	                          args.txnspec.ops[OP_GETRANGE][OP_REVERSE])
	                .eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString& val) {
	            if (f && !f.error()) {
		            f.get<future_var::KeyValueArray>();
	            }
	        } } },
	    1,
	    false },
	  { "SGET",
	    { { StepKind::READ,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString&) {
	            return tx.get(key, true /*snapshot*/).eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString& val) {
	            if (f && !f.error()) {
		            f.get<future_var::Value>();
	            }
	        } } },
	    1,
	    false },
	  { "SGETRANGE",
	    { {

	        StepKind::READ,
	        [](Transaction& tx, Arguments const& args, ByteString& begin, ByteString& end, ByteString&) {
	            return tx
	                .getRange(key_select::firstGreaterOrEqual(begin),
	                          key_select::lastLessOrEqual(end, 1),
	                          0 /*limit*/,
	                          0 /*target_bytes*/,
	                          args.streaming_mode,
	                          0 /*iteration*/,
	                          true /*snapshot*/,
	                          args.txnspec.ops[OP_GETRANGE][OP_REVERSE])
	                .eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString& val) {
	            if (f && !f.error()) {
		            f.get<future_var::KeyValueArray>();
	            }
	        } } },
	    1,
	    false },
	  { "UPDATE",
	    { { StepKind::READ,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString&) {
	            return tx.get(key, false /*snapshot*/).eraseType();
	        },
	        [](Future& f, Transaction&, Arguments const&, ByteString&, ByteString&, ByteString& val) {
	            if (f && !f.error()) {
		            f.get<future_var::Value>();
	            }
	        } },
	      { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString& value) {
	            randomString(value.data(), args.value_length);
	            tx.set(key, value);
	            return Future();
	        } } },
	    2,
	    true },
	  { "INSERT",
	    { { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString& value) {
	            // key[0..args.key_length] := concat(key_prefix, random_string)
	            randomString(key.data() + intSize(KEY_PREFIX), args.key_length - intSize(KEY_PREFIX));
	            randomString(value.data(), args.value_length);
	            tx.set(key, value);
	            return Future();
	        } } },
	    1,
	    true },
	  { "INSERTRANGE",
	    { { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString& value) {
	            randomString(value.data(), args.value_length);

	            // key[0..args.key_length] := concat(prefix, random_string, num[0..range_digits])
	            const auto range = args.txnspec.ops[OP_INSERTRANGE][OP_RANGE];
	            assert(range > 0);
	            const auto range_digits = digits(range);
	            const auto random_len = args.key_length - intSize(KEY_PREFIX) - range_digits;
	            randomString(&key[intSize(KEY_PREFIX)], random_len);
	            for (auto i = 0; i < range; i++) {
		            numericWithFill(&key[args.key_length - range_digits], range_digits, i);
		            tx.set(key, value);
	            }
	            return Future();
	        } } },
	    1,
	    true },
	  { "OVERWRITE",
	    { { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString& value) {
	            randomString(value.data(), args.value_length);
	            tx.set(key, value);
	            return Future();
	        } } },
	    1,
	    true },
	  { "CLEAR",
	    { { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString&) {
	            tx.clear(key);
	            return Future();
	        } } },
	    1,
	    true },
	  { "SETCLEAR",
	    { { StepKind::COMMIT,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString& value) {
	            randomString(&key[KEY_PREFIX.size()], args.key_length - intSize(KEY_PREFIX));
	            randomString(value.data(), args.value_length);
	            tx.set(key, value);
	            return tx.commit().eraseType();
	        } },
	      { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& key, ByteString&, ByteString&) {
	            tx.reset(); // assuming commit from step 0 worked.
	            tx.clear(key); // key should forward unchanged from step 0
	            return Future();
	        } } },
	    2,
	    true },
	  { "CLEARRANGE",
	    { { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& begin, ByteString& end, ByteString&) {
	            tx.clearRange(begin, end);
	            return Future();
	        } } },
	    1,
	    true },
	  { "SETCLEARRANGE",
	    { { StepKind::COMMIT,
	        [](Transaction& tx, Arguments const& args, ByteString& key_begin, ByteString& key, ByteString& value) {
	            randomString(value.data(), args.value_length);

	            // key[0..args.key_length] := concat(prefix, random_string, num[0..range_digits])
	            const auto range = args.txnspec.ops[OP_SETCLEARRANGE][OP_RANGE];
	            assert(range > 0);
	            const auto range_digits = digits(range);
	            const auto random_len = args.key_length - intSize(KEY_PREFIX) - range_digits;
	            randomString(&key[KEY_PREFIX.size()], random_len);
	            for (auto i = 0; i < range; i++) {
		            numericWithFill(&key[args.key_length - range_digits], range_digits, i);
		            tx.set(key, value);
		            if (i == 0)
			            key_begin.assign(key);
	            }
	            return tx.commit().eraseType();
	        } },
	      { StepKind::IMM,
	        [](Transaction& tx, Arguments const& args, ByteString& begin, ByteString& end, ByteString&) {
	            tx.reset();
	            tx.clearRange(begin, end);
	            return Future();
	        } } },
	    2,
	    true },
	  { "COMMIT", { { StepKind::NONE, nullptr } }, 0, false },
	  { "TRANSACTION", { { StepKind::NONE, nullptr } }, 0, false },
	  { "READBLOBGRANULE",
	    { { StepKind::ON_ERROR,
	        [](Transaction& tx, Arguments const& args, ByteString& begin, ByteString& end, ByteString&) {
	            auto err = Error{};

	            err = tx.setOptionNothrow(FDB_TR_OPTION_READ_YOUR_WRITES_DISABLE, BytesRef());
	            if (err) {
		            // Issuing read/writes before disabling RYW results in error.
		            // Possible malformed workload?
		            // As workloads execute in sequence, retrying would likely repeat this error.
		            fmt::print(stderr, "ERROR: TR_OPTION_READ_YOUR_WRITES_DISABLE: {}", err.what());
		            return Future();
	            }

	            // Allocate a separate context per call to avoid multiple threads accessing
	            auto user_context = blob_granules::local_file::UserContext(args.bg_file_path);

	            auto api_context = blob_granules::local_file::createApiContext(user_context, args.bg_materialize_files);

	            auto r = tx.readBlobGranules(begin,
	                                         end,
	                                         0 /* beginVersion*/,
	                                         -2, /* endVersion. -2 (latestVersion) is use txn read version */
	                                         api_context);

	            user_context.clear();

	            auto out = Result::KeyValueArray{};
	            err = r.getKeyValueArrayNothrow(out);
	            if (!err || err.is(2037 /*blob_granule_not_materialized*/))
		            return Future();
	            const auto level = (err.is(1020 /*not_committed*/) || err.is(1021 /*commit_unknown_result*/) ||
	                                err.is(1213 /*tag_throttled*/))
	                                   ? VERBOSE_WARN
	                                   : VERBOSE_NONE;
	            logr.printWithLogLevel(level, "ERROR", "get_keyvalue_array() after readBlobGranules(): {}", err.what());
	            return tx.onError(err).eraseType();
	        } } },
	    1,
	    false } }
};

} // namespace mako
