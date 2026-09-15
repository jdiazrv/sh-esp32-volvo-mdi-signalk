#include "../src/capture_file_writer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

// Deliberately models a stale size() on every writing handle, even after
// flush(). Changes become visible only to a handle opened after close().
struct FakeFilesystem {
  struct File {
    FakeFilesystem* fs = nullptr;
    bool writable = false;
    bool closed = false;
    std::size_t cached_size = 0;
    std::string pending;

    explicit operator bool() const { return fs != nullptr && !closed; }
    std::size_t size() const { return cached_size; }

    std::size_t write(const std::uint8_t* bytes, std::size_t length) {
      assert(fs != nullptr && writable && !closed);
      ++fs->write_calls;
      const std::size_t accepted = length < fs->write_limit
                                       ? length
                                       : fs->write_limit;
      pending.assign(reinterpret_cast<const char*>(bytes), accepted);
      return fs->lie_about_count ? length : accepted;
    }

    void flush() {
      assert(fs != nullptr && !closed);
      ++fs->flush_calls;
      // Do not update cached_size or publish pending bytes here.
    }

    void close() {
      assert(fs != nullptr && !closed);
      if (writable) {
        if (!fs->discard_write) fs->contents += pending;
        fs->writer_is_open = false;
      }
      closed = true;
      ++fs->close_calls;
    }
  };

  std::string contents;
  bool fail_write_open = false;
  bool fail_read_open = false;
  bool discard_write = false;
  bool lie_about_count = false;
  bool writer_is_open = false;
  std::size_t write_limit = std::numeric_limits<std::size_t>::max();
  unsigned int open_calls = 0;
  unsigned int write_calls = 0;
  unsigned int flush_calls = 0;
  unsigned int close_calls = 0;

  File open(const char* path, const char* mode) {
    assert(std::string(path) == "/capture.csv");
    ++open_calls;
    const std::string requested_mode(mode);
    if (requested_mode == "r") {
      // Reopening before closing the writer must fail this test.
      assert(!writer_is_open);
      if (fail_read_open) return {};
      return File{this, false, false, contents.size(), {}};
    }
    assert(requested_mode == "a" || requested_mode == "w");
    assert(!writer_is_open);
    if (fail_write_open) return {};
    if (requested_mode == "w") contents.clear();
    writer_is_open = true;
    return File{this, true, false, contents.size(), {}};
  }
};

capture_file_writer::Result append(FakeFilesystem& fs, const std::string& row) {
  return capture_file_writer::write_verified(
      fs, "/capture.csv", "a",
      reinterpret_cast<const std::uint8_t*>(row.data()), row.size());
}

void append_works_with_stale_writer_size() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  const auto result = append(fs, "1,2\r\n");
  assert(result.ok());
  assert(result.requested_bytes == 5);
  assert(result.written_bytes == 5);
  assert(result.size_before == 8);
  assert(result.size_after == 13);
  assert(result.size_after_known);
  assert(fs.contents == "header\r\n1,2\r\n");
  assert(fs.open_calls == 2 && fs.write_calls == 1);
  assert(fs.flush_calls == 1 && fs.close_calls == 2);
}

void truncate_uses_new_empty_size() {
  FakeFilesystem fs;
  fs.contents = "old file\r\n";
  const std::string header = "new,header\r\n";
  const auto result = capture_file_writer::write_verified(
      fs, "/capture.csv", "w",
      reinterpret_cast<const std::uint8_t*>(header.data()), header.size());
  assert(result.ok());
  assert(result.size_before == 0);
  assert(result.size_after == header.size());
  assert(fs.contents == header);
}

void failed_open_does_not_write_or_verify() {
  FakeFilesystem fs;
  fs.contents = "original";
  fs.fail_write_open = true;
  const auto result = append(fs, "row\r\n");
  assert(result.status == capture_file_writer::Status::OpenFailed);
  assert(result.requested_bytes == 5 && result.written_bytes == 0);
  assert(!result.size_after_known);
  assert(fs.contents == "original");
  assert(fs.open_calls == 1 && fs.write_calls == 0 && fs.close_calls == 0);
}

void partial_write_is_reported_and_never_retried() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  fs.write_limit = 2;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::ShortWrite);
  assert(result.written_bytes == 2);
  assert(result.size_after_known && result.size_after == 10);
  assert(fs.contents == "header\r\n1,");
  assert(fs.write_calls == 1 && fs.open_calls == 2);
}

void zero_byte_write_is_short_write() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  fs.write_limit = 0;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::ShortWrite);
  assert(result.written_bytes == 0);
  assert(result.size_after_known && result.size_after == 8);
  assert(fs.contents == "header\r\n");
  assert(fs.write_calls == 1);
}

void reopen_failure_is_ambiguous_not_retried() {
  FakeFilesystem fs;
  fs.fail_read_open = true;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::VerifyFailed);
  assert(result.written_bytes == 5 && !result.size_after_known);
  assert(fs.contents == "1,2\r\n");
  assert(fs.write_calls == 1 && fs.close_calls == 1);
}

void short_write_takes_precedence_over_failed_reopen() {
  FakeFilesystem fs;
  fs.write_limit = 2;
  fs.fail_read_open = true;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::ShortWrite);
  assert(result.written_bytes == 2 && !result.size_after_known);
  assert(fs.contents == "1,");
  assert(fs.write_calls == 1);
}

void reported_success_with_wrong_length_is_verification_failure() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  fs.write_limit = 2;
  fs.lie_about_count = true;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::VerifyFailed);
  assert(result.written_bytes == 5);
  assert(result.size_after_known && result.size_after == 10);
  assert(fs.write_calls == 1);
}

void lost_write_is_verification_failure() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  fs.discard_write = true;
  const auto result = append(fs, "1,2\r\n");
  assert(result.status == capture_file_writer::Status::VerifyFailed);
  assert(result.written_bytes == 5);
  assert(result.size_after_known && result.size_after == 8);
  assert(fs.write_calls == 1);
}

void empty_write_keeps_existing_file() {
  FakeFilesystem fs;
  fs.contents = "header\r\n";
  const auto result = append(fs, "");
  assert(result.ok());
  assert(result.written_bytes == 0 && result.size_after == 8);
  assert(fs.contents == "header\r\n");
}

}  // namespace

int main() {
  append_works_with_stale_writer_size();
  truncate_uses_new_empty_size();
  failed_open_does_not_write_or_verify();
  partial_write_is_reported_and_never_retried();
  zero_byte_write_is_short_write();
  reopen_failure_is_ambiguous_not_retried();
  short_write_takes_precedence_over_failed_reopen();
  reported_success_with_wrong_length_is_verification_failure();
  lost_write_is_verification_failure();
  empty_write_keeps_existing_file();
  std::cout << "capture_file_writer: 10 tests passed\n";
}
