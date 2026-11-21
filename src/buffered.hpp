#pragma once
#include "input.hpp"
#include <cerrno>
#include <deque>
#include <system_error>

namespace shop {

struct FilePiece {
    const fs::path* path;
    size_t offset, length, file_size;
};

class CsvPieces {
    std::deque<fs::path> paths_;
public:
    std::vector<FilePiece> chunks;
    size_t bytes = 0;

    explicit CsvPieces(const fs::path& dir) {
        constexpr size_t target = 512 * 1024;
        for_csv_file(dir, [&](const fs::path& path) {
            size_t size = fs::file_size(path);
            bytes += size;
            const auto* name = &paths_.emplace_back(path);
            for (size_t offset = 0; offset < size; offset += target)
                chunks.push_back({name, offset, std::min(target, size - offset), size});
        });
    }
};

// A parser owns its read buffer. It keeps one open file across adjacent jobs.
// Each piece includes the end of the record intersecting its right boundary.
class PieceReader {
    const fs::path* path_ = nullptr;
    int fd_ = -1;
    std::vector<char> data_;

    void fill(size_t offset, size_t first, size_t limit) {
        if (data_.size() < limit) data_.resize(limit);
        while (first < limit) {
            ssize_t read = pread(fd_, data_.data() + first, std::min(limit - first, size_t{8} << 20),
                                 static_cast<off_t>(offset + first));
            if (read > 0) first += size_t(read);
            else if (read == 0) throw std::runtime_error("CSV shortened while reading " + path_->string());
            else if (errno != EINTR) throw std::system_error(errno, std::generic_category(), path_->string());
        }
    }
public:
    PieceReader() = default;
    PieceReader(const PieceReader&) = delete;
    PieceReader& operator=(const PieceReader&) = delete;
    ~PieceReader() { if (fd_ >= 0) close(fd_); }

    Chunk read(const FilePiece& piece) {
        if (path_ != piece.path) {
            if (fd_ >= 0) close(fd_);
            fd_ = open(piece.path->c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0) throw std::system_error(errno, std::generic_category(), piece.path->string());
            path_ = piece.path;
        }
        const size_t available = piece.file_size - piece.offset;
        size_t loaded = piece.length + std::min(size_t{512}, available - piece.length);
        fill(piece.offset, 0, loaded);
        size_t scan = piece.length;
        while (loaded < available && !memchr(data_.data() + scan, '\n', loaded - scan)) {
            // A piece entirely inside a long record owns no rows.
            if (scan == piece.length && !memchr(data_.data(), '\n', piece.length)) break;
            scan = loaded;
            loaded += std::min(loaded, available - loaded);
            fill(piece.offset, scan, loaded);
        }
        return {data_.data(), data_.data() + piece.length, data_.data() + loaded};
    }
};
} // namespace shop
