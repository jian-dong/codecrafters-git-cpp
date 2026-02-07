#include "git-cpp/git_pack.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <zlib.h>

#include "git-cpp/utils.hpp"

namespace gitcpp {
namespace {

constexpr std::size_t kPackHeaderLength = 12;
constexpr std::size_t kPackObjectHashLength = 20;

struct ParsedPackObject {
  std::string hash;
  std::string raw_object;
  std::size_t next_position;
};

GitExpected<std::string> ExtractPackStream(const std::string& response) {
  const std::size_t pack_pos = response.find("PACK");
  if (pack_pos == std::string::npos) {
    return UnexpectedError(GitErrorCode::kPackSignatureMissing,
                           "PACK signature not found in upload-pack response",
                           "extract pack stream");
  }
  return response.substr(pack_pos);
}

GitExpected<int> ParseObjectCount(const std::string& pack_data) {
  if (pack_data.size() < kPackHeaderLength || pack_data.substr(0, 4) != "PACK") {
    return UnexpectedError(GitErrorCode::kPackHeaderInvalid, "Invalid pack header",
                           "parse pack object count");
  }

  int object_count = 0;
  for (int i = 8; i < 12; ++i) {
    object_count = (object_count << 8) |
                   static_cast<unsigned char>(pack_data[static_cast<std::size_t>(i)]);
  }
  return object_count;
}

GitExpected<int> ExtractObjectType(const std::string& pack_data,
                                   std::size_t position) {
  if (position >= pack_data.size()) {
    return UnexpectedError(GitErrorCode::kPackStreamTruncated,
                           "Unexpected end of pack stream",
                           "extract pack object type");
  }
  return (static_cast<unsigned char>(pack_data[position]) & 0x70) >> 4;
}

GitExpected<std::uint64_t> ReadPackObjectSize(const std::string& pack_data,
                                              std::size_t* position) {
  if (position == nullptr || *position >= pack_data.size()) {
    return UnexpectedError(GitErrorCode::kInvalidArgument,
                           "Invalid pack stream position",
                           "read pack object size");
  }

  unsigned char byte = static_cast<unsigned char>(pack_data[*position]);
  std::uint64_t size = byte & 0x0F;
  int shift = 4;
  ++(*position);

  while ((byte & 0x80U) != 0U) {
    if (*position >= pack_data.size()) {
      return UnexpectedError(GitErrorCode::kPackObjectLengthInvalid,
                             "Malformed object length in pack stream",
                             "read pack object size");
    }
    byte = static_cast<unsigned char>(pack_data[*position]);
    size |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
    shift += 7;
    ++(*position);
  }

  return size;
}

GitExpected<std::uint64_t> ReadDeltaVarInt(const std::string& input,
                                           std::size_t* position) {
  if (position == nullptr) {
    return UnexpectedError(GitErrorCode::kInvalidArgument,
                           "Invalid delta position", "read delta varint");
  }

  std::uint64_t value = 0;
  int shift = 0;
  while (true) {
    if (*position >= input.size()) {
      return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                             "Unexpected end of delta stream",
                             "read delta varint");
    }
    const unsigned char byte = static_cast<unsigned char>(input[*position]);
    ++(*position);

    value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
    if ((byte & 0x80U) == 0U) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                             "Delta varint is too large",
                             "read delta varint");
    }
  }
  return value;
}

GitExpected<std::pair<std::string, std::size_t>> DecompressWithConsumedInput(
    std::string_view compressed_data) {
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) {
    return UnexpectedError(GitErrorCode::kCompressionError, "inflateInit failed",
                           "decompress pack object");
  }

  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed_data.data()));
  stream.avail_in = static_cast<uInt>(compressed_data.size());

  constexpr std::size_t kBufferSize = 16 * 1024;
  std::array<char, kBufferSize> output_buffer{};
  std::string decompressed_data;

  while (true) {
    stream.next_out = reinterpret_cast<Bytef*>(output_buffer.data());
    stream.avail_out = static_cast<uInt>(output_buffer.size());
    const int status = inflate(&stream, Z_NO_FLUSH);

    if (status != Z_OK && status != Z_STREAM_END) {
      (void)inflateEnd(&stream);
      return UnexpectedError(GitErrorCode::kCompressionError,
                             "inflate failed while parsing pack object",
                             "decompress pack object");
    }

    const std::size_t produced_size = output_buffer.size() - stream.avail_out;
    decompressed_data.append(output_buffer.data(), produced_size);

    if (status == Z_STREAM_END) {
      const std::size_t consumed_size = stream.total_in;
      if (inflateEnd(&stream) != Z_OK) {
        return UnexpectedError(GitErrorCode::kCompressionError,
                               "inflateEnd failed",
                               "decompress pack object");
      }
      return std::make_pair(decompressed_data, consumed_size);
    }
  }
}

