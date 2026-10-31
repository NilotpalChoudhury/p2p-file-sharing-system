#ifndef FILE_PIECES_H
#define FILE_PIECES_H

#include <cstdint>
#include <string>
#include <vector>


constexpr uint64_t PIECE_SIZE = 512ULL * 1024ULL; // 512 KB

struct FilePieceInfo {
    uint64_t file_size = 0;
    std::vector<std::string> piece_hashes; 
    std::string file_hash;                
};


bool compute_file_pieces(const std::string& path, FilePieceInfo& out, std::string& error_out);

// Number of pieces a file of this size will be split into (0-byte files count as 0 pieces).
uint64_t piece_count_for_size(uint64_t file_size);


std::string join_piece_hashes(const std::vector<std::string>& piece_hashes);
std::vector<std::string> split_piece_hashes(const std::string& csv);

#endif // FILE_PIECES_H
