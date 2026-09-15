#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// A small, host-testable adapter for the Arduino FS/File interface. In some
// SPIFFS implementations File::size() on the writing handle is cached. Always
// close that handle and inspect a freshly opened one before judging the write.
namespace capture_file_writer {

enum class Status {
  Ok,
  OpenFailed,
  ShortWrite,
  VerifyFailed,
};

struct Result {
  Status status = Status::OpenFailed;
  std::size_t requested_bytes = 0;
  // Bytes reported by write(), not a claim about durability after power loss.
  std::size_t written_bytes = 0;
  std::size_t size_before = 0;
  std::size_t size_after = 0;
  bool size_after_known = false;

  bool ok() const { return status == Status::Ok; }
};

// `mode` must be the filesystem's truncate/write or append mode (Arduino "w"
// or "a"). Calls affecting this path must be serialized by the caller.
//
// Exactly one write is attempted. In particular, a partial or ambiguous write
// is never retried: appending the complete CSV row again could duplicate an
// event or concatenate it with an incomplete row. The caller must stop writing
// to, or safely rotate/repair, such a file before adding another row. This
// helper neither removes previous data nor classifies I/O errors as "full".
//
// Verification checks the reported byte count and file length, not content or
// resistance to power loss during a flash operation. A successful flush/close
// followed by reopen cannot guarantee atomicity of an entire CSV row.
template <typename FileSystem>
Result write_verified(FileSystem& filesystem, const char* path,
                      const char* mode, const std::uint8_t* data,
                      std::size_t length) {
  Result result;
  result.requested_bytes = length;
  auto file = filesystem.open(path, mode);
  if (!file) return result;

  // This size is obtained before any writes on the newly opened handle. In
  // truncate mode the filesystem has already made it zero.
  result.size_before = file.size();
  result.written_bytes = file.write(data, length);
  file.flush();
  file.close();

  // Read even after a short write so diagnostics can report the actual file
  // length. Do not consult size() on the closed/cached writing handle.
  auto verification = filesystem.open(path, "r");
  if (verification) {
    result.size_after = verification.size();
    result.size_after_known = true;
    verification.close();
  }

  if (result.written_bytes != length) {
    result.status = Status::ShortWrite;
  } else if (!result.size_after_known ||
             length > std::numeric_limits<std::size_t>::max() -
                          result.size_before ||
             result.size_after != result.size_before + length) {
    result.status = Status::VerifyFailed;
  } else {
    result.status = Status::Ok;
  }
  return result;
}

}  // namespace capture_file_writer
