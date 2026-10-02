#pragma once
#include "nitro/model.hpp"
#include <filesystem>
namespace nitro {
enum class ExportFormat : std::uint8_t { trace, csv };
// Exclusive creation protects existing recordings. Each sample is streamed to bound memory.
class Recorder {
  public:
    Recorder(const std::filesystem::path& path, ExportFormat format, Clock::time_point start);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    void append(const Snapshot& snapshot);
    void finish();

  private:
    int fd_{-1};
    ExportFormat format_;
    Clock::time_point start_;
    bool first_{true};
    std::map<std::string, std::size_t> tracks_;
    void write(std::string_view text);
};
[[nodiscard]] std::string snapshot_json(const Snapshot& snapshot);
[[nodiscard]] std::string recording_filename();
} // namespace nitro
