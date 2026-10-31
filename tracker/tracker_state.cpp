#include "tracker_state.h"
#include "../common/file_pieces.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <chrono>

using namespace std;


static string join_addr_csv(const vector<string>& addrs);
static vector<string> split_addr_csv(const string& s);

static uint64_t current_millis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// A user counts as "in" iff their most recent add is more recent than their most recent remove.
static uint64_t seq_of(const map<string, uint64_t>& m, const string& user_id) {
    auto it = m.find(user_id);
    return it == m.end() ? 0 : it->second;
}

bool Group::is_member(const string& user_id) const {
    return seq_of(member_add_seq, user_id) > seq_of(member_remove_seq, user_id);
}

bool Group::is_pending(const string& user_id) const {
    return seq_of(pending_add_seq, user_id) > seq_of(pending_remove_seq, user_id);
}

void Group::recompute_owner() {
    string chosen;
    uint64_t chosen_seq = UINT64_MAX;
    for (const auto& [user_id, seq] : member_add_seq) {
        if (!is_member(user_id)) continue;
        if (seq < chosen_seq || (seq == chosen_seq && (chosen.empty() || user_id < chosen))) {
            chosen_seq = seq;
            chosen = user_id;
        }
    }
    owner_id = chosen;
}

set<string> Group::member_set() const {
    set<string> out;
    for (const auto& [user_id, seq] : member_add_seq) {
        (void)seq;
        if (is_member(user_id)) out.insert(user_id);
    }
    return out;
}

set<string> Group::pending_set() const {
    set<string> out;
    for (const auto& [user_id, seq] : pending_add_seq) {
        (void)seq;
        if (is_pending(user_id)) out.insert(user_id);
    }
    return out;
}

size_t Group::member_count() const {
    size_t count = 0;
    for (const auto& [user_id, seq] : member_add_seq) {
        (void)seq;
        if (is_member(user_id)) count++;
    }
    return count;
}

bool TrackerState::create_user(const string& user_id, const string& password,
                                bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    if (users.count(user_id)) {
        if (is_replay) return true; // already applied
        error_out = "user '" + user_id + "' already exists";
        return false;
    }
    users[user_id] = User{user_id, password, current_millis()};
    return true;
}

bool TrackerState::check_login(const string& user_id, const string& password, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = users.find(user_id);
    if (it == users.end()) {
        error_out = "no such user '" + user_id + "'";
        return false;
    }
    if (it->second.password != password) {
        error_out = "incorrect password";
        return false;
    }
    return true;
}

bool TrackerState::create_group(const string& group_id, const string& owner_id,
                                 bool is_replay, string& error_out,
                                 uint64_t created_at_ms) {
    lock_guard<mutex> lock(mtx);
    if (groups.count(group_id)) {
        if (is_replay) return true;
        error_out = "group '" + group_id + "' already exists";
        return false;
    }
    Group g;
    g.group_id = group_id;
    g.owner_id = owner_id;
    g.creator_id = owner_id;
    g.created_at_ms = (created_at_ms != 0) ? created_at_ms : current_millis();
    g.member_add_seq[owner_id] = g.next_seq++; // creator is automatically a member
    groups[group_id] = g;
    return true;
}

bool TrackerState::join_group(const string& group_id, const string& user_id,
                               bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true;
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    Group& g = it->second;
    if (g.is_member(user_id)) {
        if (is_replay) return true;
        error_out = "you are already a member of '" + group_id + "'";
        return false;
    }
    if (g.is_pending(user_id)) {
        if (is_replay) return true;
        error_out = "you already have a pending request for '" + group_id + "'";
        return false;
    }
    g.pending_add_seq[user_id] = g.next_seq++;
    return true;
}

