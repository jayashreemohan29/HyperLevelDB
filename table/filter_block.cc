// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "hyperleveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

FileLevelFilterBuilder::FileLevelFilterBuilder(const FilterPolicy* policy) {
//	printf("Before assigning policy..\n");
//	Env::Default()->SleepForMicroseconds(5000000);
	this->policy_ = policy;
//	printf("After assigning policy..\n");
//	Env::Default()->SleepForMicroseconds(5000000);
//	printf("keys_ size: %d, key_offsets_ size: %d key_offsets_ capacity: %d tmp_keys_ size: %d tmp_keys_ capacity: %d\n", keys_.size(), key_offsets_.size(), key_offsets_.capacity(), tmp_keys_.size(), tmp_keys_.capacity());
	Clear();
//	keys_.init_buffer(SIZE_FOR_FILTER_KEYS);
//	printf("End of constructor..\n");
//	Env::Default()->SleepForMicroseconds(5000000);

//	printf("After clearing..\nkeys_ size: %d, key_offsets_ size: %d key_offsets_ capacity: %d tmp_keys_ size: %d tmp_keys_ capacity: %d\n", keys_.size(), key_offsets_.size(), key_offsets_.capacity(), tmp_keys_.size(), tmp_keys_.capacity());
}

FileLevelFilterBuilder::~FileLevelFilterBuilder() {
	Destroy();
}

void FileLevelFilterBuilder::Destroy() {
//	printf("Destroying memory for file level filter builder..\n");
	std::vector<Slice>().swap(tmp_keys_);
	std::vector<size_t>().swap(key_offsets_);
	keys_.destroy_memory();
}

void FileLevelFilterBuilder::Clear() {
	keys_.clear();
	key_offsets_.clear();
//	keys_vec_.clear();
//	std::vector<size_t>().swap(key_offsets_);
	tmp_keys_.clear();
//	std::vector<Slice>().swap(tmp_keys_);
}

void FileLevelFilterBuilder::AddKey(const Slice& key) {
	Slice k = key;
	key_offsets_.push_back(keys_.size());
	keys_.append(k.data(), k.size());
}

std::string* FileLevelFilterBuilder::GenerateFilter() {
//	printf("GenerateFilter:: tmp_keys size: %d key_offsets size: %d result size: %d keys_ size: %d\n", tmp_keys_.size(), key_offsets_.size(), 0, keys_.size());
//	Clear();
//	return NULL;

	std::string* result = new std::string;
	const size_t num_keys = key_offsets_.size();
	if (num_keys == 0) {
	  delete result;
	  Clear();
	  return NULL;
	}

	// Make list of keys from flattened key structure
	key_offsets_.push_back(keys_.size());  // Simplify length computation
	tmp_keys_.resize(num_keys);
	for (size_t i = 0; i < num_keys; i++) {
	  const char* base = keys_.data() + key_offsets_[i];
	  size_t length = key_offsets_[i+1] - key_offsets_[i];
	  tmp_keys_[i] = Slice(base, length);
	}

	// Generate filter for current set of keys and append to result_.
//	printf("Before generating filter - result pointer: %p\n", (void*) result);
	policy_->CreateFilter(&tmp_keys_[0], num_keys, result);
//	printf("GenerateFilter:: tmp_keys size: %d key_offsets size: %d result size: %d keys_ size: %d\n", tmp_keys_.size(), key_offsets_.size(), result->length(), keys_.size());

	Clear();
	return result;
}

// See doc/table_format.txt for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy),
      keys_(),
      start_(),
      result_(),
      tmp_keys_(),
      filter_offsets_() {
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i+1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  policy_->CreateFilter(&tmp_keys_[0], num_keys, &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy),
      data_(NULL),
      offset_(NULL),
      num_(0),
      base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n-1];
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index*4);
    uint32_t limit = DecodeFixed32(offset_ + index*4 + 4);
    if (start <= limit && limit <= (offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}
