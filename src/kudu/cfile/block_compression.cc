// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <boost/foreach.hpp>
#include <glog/logging.h>
#include <algorithm>

#include "kudu/cfile/block_compression.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/util/coding.h"
#include "kudu/util/coding-inl.h"

namespace kudu {
namespace cfile {

using std::vector;

CompressedBlockBuilder::CompressedBlockBuilder(const CompressionCodec* codec,
                                               size_t size_limit)
  : codec_(DCHECK_NOTNULL(codec)),
    compressed_size_limit_(size_limit) {
}

Status CompressedBlockBuilder::Compress(const Slice& data, Slice *result) {
  vector<Slice> v;
  v.push_back(data);
  return Compress(v, result);
}

Status CompressedBlockBuilder::Compress(const vector<Slice> &data_slices, Slice *result) {
  size_t data_size = 0;
  BOOST_FOREACH(const Slice& data, data_slices) {
    data_size += data.size();
  }

  // Ensure that the buffer for header + compressed data is large enough
  size_t max_compressed_size = codec_->MaxCompressedLength(data_size);
  if (max_compressed_size > compressed_size_limit_) {
    return Status::InvalidArgument(
      StringPrintf("estimated max size %lu is greater than the expected %lu",
        max_compressed_size, compressed_size_limit_));
  }

  buffer_.resize(kHeaderReservedLength + max_compressed_size);

  // Compress
  size_t compressed_size;
  RETURN_NOT_OK(codec_->Compress(data_slices,
                                 buffer_.data() + kHeaderReservedLength, &compressed_size));

  // Set up the header
  InlineEncodeFixed32(&buffer_[0], compressed_size);
  InlineEncodeFixed32(&buffer_[4], data_size);
  *result = Slice(buffer_.data(), compressed_size + kHeaderReservedLength);

  return Status::OK();
}

CompressedBlockDecoder::CompressedBlockDecoder(const CompressionCodec* codec,
                                               size_t size_limit)
  : codec_(DCHECK_NOTNULL(codec)),
    uncompressed_size_limit_(size_limit) {
}

Status CompressedBlockDecoder::Uncompress(const Slice& data, Slice *result) {
  // Check if the on-disk data is large enough to hold the header
  if (data.size() < CompressedBlockBuilder::kHeaderReservedLength) {
    return Status::Corruption(
      StringPrintf("data size %lu is not enough to contains the header. required %lu, buffer",
        data.size(), CompressedBlockBuilder::kHeaderReservedLength),
        data.ToDebugString(50));
  }

  // Decode the header
  uint32_t compressed_size = DecodeFixed32(data.data());
  uint32_t uncompressed_size = DecodeFixed32(data.data() + 4);

  // Check if the on-disk data size matches with the buffer
  if (data.size() != (CompressedBlockBuilder::kHeaderReservedLength + compressed_size)) {
    return Status::Corruption(
      StringPrintf("compressed size %u does not match remaining length in buffer %lu, buffer",
        compressed_size, data.size() - CompressedBlockBuilder::kHeaderReservedLength),
        data.ToDebugString(50));
  }

  // Check if uncompressed size seems to be reasonable
  if (uncompressed_size > uncompressed_size_limit_) {
    return Status::Corruption(
      StringPrintf("uncompressed size %u overflows the maximum length %lu, buffer",
        compressed_size, uncompressed_size_limit_), data.ToDebugString(50));
  }

  Slice compressed(data.data() + CompressedBlockBuilder::kHeaderReservedLength, compressed_size);

  // Allocate the buffer for the uncompressed data and uncompress
  ::gscoped_array<uint8_t> buffer(new uint8_t[uncompressed_size]);
  RETURN_NOT_OK(codec_->Uncompress(compressed, buffer.get(), uncompressed_size));
  *result = Slice(buffer.release(), uncompressed_size);

  return Status::OK();
}

} // namespace cfile
} // namespace kudu