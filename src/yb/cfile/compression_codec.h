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
#ifndef YB_CFILE_COMPRESSION_CODEC_H
#define YB_CFILE_COMPRESSION_CODEC_H

#include <string>
#include <vector>

#include "yb/cfile/cfile.pb.h"
#include "yb/gutil/macros.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"

namespace yb {
namespace cfile {

class CompressionCodec {
 public:
  CompressionCodec();
  virtual ~CompressionCodec();

  // REQUIRES: "compressed" must point to an area of memory that is at
  // least "MaxCompressedLength(input_length)" bytes in length.
  //
  // Takes the data stored in "input[0..input_length]" and stores
  // it in the array pointed to by "compressed".
  //
  // returns the length of the compressed output.
  virtual CHECKED_STATUS Compress(const Slice& input,
                          uint8_t *compressed, size_t *compressed_length) const = 0;

  virtual CHECKED_STATUS Compress(const std::vector<Slice>& input_slices,
                          uint8_t *compressed, size_t *compressed_length) const = 0;

  // Given data in "compressed[0..compressed_length-1]" generated by
  // calling the Compress routine, this routine stores the uncompressed data
  // to uncompressed[0..uncompressed_length-1]
  // returns false if the message is corrupted and could not be uncompressed
  virtual CHECKED_STATUS Uncompress(const Slice& compressed,
                            uint8_t *uncompressed, size_t uncompressed_length) const = 0;

  // Returns the maximal size of the compressed representation of
  // input data that is "source_bytes" bytes in length.
  virtual size_t MaxCompressedLength(size_t source_bytes) const = 0;
 private:
  DISALLOW_COPY_AND_ASSIGN(CompressionCodec);
};

// Returns the compression codec for the specified type.
//
// The returned codec is a singleton and should be not be destroyed.
Status GetCompressionCodec(CompressionType compression,
                           const CompressionCodec** codec);

// Returns the compression codec type given the name
CompressionType GetCompressionCodecType(const std::string& name);

} // namespace cfile
} // namespace yb
#endif