std::string BuildRawObject(std::string_view object_type,
                           std::string_view object_body) {
  return std::string(object_type) + " " + std::to_string(object_body.size()) + '\0' +
         std::string(object_body);
}

GitExpected<std::string> ExtractBody(std::string_view raw_object) {
  const std::size_t null_pos = raw_object.find('\0');
  if (null_pos == std::string_view::npos) {
    return UnexpectedError(GitErrorCode::kObjectFormatInvalid, "Invalid raw object format",
                           "extract pack object body");
  }
  return std::string(raw_object.substr(null_pos + 1));
}

GitExpected<std::string> ExtractType(std::string_view raw_object) {
  const std::size_t space_pos = raw_object.find(' ');
  if (space_pos == std::string_view::npos) {
    return UnexpectedError(GitErrorCode::kObjectFormatInvalid, "Invalid raw object header",
                           "extract pack object type");
  }
  return std::string(raw_object.substr(0, space_pos));
}

GitExpected<std::string> ApplyDelta(const std::string& delta_data,
                                    const std::string& base_data) {
  std::size_t pos = 0;
  auto base_size = ReadDeltaVarInt(delta_data, &pos);
  if (!base_size) {
    return UnexpectedError(base_size.error());
  }
  auto result_size = ReadDeltaVarInt(delta_data, &pos);
  if (!result_size) {
    return UnexpectedError(result_size.error());
  }

  if (*base_size != base_data.size()) {
    return UnexpectedError(GitErrorCode::kPackDeltaInvalid, "Delta base size mismatch",
                           "apply pack delta");
  }

  std::string result;
  result.reserve(static_cast<std::size_t>(*result_size));

  while (pos < delta_data.size()) {
    const unsigned char opcode = static_cast<unsigned char>(delta_data[pos]);
    ++pos;

    if ((opcode & 0x80U) != 0U) {
      std::size_t copy_offset = 0;
      std::size_t copy_size = 0;

      if ((opcode & 0x01U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy offset", "apply pack delta");
        }
        copy_offset |= static_cast<unsigned char>(delta_data[pos++]);
      }
      if ((opcode & 0x02U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy offset", "apply pack delta");
        }
        copy_offset |= static_cast<std::size_t>(static_cast<unsigned char>(delta_data[pos++])) << 8;
      }
      if ((opcode & 0x04U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy offset", "apply pack delta");
        }
        copy_offset |= static_cast<std::size_t>(static_cast<unsigned char>(delta_data[pos++])) << 16;
      }
      if ((opcode & 0x08U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy offset", "apply pack delta");
        }
        copy_offset |= static_cast<std::size_t>(static_cast<unsigned char>(delta_data[pos++])) << 24;
      }

      if ((opcode & 0x10U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy size", "apply pack delta");
        }
        copy_size |= static_cast<unsigned char>(delta_data[pos++]);
      }
      if ((opcode & 0x20U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy size", "apply pack delta");
        }
        copy_size |= static_cast<std::size_t>(static_cast<unsigned char>(delta_data[pos++])) << 8;
      }
      if ((opcode & 0x40U) != 0U) {
        if (pos >= delta_data.size()) {
          return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                                 "Invalid delta copy size", "apply pack delta");
        }
        copy_size |= static_cast<std::size_t>(static_cast<unsigned char>(delta_data[pos++])) << 16;
      }

      if (copy_size == 0) {
        copy_size = 0x10000;
      }
      if (copy_offset + copy_size > base_data.size()) {
        return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                               "Delta copy exceeds base object bounds",
                               "apply pack delta");
      }

      result.append(base_data, copy_offset, copy_size);
      continue;
    }

    if (opcode == 0) {
      return UnexpectedError(GitErrorCode::kPackDeltaInvalid, "Invalid delta opcode",
                             "apply pack delta");
    }

    const std::size_t insert_size = opcode & 0x7FU;
    if (pos + insert_size > delta_data.size()) {
      return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                             "Delta insert exceeds input bounds",
                             "apply pack delta");
    }
    result.append(delta_data, pos, insert_size);
    pos += insert_size;
  }

  if (result.size() != *result_size) {
    return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                           "Delta output size mismatch", "apply pack delta");
  }
  return result;
}

