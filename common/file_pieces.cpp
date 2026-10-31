#include "file_pieces.h"
#include "sha1.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

using namespace std;

uint64_t piece_count_for_size(uint64_t file_size) {
    if (file_size == 0) return 0;
    return (file_size + PIECE_SIZE - 1) / PIECE_SIZE; // ceiling division
}

bool compute_file_pieces(const string& path, FilePieceInfo& out, string& error_out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error_out = string("could not open '") + path + "': " + strerror(errno);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        error_out = string("fstat failed on '") + path + "': " + strerror(errno);
        close(fd);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error_out = "'" + path + "' is not a regular file";
        close(fd);
        return false;
    }

    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint8_t* buf = new uint8_t[PIECE_SIZE]; // one piece-sized buffer, reused for every piece
    Sha1 whole_file_hasher;
    vector<string> piece_hashes;
    bool ok = true;

    uint64_t remaining = file_size;
    while (remaining > 0) {
        size_t want = static_cast<size_t>(remaining < PIECE_SIZE ? remaining : PIECE_SIZE);

        // A single read() call is not guaranteed to return `want` bytes even when they're
        // all available -- loop until we have them, the same way recv_all() does for sockets.
        size_t got = 0;
        while (got < want) {
            ssize_t n = read(fd, buf + got, want - got);
            if (n < 0) {
                if (errno == EINTR) continue;
                error_out = string("read failed on '") + path + "': " + strerror(errno);
                ok = false;
                break;
            }
            if (n == 0) break; // EOF earlier than fstat() promised -- file changed underneath us
            got += static_cast<size_t>(n);
        }
        if (!ok) break;
        if (got != want) {
            error_out = "unexpected end of file while reading '" + path + "' (did it change size during read?)";
            ok = false;
            break;
        }

        piece_hashes.push_back(Sha1::hex(buf, got));
        whole_file_hasher.update(buf, got);
        remaining -= got;
    }

    delete[] buf;
    close(fd);
    if (!ok) return false;

    out.file_size = file_size;
    out.piece_hashes = piece_hashes;
    out.file_hash = whole_file_hasher.final_hex();
    return true;
}

std::string join_piece_hashes(const vector<string>& piece_hashes) {
    if (piece_hashes.empty()) return "-";
    string out;
    for (size_t i = 0; i < piece_hashes.size(); i++) {
        if (i > 0) out += ",";
        out += piece_hashes[i];
    }
    return out;
}

vector<string> split_piece_hashes(const string& csv) {
    vector<string> out;
    if (csv.empty() || csv == "-") return out;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        if (comma == string::npos) {
            out.push_back(csv.substr(start));
            break;
        }
        out.push_back(csv.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}