bool TrackerState::leave_group(const string& group_id, const string& user_id,
                                bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true;
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    Group& g = it->second;
    if (!g.is_member(user_id)) {
        if (is_replay) return true;
        error_out = "you are not a member of '" + group_id + "'";
        return false;
    }
    const bool was_owner = (g.owner_id == user_id);
    if (!is_replay && was_owner && g.member_count() == 1) {
        
        error_out = "owner cannot leave because they are the last member";
        return false;
    }
    g.member_remove_seq[user_id] = g.next_seq++;
    if (was_owner) {
    
        g.recompute_owner();
    }
    return true;
}

bool TrackerState::accept_request(const string& group_id, const string& owner_id,
                                   const string& target_user_id, bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true;
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    Group& g = it->second;
    if (!is_replay && g.owner_id != owner_id) { // no owner_id to check on replay -- see apply_sync_op
        error_out = "only the owner of '" + group_id + "' can accept join requests";
        return false;
    }
    if (!g.is_pending(target_user_id)) {
        if (is_replay) return true;
        error_out = "no pending request from '" + target_user_id + "' for '" + group_id + "'";
        return false;
    }
    g.pending_remove_seq[target_user_id] = g.next_seq++;
    g.member_add_seq[target_user_id] = g.next_seq++;
    return true;
}

bool TrackerState::upload_file(const string& group_id, const string& filename,
                                const string& uploader_id, const string& uploader_address,
                                uint64_t file_size, const string& file_hash,
                                const string& piece_hashes_csv,
                                bool is_replay, string& error_out,
                                uint64_t uploaded_at_ms) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true; // nothing to attach the file to locally yet; a later snapshot heals this
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    Group& g = it->second;
    if (!is_replay && !g.is_member(uploader_id)) {
      
        error_out = "you must be a member of '" + group_id + "' to share a file there";
        return false;
    }
    if (g.files.count(filename)) {
        FileInfo& existing = g.files[filename];
        if (existing.file_hash == file_hash) {
          
            if (find(existing.seeder_addresses.begin(), existing.seeder_addresses.end(), uploader_address)
                == existing.seeder_addresses.end()) {
                existing.seeder_addresses.push_back(uploader_address);
            }
            return true;
        }
        if (is_replay) return true; // genuine conflict on replay -- left to merge_snapshot's
                                     // earliest-wins tie-break, same as before
        error_out = "file '" + filename + "' already exists in group '" + group_id + "' with different content";
        return false;
    }
    FileInfo f;
    f.filename = filename;
    f.file_size = file_size;
    f.file_hash = file_hash;
    f.piece_hashes = split_piece_hashes(piece_hashes_csv);
    f.uploader_id = uploader_id;
    f.uploader_address = uploader_address;
    f.uploaded_at_ms = (uploaded_at_ms != 0) ? uploaded_at_ms : current_millis();
    f.seeder_addresses.push_back(uploader_address); // Day 4: the uploader is the first seeder
    g.files[filename] = f;
    return true;
}

bool TrackerState::add_seeder(const string& group_id, const string& filename,
                               const string& address, bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true; // self-heals via the next snapshot, same as upload_file
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    auto fit = it->second.files.find(filename);
    if (fit == it->second.files.end()) {
        if (is_replay) return true;
        error_out = "no such file '" + filename + "' in group '" + group_id + "'";
        return false;
    }
    auto& addrs = fit->second.seeder_addresses;
    if (find(addrs.begin(), addrs.end(), address) == addrs.end()) {
        addrs.push_back(address);
    }
    // Already present is a harmless no-op either way -- re-registering yourself isn't an error.
    return true;
}

bool TrackerState::remove_seeder(const string& group_id, const string& filename,
                                  const string& address, bool is_replay, string& error_out) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        if (is_replay) return true;
        error_out = "no such group '" + group_id + "'";
        return false;
    }
    auto fit = it->second.files.find(filename);
    if (fit == it->second.files.end()) {
        if (is_replay) return true;
        error_out = "no such file '" + filename + "' in group '" + group_id + "'";
        return false;
    }
    auto& addrs = fit->second.seeder_addresses;
    auto pos = find(addrs.begin(), addrs.end(), address);
    if (pos == addrs.end()) {
        if (is_replay) return true; // already gone, or never had it -- fine
        error_out = "you are not currently seeding '" + filename + "' in '" + group_id + "'";
        return false;
    }
    addrs.erase(pos);
    return true;
}