GitExpected<ParsedPackObject> ProcessReferenceDelta(
    const std::string& pack_data, std::size_t position,
    const GitObjectStore& object_store) {
  if (position + kPackObjectHashLength > pack_data.size()) {
    return UnexpectedError(GitErrorCode::kPackDeltaInvalid,
                           "Malformed ref-delta object",
                           "process reference delta");
  }

  const std::string base_hash =
      HashToHex(std::string_view(pack_data).substr(position, kPackObjectHashLength));
  position += kPackObjectHashLength;

  auto base_raw = object_store.ReadRawObject(base_hash);
  if (!base_raw) {
    return UnexpectedError(base_raw.error());
  }
  auto base_type = ExtractType(*base_raw);
  if (!base_type) {
    return UnexpectedError(base_type.error());
  }
  auto base_body = ExtractBody(*base_raw);
  if (!base_body) {
    return UnexpectedError(base_body.error());
  }

  auto delta_payload = DecompressWithConsumedInput(
      std::string_view(pack_data).substr(position));
  if (!delta_payload) {
    return UnexpectedError(delta_payload.error());
  }
  position += delta_payload->second;

  auto reconstructed_body = ApplyDelta(delta_payload->first, *base_body);
  if (!reconstructed_body) {
    return UnexpectedError(reconstructed_body.error());
  }

  ParsedPackObject parsed;
  parsed.raw_object = BuildRawObject(*base_type, *reconstructed_body);
  parsed.hash = ComputeSha1(parsed.raw_object);
  parsed.next_position = position;
  return parsed;
}

GitExpected<ParsedPackObject> ProcessRegularObject(const std::string& pack_data,
                                                   std::size_t position,
                                                   int object_type) {
  auto payload = DecompressWithConsumedInput(std::string_view(pack_data).substr(position));
  if (!payload) {
    return UnexpectedError(payload.error());
  }
  position += payload->second;

  std::string type_name = "blob";
  if (object_type == 1) {
    type_name = "commit";
  } else if (object_type == 2) {
    type_name = "tree";
  } else if (object_type == 4) {
    type_name = "tag";
  }

  ParsedPackObject parsed;
  parsed.raw_object = BuildRawObject(type_name, payload->first);
  parsed.hash = ComputeSha1(parsed.raw_object);
  parsed.next_position = position;
  return parsed;
}

}  // namespace

GitExpected<std::string> GitPackProcessor::UnpackAndStore(
    const std::string& upload_pack_response, const std::string& head_hash,
    const GitObjectStore& object_store) const {
  auto pack_data = ExtractPackStream(upload_pack_response);
  if (!pack_data) {
    return UnexpectedError(pack_data.error());
  }

  auto object_count = ParseObjectCount(*pack_data);
  if (!object_count) {
    return UnexpectedError(object_count.error());
  }

  std::size_t position = kPackHeaderLength;
  std::string head_commit_body;

  for (int index = 0; index < *object_count; ++index) {
    auto object_type = ExtractObjectType(*pack_data, position);
    if (!object_type) {
      return UnexpectedError(object_type.error());
    }

    auto object_size = ReadPackObjectSize(*pack_data, &position);
    if (!object_size) {
      return UnexpectedError(object_size.error());
    }
    (void)object_size;

    GitExpected<ParsedPackObject> parsed = [&]() -> GitExpected<ParsedPackObject> {
      if (*object_type == 6) {
        return UnexpectedError(GitErrorCode::kPackDeltaUnsupported,
                               "Offset deltas are not implemented",
                               "unpack and store");
      }
      if (*object_type == 7) {
        return ProcessReferenceDelta(*pack_data, position, object_store);
      }
      return ProcessRegularObject(*pack_data, position, *object_type);
    }();
    if (!parsed) {
      return UnexpectedError(parsed.error());
    }
    position = parsed->next_position;

    auto store_status = object_store.StoreRawObject(parsed->hash, parsed->raw_object);
    if (!store_status) {
      return UnexpectedError(store_status.error());
    }

    if (parsed->hash == head_hash) {
      auto body = ExtractBody(parsed->raw_object);
      if (!body) {
        return UnexpectedError(body.error());
      }
      head_commit_body = *body;
    }
  }

  if (head_commit_body.empty()) {
    return UnexpectedError(GitErrorCode::kPackHeadNotFound,
                           "Failed to find HEAD commit in pack stream",
                           "unpack and store", head_hash);
  }
  return head_commit_body;
}

}  // namespace gitcpp
