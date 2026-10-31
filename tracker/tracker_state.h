
#ifndef TRACKER_STATE_H
#define TRACKER_STATE_H

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <cstdint>

struct User {
    std::string user_id;
    std::string password;
    uint64_t created_at_ms = 0; // used only to resolve a create_user naming conflict on merge
};

struct FileInfo {
    std::string filename;
    uint64_t file_size = 0;
    std::string file_hash;                   // whole-file SHA1, 40 hex chars (see common/sha1.h)
    std::vector<std::string> piece_hashes;    
    std::string uploader_id;
    std::string uploader_address;             
    uint64_t uploaded_at_ms = 0;            

    std::vector<std::string> seeder_addresses;
};

struct Group {
    std::string group_id;
    std::string owner_id;
    std::string creator_id; // immutable identity of the group creation; owner_id may change later
    uint64_t created_at_ms = 0; // used only to resolve a create_group naming conflict on merge

    std::map<std::string, uint64_t> member_add_seq;
    std::map<std::string, uint64_t> member_remove_seq;
    std::map<std::string, uint64_t> pending_add_seq;
    std::map<std::string, uint64_t> pending_remove_seq;
    uint64_t next_seq = 1; // local clock, stamped onto whichever counter above just changed

    std::map<std::string, FileInfo> files; // keyed by filename, spec 4.2

    bool is_member(const std::string& user_id) const;
    void recompute_owner();
    bool is_pending(const std::string& user_id) const;
    std::set<std::string> member_set() const;
    std::set<std::string> pending_set() const;
    size_t member_count() const;
};


class TrackerState {
public:
    bool create_user(const std::string& user_id, const std::string& password,
                      bool is_replay, std::string& error_out);

    bool check_login(const std::string& user_id, const std::string& password, std::string& error_out);

    bool create_group(const std::string& group_id, const std::string& owner_id,
                       bool is_replay, std::string& error_out,
                       uint64_t created_at_ms = 0);

    bool join_group(const std::string& group_id, const std::string& user_id,
                     bool is_replay, std::string& error_out);

    bool leave_group(const std::string& group_id, const std::string& user_id,
                      bool is_replay, std::string& error_out);

    bool accept_request(const std::string& group_id, const std::string& owner_id,
                         const std::string& target_user_id, bool is_replay, std::string& error_out);

    std::string list_groups();
    uint64_t get_group_created_at(const std::string& group_id);

    bool list_requests(const std::string& group_id, const std::string& requester_id,
                        std::string& out_or_error);

    bool upload_file(const std::string& group_id, const std::string& filename,
                      const std::string& uploader_id, const std::string& uploader_address,
                      uint64_t file_size, const std::string& file_hash,
                      const std::string& piece_hashes_csv,
                      bool is_replay, std::string& error_out,
                      uint64_t uploaded_at_ms = 0);

    uint64_t get_file_uploaded_at(const std::string& group_id, const std::string& filename);

    bool list_files(const std::string& group_id, std::string& out_or_error);


    bool get_file_info(const std::string& group_id, const std::string& filename,
                        std::string& out_or_error);


    bool add_seeder(const std::string& group_id, const std::string& filename,
                     const std::string& address, bool is_replay, std::string& error_out);
    bool remove_seeder(const std::string& group_id, const std::string& filename,
                        const std::string& address, bool is_replay, std::string& error_out);

    std::string serialize_snapshot(); // dumps entire state as text, one record per line
    void merge_snapshot(const std::string& snapshot); // merges a peer's dump into our own state

private:
    std::mutex mtx;
    std::map<std::string, User> users;
    std::map<std::string, Group> groups;
};

#endif // TRACKER_STATE_H