uint64_t TrackerState::get_file_uploaded_at(const string& group_id, const string& filename) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) return 0;
    auto fit = it->second.files.find(filename);
    return fit == it->second.files.end() ? 0 : fit->second.uploaded_at_ms;
}

bool TrackerState::list_files(const string& group_id, string& out_or_error) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        out_or_error = "no such group '" + group_id + "'";
        return false;
    }
    if (it->second.files.empty()) {
        out_or_error = "no files shared in '" + group_id + "' yet";
        return true;
    }
    ostringstream oss;
    bool first = true;
    for (const auto& [fname, f] : it->second.files) {
        if (!first) oss << "\n";
        first = false;
        oss << fname << " (size: " << f.file_size << " bytes, pieces: " << f.piece_hashes.size()
            << ", uploader: " << f.uploader_id << " at " << f.uploader_address
            << ", seeders: " << f.seeder_addresses.size() << ")";
    }
    out_or_error = oss.str();
    return true;
}

bool TrackerState::get_file_info(const string& group_id, const string& filename, string& out_or_error) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        out_or_error = "no such group '" + group_id + "'";
        return false;
    }
    auto fit = it->second.files.find(filename);
    if (fit == it->second.files.end()) {
        out_or_error = "no such file '" + filename + "' in group '" + group_id + "'";
        return false;
    }
    const FileInfo& f = fit->second;
    if (f.seeder_addresses.empty()) {
        // Can happen if every seeder (including the original uploader) has since stop_share'd.
        // The file's catalog entry (size/hash/piece list) is untouched -- just nobody to ask.
        out_or_error = "no peers currently seeding '" + filename + "' in '" + group_id + "'";
        return false;
    }
    out_or_error = to_string(f.file_size) + " " + f.file_hash + " " + join_addr_csv(f.seeder_addresses) + " "
                 + join_piece_hashes(f.piece_hashes);
    return true;
}

uint64_t TrackerState::get_group_created_at(const string& group_id) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    return it == groups.end() ? 0 : it->second.created_at_ms;
}

string TrackerState::list_groups() {
    lock_guard<mutex> lock(mtx);
    if (groups.empty()) return "No groups exist yet.";
    ostringstream oss;
    bool first = true;
    for (const auto& [gid, g] : groups) {
        if (!first) oss << "\n";
        first = false;
        oss << gid << " (owner: " << g.owner_id << ", members: " << g.member_count() << ")";
    }
    return oss.str();
}

bool TrackerState::list_requests(const string& group_id, const string& requester_id, string& out_or_error) {
    lock_guard<mutex> lock(mtx);
    auto it = groups.find(group_id);
    if (it == groups.end()) {
        out_or_error = "no such group '" + group_id + "'";
        return false;
    }
    if (it->second.owner_id != requester_id) {
        out_or_error = "only the owner of '" + group_id + "' can view join requests";
        return false;
    }
    set<string> pending = it->second.pending_set();
    if (pending.empty()) {
        out_or_error = "no pending requests for '" + group_id + "'";
        return true;
    }
    ostringstream oss;
    bool first = true;
    for (const auto& uid : pending) {
        if (!first) oss << "\n";
        first = false;
        oss << uid;
    }
    out_or_error = oss.str();
    return true;
}

static string join_seq_csv(const map<string, uint64_t>& m) {
    if (m.empty()) return "-";
    ostringstream oss;
    bool first = true;
    for (const auto& [user_id, seq] : m) {
        if (!first) oss << ",";
        first = false;
        oss << user_id << ":" << seq;
    }
    return oss.str();
}


static string join_addr_csv(const vector<string>& addrs) {
    if (addrs.empty()) return "-";
    ostringstream oss;
    bool first = true;
    for (const auto& a : addrs) {
        if (!first) oss << ",";
        first = false;
        oss << a;
    }
    return oss.str();
}

static vector<string> split_addr_csv(const string& s) {
    vector<string> out;
    if (s.empty() || s == "-") return out;
    stringstream ss(s);
    string item;
    while (getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

static map<string, uint64_t> split_seq_csv(const string& s) {
    map<string, uint64_t> out;
    if (s.empty() || s == "-") return out;
    stringstream ss(s);
    string item;
    while (getline(ss, item, ',')) {
        if (item.empty()) continue;
        size_t colon = item.find(':');
        if (colon == string::npos) continue; // malformed, skip
        out[item.substr(0, colon)] = stoull(item.substr(colon + 1));
    }
    return out;
}



string TrackerState::serialize_snapshot() {
    lock_guard<mutex> lock(mtx);
    ostringstream oss;
    for (const auto& [uid, u] : users) {
        oss << "USER " << uid << " " << u.password << " " << u.created_at_ms << "\n";
    }
    for (const auto& [gid, g] : groups) {
        oss << "GROUP " << gid << " " << g.creator_id << " " << (g.owner_id.empty() ? "-" : g.owner_id) << " " << g.created_at_ms << " " << g.next_seq << " "
            << join_seq_csv(g.member_add_seq) << " " << join_seq_csv(g.member_remove_seq) << " "
            << join_seq_csv(g.pending_add_seq) << " " << join_seq_csv(g.pending_remove_seq) << "\n";
        for (const auto& [fname, f] : g.files) {
            oss << "FILE " << gid << " " << fname << " " << f.uploader_id << " " << f.uploader_address << " "
                << f.file_size << " " << f.file_hash << " " << f.uploaded_at_ms << " "
                << join_piece_hashes(f.piece_hashes) << " " << join_addr_csv(f.seeder_addresses) << "\n";
        }
    }
    return oss.str();
}


static void merge_seq_map(map<string, uint64_t>& local, const map<string, uint64_t>& incoming) {
    for (const auto& [user_id, seq] : incoming) {
        uint64_t& slot = local[user_id];
        if (seq > slot) slot = seq;
    }
}

void TrackerState::merge_snapshot(const string& snapshot) {
    lock_guard<mutex> lock(mtx);
    istringstream iss(snapshot);
    string line;
    while (getline(iss, line)) {
        if (line.empty()) continue;
        istringstream ls(line);
        string tag;
        ls >> tag;

        if (tag == "USER") {
            string uid, pw, created_str;
            ls >> uid >> pw >> created_str;
            uint64_t incoming_created = created_str.empty() ? 0 : stoull(created_str);

            auto it = users.find(uid);
            if (it == users.end()) {
                users[uid] = User{uid, pw, incoming_created};
            } else if (it->second.password != pw) {
                
                bool incoming_wins = (incoming_created != it->second.created_at_ms)
                                          ? (incoming_created < it->second.created_at_ms)
                                          : (pw < it->second.password);
                if (incoming_wins) {
                    it->second.password = pw;
                    it->second.created_at_ms = incoming_created;
                }
            }
        }  else if (tag == "GROUP") {
            string gid, creator, owner, created_str, next_seq_str, madd, mrem, padd, prem;
            ls >> gid >> creator >> owner >> created_str >> next_seq_str >> madd >> mrem >> padd >> prem;

            if (owner == "-") {
                owner.clear();
            }

            uint64_t incoming_created = created_str.empty() ? 0 : stoull(created_str);
            uint64_t incoming_next_seq = stoull(next_seq_str);

            auto it = groups.find(gid);
            if (it == groups.end()) {
                Group g;
                g.group_id = gid;
                g.creator_id = creator;
                g.owner_id = owner;
                g.created_at_ms = incoming_created;
                g.next_seq = incoming_next_seq;
                g.member_add_seq = split_seq_csv(madd);
                g.member_remove_seq = split_seq_csv(mrem);
                g.pending_add_seq = split_seq_csv(padd);
                g.pending_remove_seq = split_seq_csv(prem);
                g.recompute_owner();
                groups[gid] = g;
            } else {
                Group& g = it->second;
               
                bool same_group = (g.creator_id == creator && g.created_at_ms == incoming_created);
                if (!same_group) {
                    
                    bool incoming_wins = (incoming_created != g.created_at_ms)
                                              ? (incoming_created < g.created_at_ms)
                                              : (creator < g.creator_id);
                    if (incoming_wins) {
                        Group replacement;
                        replacement.group_id = gid;
                        replacement.creator_id = creator;
                        replacement.owner_id = owner;
                        replacement.created_at_ms = incoming_created;
                        replacement.next_seq = incoming_next_seq;
                        replacement.member_add_seq = split_seq_csv(madd);
                        replacement.member_remove_seq = split_seq_csv(mrem);
                        replacement.pending_add_seq = split_seq_csv(padd);
                        replacement.pending_remove_seq = split_seq_csv(prem);
                        replacement.recompute_owner();
                        g = replacement;
                    }
                } else {
                    // Same group instance: merge membership/request counters independently.
                    merge_seq_map(g.member_add_seq, split_seq_csv(madd));
                    merge_seq_map(g.member_remove_seq, split_seq_csv(mrem));
                    merge_seq_map(g.pending_add_seq, split_seq_csv(padd));
                    merge_seq_map(g.pending_remove_seq, split_seq_csv(prem));
                    g.next_seq = max(g.next_seq, incoming_next_seq);
                    // owner_id is derived from the surviving membership history.
                    g.recompute_owner();
                }
            }
        } else if (tag == "FILE") {
            string gid, fname, uploader, addr, size_str, fhash, uploaded_str, pieces_csv, seeders_csv;
            ls >> gid >> fname >> uploader >> addr >> size_str >> fhash >> uploaded_str >> pieces_csv >> seeders_csv;

            auto git = groups.find(gid);
            if (git == groups.end()) continue; // group's own GROUP record precedes this in any real snapshot

            uint64_t incoming_size = size_str.empty() ? 0 : stoull(size_str);
            uint64_t incoming_uploaded = uploaded_str.empty() ? 0 : stoull(uploaded_str);
            vector<string> incoming_seeders = split_addr_csv(seeders_csv);

            Group& g = git->second;
            auto fit = g.files.find(fname);
            if (fit == g.files.end()) {
                FileInfo f;
                f.filename = fname;
                f.uploader_id = uploader;
                f.uploader_address = addr;
                f.file_size = incoming_size;
                f.file_hash = fhash;
                f.uploaded_at_ms = incoming_uploaded;
                f.piece_hashes = split_piece_hashes(pieces_csv);
                f.seeder_addresses = incoming_seeders;
                g.files[fname] = f;
            } else if (fit->second.file_hash != fhash) {
                
                FileInfo& existing = fit->second;
                bool incoming_wins = (incoming_uploaded != existing.uploaded_at_ms)
                                          ? (incoming_uploaded < existing.uploaded_at_ms)
                                          : (fhash < existing.file_hash);
                if (incoming_wins) {
                    existing.uploader_id = uploader;
                    existing.uploader_address = addr;
                    existing.file_size = incoming_size;
                    existing.file_hash = fhash;
                    existing.uploaded_at_ms = incoming_uploaded;
                    existing.piece_hashes = split_piece_hashes(pieces_csv);
                    existing.seeder_addresses = incoming_seeders;
                }
            } else {
                
                auto& existing_addrs = fit->second.seeder_addresses;
                for (const auto& a : incoming_seeders) {
                    if (find(existing_addrs.begin(), existing_addrs.end(), a) == existing_addrs.end()) {
                        existing_addrs.push_back(a);
                    }
                }
            }
        }
    }
}
