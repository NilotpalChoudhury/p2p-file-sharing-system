
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/network.h"
#include "../common/protocol.h"
#include "../common/file_pieces.h"
#include "../common/sha1.h"

using namespace std;


static mutex print_mutex;
static void print_line(const string& s) {
    lock_guard<mutex> lock(print_mutex);
    cout << s << endl;
}

static void print_prompt() {
    lock_guard<mutex> lock(print_mutex);
    cout << "> " << flush;
}


constexpr int MAX_PEERS_PER_DOWNLOAD = 4;

static bool command_for(const string& verb, uint16_t& out_command) {
    if (verb == "create_user")    { out_command = MSG_CREATE_USER;    return true; }
    if (verb == "login")          { out_command = MSG_LOGIN;          return true; }
    if (verb == "logout")         { out_command = MSG_LOGOUT;         return true; }
    if (verb == "create_group")   { out_command = MSG_CREATE_GROUP;   return true; }
    if (verb == "join_group")     { out_command = MSG_JOIN_GROUP;     return true; }
    if (verb == "leave_group")    { out_command = MSG_LEAVE_GROUP;    return true; }
    if (verb == "list_groups")    { out_command = MSG_LIST_GROUPS;    return true; }
    if (verb == "list_requests")  { out_command = MSG_LIST_REQUESTS;  return true; }
    if (verb == "accept_request") { out_command = MSG_ACCEPT_REQUEST; return true; }
    if (verb == "list_files")     { out_command = MSG_LIST_FILES;     return true; } 
    if (verb == "ping")           { out_command = MSG_PING;           return true; } 
    return false;
}

static string basename_of(const string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == string::npos) ? path : path.substr(pos + 1);
}


class SeedingFiles {
public:
    void add(const string& filename, const string& local_path) {
        lock_guard<mutex> lock(mtx);
        paths[filename] = local_path;
    }
    bool lookup(const string& filename, string& local_path) {
        lock_guard<mutex> lock(mtx);
        auto it = paths.find(filename);
        if (it == paths.end()) return false;
        local_path = it->second;
        return true;
    }

    void remove(const string& filename) {
        lock_guard<mutex> lock(mtx);
        paths.erase(filename);
    }

    void clear_all() {
        lock_guard<mutex> lock(mtx);
        paths.clear();
    }
private:
    mutex mtx;
    map<string, string> paths;
};


class SeederRegistrations {
public:

    bool add(const string& group_id, const string& filename) {
        lock_guard<mutex> lock(mtx);
        if (logged_out) return false;
        entries.push_back({group_id, filename});
        return true;
    }

    vector<pair<string, string>> mark_logged_out_and_take_all() {
        lock_guard<mutex> lock(mtx);
        logged_out = true;
        vector<pair<string, string>> out;
        out.swap(entries);
        return out;
    }
    // Called after a fresh login (following an earlier logout) so THAT session's downloads can register again.
    void reset_for_new_session() {
        lock_guard<mutex> lock(mtx);
        logged_out = false;
    }
private:
    mutex mtx;
    bool logged_out = false;
    vector<pair<string, string>> entries;
};

static bool read_piece_from_disk(const string& path, uint64_t piece_index, string& piece_data, string& err) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = string("could not open '") + path + "': " + strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        err = string("fstat failed: ") + strerror(errno);
        close(fd);
        return false;
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t total_pieces = piece_count_for_size(file_size);
    if (piece_index >= total_pieces) {
        err = "piece index " + to_string(piece_index) + " out of range (file has " + to_string(total_pieces) + " pieces)";
        close(fd);
        return false;
    }
    uint64_t offset = piece_index * PIECE_SIZE;
    uint64_t want = min(PIECE_SIZE, file_size - offset);

    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        err = string("seek failed: ") + strerror(errno);
        close(fd);
        return false;
    }
    string buf(want, '\0');
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, &buf[got], want - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = string("read failed: ") + strerror(errno);
            close(fd);
            return false;
        }
        if (n == 0) break; // file shrank underneath us
        got += static_cast<size_t>(n);
    }
    close(fd);
    if (got != want) {
        err = "unexpected end of file while reading piece " + to_string(piece_index);
        return false;
    }
    piece_data = std::move(buf);
    return true;
}

// One accepted peer connection: serve as many piece requests as they send, until they
// disconnect. Mirrors tracker.cpp's own per-connection handler loop.
static void handle_peer_connection(int peer_fd, SeedingFiles* seeding) {
    string peer_address = get_peer_address(peer_fd); 
    string announced_filename; 
    uint64_t pieces_served = 0;

    while (true) {
        uint16_t cmd, status;
        string payload;
        if (!recv_message(peer_fd, cmd, status, payload)) break; // peer disconnected

        if (cmd != MSG_PEER_PIECE_REQUEST) {
            send_message(peer_fd, cmd, STATUS_ERROR, "unsupported request");
            continue;
        }
        istringstream iss(payload);
        string filename;
        uint64_t piece_index;
        if (!(iss >> filename >> piece_index)) {
            send_message(peer_fd, cmd, STATUS_ERROR, "malformed piece request");
            continue;
        }
        if (announced_filename.empty()) {
            announced_filename = filename;
            print_line("[seeding] now sending '" + filename + "' to "
                       + (peer_address.empty() ? "a peer" : peer_address));
        }
        string local_path;
        if (!seeding->lookup(filename, local_path)) {
            send_message(peer_fd, cmd, STATUS_ERROR, "not seeding '" + filename + "'");
            continue;
        }
        string piece_data, err;
        if (!read_piece_from_disk(local_path, piece_index, piece_data, err)) {
            send_message(peer_fd, cmd, STATUS_ERROR, err);
            continue;
        }
        send_message(peer_fd, cmd, STATUS_OK, piece_data);
        pieces_served++;
    }
    close(peer_fd);

    if (!announced_filename.empty()) {
        print_line("[seeding] finished sending '" + announced_filename + "' to "
                   + (peer_address.empty() ? "a peer" : peer_address)
                   + " (" + to_string(pieces_served) + " piece(s) sent)");
    }
}


static void seeder_accept_loop(int seed_fd, SeedingFiles* seeding) {
    while (true) {
        int peer_fd = accept_connection(seed_fd);
        if (peer_fd < 0) continue;
        thread(handle_peer_connection, peer_fd, seeding).detach();
    }
}


struct DownloadState {
    string group_id, filename, dest_path;
    uint64_t file_size = 0;
    string expected_file_hash;
    vector<string> piece_hashes;
    size_t num_pieces = 0;

    // Needed to talk to the tracker again after this download finishes (to register as a new
    // seeder) without touching the REPL's own tracker socket from a background thread.
    string tracker_host;
    int tracker_port = 0;
    string my_seed_address;
    SeedingFiles* seeding_files_ptr = nullptr; 
    SeederRegistrations* registrations_ptr = nullptr; 

    mutex m; // guards every field below -- read or write, always under this lock
    condition_variable cv; 
    deque<size_t> pending;                 
    map<size_t, set<string>> failed_peers; 
    set<size_t> in_flight_indices; 
    size_t pieces_done = 0;
    int last_reported_percent = -1; 
    int active_workers = 0;
    bool failed = false;
    string fail_reason;
    bool finished = false;
    bool succeeded = false;
};

// All downloads this client has ever started this session, for show_downloads (Day 5) to walk.
// Kept even after completion so "[C] [group] filename" remains visible, matching spec section 8.
class DownloadRegistry {
public:
    shared_ptr<DownloadState> start(const string& gid, const string& filename, const string& dest_path,
                                     uint64_t file_size, const string& file_hash,
                                     const vector<string>& piece_hashes,
                                     const string& tracker_host, int tracker_port,
                                     const string& my_seed_address, SeedingFiles* seeding_files_ptr,
                                     SeederRegistrations* registrations_ptr) {
        auto st = make_shared<DownloadState>();
        st->group_id = gid;
        st->filename = filename;
        st->dest_path = dest_path;
        st->file_size = file_size;
        st->expected_file_hash = file_hash;
        st->piece_hashes = piece_hashes;
        st->num_pieces = piece_hashes.size();
        st->tracker_host = tracker_host;
        st->tracker_port = tracker_port;
        st->my_seed_address = my_seed_address;
        st->seeding_files_ptr = seeding_files_ptr;
        st->registrations_ptr = registrations_ptr;
        for (size_t i = 0; i < st->num_pieces; i++) st->pending.push_back(i);

        lock_guard<mutex> lock(mtx);
        downloads.push_back(st);
        return st;
    }

    // Refuses a second concurrent download into the same destination file -- two independent
    // worker pools pwrite()-ing the same path would just race for no benefit.
    bool has_active_download_to(const string& dest_path) {
        lock_guard<mutex> lock(mtx);
        for (auto& st : downloads) {
            lock_guard<mutex> slock(st->m);
            if (!st->finished && st->dest_path == dest_path) return true;
        }
        return false;
    }

    vector<shared_ptr<DownloadState>> list_all() {
        lock_guard<mutex> lock(mtx);
        return downloads; // copying the shared_ptrs is cheap and race-free
    }

private:
    mutex mtx;
    vector<shared_ptr<DownloadState>> downloads;
};

// Runs once a download's pieces are either all in or unrecoverable -- called by whichever
// worker happens to be the last one to finish (see finish_worker), or directly for a 0-piece
// (empty) file with no workers to call it at all.
static void finalize_download(const shared_ptr<DownloadState>& st) {
    string result_line;
    bool succeeded_now;
    {
        lock_guard<mutex> lock(st->m);
        if (st->finished) return; 

        if (!st->failed && st->pieces_done == st->num_pieces) {
          
            FilePieceInfo written_info;
            string verify_err;
            if (!compute_file_pieces(st->dest_path, written_info, verify_err)) {
                st->failed = true;
                st->fail_reason = "could not verify downloaded file: " + verify_err;
            } else if (written_info.file_size != st->file_size || written_info.file_hash != st->expected_file_hash) {
                st->failed = true;
                st->fail_reason = "downloaded file failed whole-file verification";
            } else {
                st->succeeded = true;
            }
        } else if (!st->failed) {
            // Every worker gave up (peers exhausted / all disconnected) before finishing the queue.
            st->failed = true;
            st->fail_reason = "ran out of peers before all pieces could be fetched ("
                             + to_string(st->pieces_done) + "/" + to_string(st->num_pieces) + " pieces done)";
        }
        st->finished = true;
        succeeded_now = st->succeeded;
        result_line = succeeded_now
            ? ("[C] [" + st->group_id + "] " + st->filename)
            : ("[F] [" + st->group_id + "] " + st->filename + " - " + st->fail_reason);
    }

    if (succeeded_now) {
     
        if (st->registrations_ptr->add(st->group_id, st->filename)) {
           
            st->seeding_files_ptr->add(st->filename, st->dest_path);

            int reg_fd = connect_to(st->tracker_host, st->tracker_port);
            if (reg_fd >= 0) {
                string payload = st->group_id + " " + st->filename + " " + st->my_seed_address;
                send_message(reg_fd, MSG_REGISTER_SEEDER, 0, payload);
                uint16_t rc, rs;
                string rp;
                recv_message(reg_fd, rc, rs, rp); // response discarded either way -- best-effort
                close(reg_fd);
            }
        }
    }

    lock_guard<mutex> plock(print_mutex);
    cout << "\n" << result_line << "\n> " << flush;
}

static void finish_worker(const shared_ptr<DownloadState>& st) {
    bool last;
    {
        lock_guard<mutex> lock(st->m);
        st->active_workers--;
        last = (st->active_workers == 0);
    }
    if (last) finalize_download(st);
}


static void maybe_report_progress(const shared_ptr<DownloadState>& st) {
    string message;
    {
        lock_guard<mutex> lock(st->m);
        if (st->num_pieces == 0) return; // handled inline in download_file, no workers involved
        int percent = static_cast<int>((st->pieces_done * 100) / st->num_pieces);
        int milestone = (percent / 10) * 10;
        if (milestone > st->last_reported_percent && milestone < 100 && milestone > 0) {
            st->last_reported_percent = milestone;
            message = "[..] [" + st->group_id + "] " + st->filename + ": " + to_string(milestone)
                    + "% (" + to_string(st->pieces_done) + "/" + to_string(st->num_pieces) + " pieces)";
        }
    }
    if (!message.empty()) {
        lock_guard<mutex> lock(print_mutex);
        cout << "\n" << message << "\n> " << flush;
    }
}


static void download_worker(shared_ptr<DownloadState> st, string peer_address) {
    string peer_host;
    int peer_port;
    if (!parse_host_port(peer_address, peer_host, peer_port)) {
        finish_worker(st); // malformed address in the tracker's peer list -- just don't use it
        return;
    }
    int peer_fd = connect_to(peer_host, peer_port);
    if (peer_fd < 0) {
        finish_worker(st); // this peer's unreachable -- other peers/workers carry the load
        return;
    }
    int out_fd = open(st->dest_path.c_str(), O_WRONLY, 0644); // pre-sized by download_file already
    if (out_fd < 0) {
        close(peer_fd);
        finish_worker(st);
        return;
    }

    while (true) {
        size_t idx;
        bool have = false;
        {
            unique_lock<mutex> lock(st->m);
            while (true) {
                if (st->failed) break; // have stays false

                bool found = false;
                for (auto it = st->pending.begin(); it != st->pending.end(); ++it) {
                    auto fit = st->failed_peers.find(*it);
                    bool already_bad_from_us = (fit != st->failed_peers.end() && fit->second.count(peer_address));
                    if (!already_bad_from_us) {
                        idx = *it;
                        st->pending.erase(it);
                        st->in_flight_indices.insert(idx);
                        found = true;
                        break;
                    }
                }
                if (found) { have = true; break; }

 
                bool something_could_still_help_this_peer = false;
                for (size_t i : st->in_flight_indices) {
                    auto fit = st->failed_peers.find(i);
                    bool already_bad_from_us = (fit != st->failed_peers.end() && fit->second.count(peer_address));
                    if (!already_bad_from_us) { something_could_still_help_this_peer = true; break; }
                }
                if (!something_could_still_help_this_peer) break; // have stays false -- done for good
                st->cv.wait_for(lock, chrono::milliseconds(50));
            }
        }
        if (!have) break;

        bool ok = send_message(peer_fd, MSG_PEER_PIECE_REQUEST, 0, st->filename + " " + to_string(idx));
        uint16_t p_status = STATUS_ERROR;
        string piece_data;
        if (ok) {
            uint16_t p_cmd;
            ok = recv_message(peer_fd, p_cmd, p_status, piece_data);
        }

        if (!ok || p_status == STATUS_ERROR) {
       
            {
                lock_guard<mutex> lock(st->m);
                st->pending.push_back(idx);
                st->in_flight_indices.erase(idx);
            }
            st->cv.notify_all(); // wake anyone waiting on exactly this becoming available
            break;
        }

        string actual_hash = Sha1::hex(piece_data.data(), piece_data.size());
        if (actual_hash != st->piece_hashes[idx]) {
         
            {
                lock_guard<mutex> lock(st->m);
                st->failed_peers[idx].insert(peer_address);
                st->pending.push_back(idx); 
                st->in_flight_indices.erase(idx);
            }
            st->cv.notify_all();
            continue;
        }

     
        ssize_t n = pwrite(out_fd, piece_data.data(), piece_data.size(),
                           static_cast<off_t>(idx * PIECE_SIZE));
        if (n < 0 || static_cast<size_t>(n) != piece_data.size()) {
            {
                lock_guard<mutex> lock(st->m);
                st->pending.push_back(idx);
                st->in_flight_indices.erase(idx);
            }
            st->cv.notify_all();
            break; 
        }

        {
            lock_guard<mutex> lock(st->m);
            st->pieces_done++;
            st->in_flight_indices.erase(idx);
        }
        st->cv.notify_all();
        maybe_report_progress(st);
    }

    close(out_fd);
    close(peer_fd);
    finish_worker(st);
}


static vector<string> load_tracker_addresses(const string& path) {
    vector<string> out;
    ifstream f(path);
    string line;
    while (getline(f, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        out.push_back(line.substr(start, end - start + 1));
    }
    return out;
}


static bool failover_to_another_tracker(int& sock_fd, string& current_addr,
                                         const vector<string>& all_addresses) {
    for (const auto& addr : all_addresses) {
        if (addr == current_addr) continue; // this is the one that just failed
        string h;
        int p;
        if (!parse_host_port(addr, h, p)) continue;
        int new_fd = connect_to(h, p);
        if (new_fd >= 0) {
            close(sock_fd);
            sock_fd = new_fd;
            current_addr = addr;
            return true;
        }
    }
    return false;
}


static bool tracker_roundtrip(int& sock_fd, string& current_tracker_addr,
                               const vector<string>& tracker_addresses,
                               bool is_logged_in, const string& cached_user_id, const string& cached_password,
                               uint16_t command, const string& payload,
                               uint16_t& out_status, string& out_payload) {
    if (send_message(sock_fd, command, 0, payload)) {
        uint16_t cmd;
        if (recv_message(sock_fd, cmd, out_status, out_payload)) return true;
    }

    if (!failover_to_another_tracker(sock_fd, current_tracker_addr, tracker_addresses)) {
        return false; // no other tracker reachable either -- genuinely nothing left to try
    }
    print_line("(lost connection to the tracker -- failed over to " + current_tracker_addr + ")");

    if (is_logged_in) {
        uint16_t login_status;
        string login_resp;
        uint16_t login_cmd;
        send_message(sock_fd, MSG_LOGIN, 0, cached_user_id + " " + cached_password);
        recv_message(sock_fd, login_cmd, login_status, login_resp);
    }

    // Retry the ORIGINAL request once, against the new connection.
    if (send_message(sock_fd, command, 0, payload)) {
        uint16_t cmd;
        if (recv_message(sock_fd, cmd, out_status, out_payload)) return true;
    }
    return false;
}


static void stop_sharing_everything(int& sock_fd, string& current_tracker_addr,
                                     const vector<string>& tracker_addresses,
                                     const string& my_seed_address,
                                     SeederRegistrations& registrations, SeedingFiles& seeding_files) {
    auto all = registrations.mark_logged_out_and_take_all();
    for (auto& gid_filename : all) {
        string payload = gid_filename.first + " " + gid_filename.second + " " + my_seed_address;
        uint16_t status;
        string resp;
        tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                           false, "", "", MSG_STOP_SHARE, payload, status, resp);
    }
    seeding_files.clear_all();
}

int main(int argc, char* argv[]) {
   
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <tracker_ip>:<tracker_port> <tracker_info.txt>" << endl;
        return 1;
    }

    string tracker_addr = argv[1];
    string info_path = argv[2];

    string host;
    int port;
    if (!parse_host_port(tracker_addr, host, port)) {
        cerr << "Bad tracker address, expected IP:PORT, got: " << tracker_addr << endl;
        return 1;
    }

    vector<string> tracker_addresses = load_tracker_addresses(info_path);
    if (tracker_addresses.empty()) {
        cerr << "Could not open or parse tracker info file: " << info_path << endl;
        return 1;
    }
    string current_tracker_addr = tracker_addr;

    int sock_fd = connect_to(host, port);
    if (sock_fd < 0) {
        
        cerr << "Could not connect to tracker at " << tracker_addr << " -- trying others in "
             << info_path << "..." << endl;
        if (!failover_to_another_tracker(sock_fd, current_tracker_addr, tracker_addresses)) {
            cerr << "Could not connect to any tracker in " << info_path << endl;
            return 1;
        }
        print_line("Connected to tracker at " + current_tracker_addr + " instead");
    }

    int seed_fd = create_listening_socket(0);
    if (seed_fd < 0) {
        cerr << "Could not open a listening socket for seeding" << endl;
        close(sock_fd);
        return 1;
    }
    int seed_port = get_local_port(seed_fd);
  
    string seed_ip = get_local_ip(sock_fd);
    if (seed_port < 0 || seed_ip.empty()) {
        cerr << "Could not determine this client's own address" << endl;
        close(sock_fd);
        close(seed_fd);
        return 1;
    }
    string my_seed_address = seed_ip + ":" + to_string(seed_port);

    SeedingFiles seeding_files;
    thread(seeder_accept_loop, seed_fd, &seeding_files).detach();

  
    DownloadRegistry download_registry;

    SeederRegistrations my_registrations;
    bool is_logged_in = false;
    string cached_user_id, cached_password;

    print_line("Connected to tracker at " + current_tracker_addr);
    print_line("Seeding address: " + my_seed_address + " (accepting peer connections)");
    print_line("Commands: create_user <id> <pw>, login <id> <pw>, logout, create_group <gid>,"
               " join_group <gid>, leave_group <gid>, list_groups, list_requests <gid>,"
               " accept_request <gid> <uid>, upload_file <gid> <file_path>, list_files <gid>,"
// tracker-to-tracker sync link. All user/group state lives in TrackerState, not here.
               " download_file <gid> <file_name> <dest_path>, show_downloads,"
               " stop_share <gid> <file_name>, quit");

    string line;
    while (true) {
        print_prompt();
        if (!getline(cin, line)) break; // EOF (Ctrl+D)
        if (line.empty()) continue;

        istringstream iss(line);
        string verb;
        iss >> verb;

        if (verb == "quit") break;

 
        if (verb == "upload_file") {
            string gid, file_path;
            if (!(iss >> gid >> file_path)) {
                print_line("usage: upload_file <group_id> <file_path>");
                continue;
            }
            string filename = basename_of(file_path);
            if (filename.empty()) {
                print_line("ERROR: '" + file_path + "' has no filename component");
                continue;
            }
            FilePieceInfo info;
            string err;
            if (!compute_file_pieces(file_path, info, err)) {
                print_line("ERROR: " + err);
                continue;
            }
            string payload = gid + " " + filename + " " + to_string(info.file_size) + " "
                            + info.file_hash + " " + my_seed_address + " "
                            + join_piece_hashes(info.piece_hashes);

            uint16_t resp_status;
            string resp_payload;
            if (!tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                                    is_logged_in, cached_user_id, cached_password,
                                    MSG_UPLOAD_FILE, payload, resp_status, resp_payload)) {
                cerr << "Lost connection to all known trackers" << endl;
                break;
            }
            print_line((resp_status == STATUS_ERROR ? "ERROR: " : "") + resp_payload);
            if (resp_status != STATUS_ERROR) {
                seeding_files.add(filename, file_path);
                my_registrations.add(gid, filename); // the uploader is the first seeder tracker-side too
            }
            continue;
        }

      
        if (verb == "download_file") {
            string gid, filename, dest_path;
            if (!(iss >> gid >> filename >> dest_path)) {
                print_line("usage: download_file <group_id> <file_name> <destination_path>");
                continue;
            }

            uint16_t resp_status;
            string resp_payload;
            if (!tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                                    is_logged_in, cached_user_id, cached_password,
                                    MSG_GET_FILE_INFO, gid + " " + filename, resp_status, resp_payload)) {
                cerr << "Lost connection to all known trackers" << endl;
                break;
            }
            if (resp_status == STATUS_ERROR) {
                print_line("ERROR: " + resp_payload);
                continue;
            }
            istringstream info_iss(resp_payload);
            uint64_t expected_size;
            string expected_file_hash, peers_csv, pieces_csv;
            if (!(info_iss >> expected_size >> expected_file_hash >> peers_csv >> pieces_csv)) {
                print_line("ERROR: malformed file info from tracker");
                continue;
            }
     
            vector<string> peer_addresses = split_piece_hashes(peers_csv);
            vector<string> piece_hashes = split_piece_hashes(pieces_csv);
            size_t num_pieces = piece_hashes.size();

            if (peer_addresses.empty()) {
                print_line("ERROR: no peers currently available for '" + filename + "'");
                continue;
            }
            if (download_registry.has_active_download_to(dest_path)) {
                print_line("ERROR: a download to '" + dest_path + "' is already in progress");
                continue;
            }

   
            int precreate_fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (precreate_fd < 0) {
                print_line(string("ERROR: could not open destination '") + dest_path + "': " + strerror(errno));
                continue;
            }
            if (expected_size > 0 && ftruncate(precreate_fd, static_cast<off_t>(expected_size)) < 0) {
                print_line(string("ERROR: could not size destination '") + dest_path + "': " + strerror(errno));
                close(precreate_fd);
                continue;
            }
            close(precreate_fd);

            
            string dl_tracker_host;
            int dl_tracker_port;
            parse_host_port(current_tracker_addr, dl_tracker_host, dl_tracker_port);

            auto state = download_registry.start(gid, filename, dest_path, expected_size, expected_file_hash,
                                                  piece_hashes, dl_tracker_host, dl_tracker_port,
                                                  my_seed_address, &seeding_files, &my_registrations);

            if (num_pieces == 0) {
      
                finalize_download(state);
                continue;
            }

            size_t worker_count = min(peer_addresses.size(), static_cast<size_t>(MAX_PEERS_PER_DOWNLOAD));
            if (worker_count > num_pieces) worker_count = num_pieces; // no point idling extra threads
            state->active_workers = static_cast<int>(worker_count);

            print_line("download started: '" + filename + "' in '" + gid + "' -> '" + dest_path + "' ("
                       + to_string(num_pieces) + " piece(s), using " + to_string(worker_count) + " peer(s))");
            for (size_t i = 0; i < worker_count; i++) {
                thread(download_worker, state, peer_addresses[i]).detach();
            }
            continue;
        }

      
        if (verb == "show_downloads") {
            auto all = download_registry.list_all();
            if (all.empty()) {
                print_line("no downloads started this session");
                continue;
            }
            for (auto& st : all) {
                lock_guard<mutex> lock(st->m);
                if (st->finished && st->succeeded) {
                    print_line("[C] [" + st->group_id + "] " + st->filename);
                } else if (st->finished) {
                    print_line("[F] [" + st->group_id + "] " + st->filename + " - " + st->fail_reason);
                } else {
                    print_line("[..] [" + st->group_id + "] " + st->filename + " ("
                               + to_string(st->pieces_done) + "/" + to_string(st->num_pieces) + " pieces)");
                }
            }
            continue;
        }

        
        if (verb == "stop_share") {
            string gid, filename;
            if (!(iss >> gid >> filename)) {
                print_line("usage: stop_share <group_id> <file_name>");
                continue;
            }
            string payload = gid + " " + filename + " " + my_seed_address;
            uint16_t resp_status;
            string resp_payload;
            if (!tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                                    is_logged_in, cached_user_id, cached_password,
                                    MSG_STOP_SHARE, payload, resp_status, resp_payload)) {
                cerr << "Lost connection to all known trackers" << endl;
                break;
            }
            print_line((resp_status == STATUS_ERROR ? "ERROR: " : "") + resp_payload);
            if (resp_status != STATUS_ERROR) {
                seeding_files.remove(filename); // this client itself stops answering piece requests for it
            }
            continue;
        }

    
        if (verb == "logout") {
            uint16_t resp_status;
            string resp_payload;
            if (!tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                                    is_logged_in, cached_user_id, cached_password,
                                    MSG_LOGOUT, "", resp_status, resp_payload)) {
                cerr << "Lost connection to all known trackers" << endl;
                break;
            }
            print_line((resp_status == STATUS_ERROR ? "ERROR: " : "") + resp_payload);
            if (resp_status != STATUS_ERROR) {
                is_logged_in = false;
                cached_user_id.clear();
                cached_password.clear();
                stop_sharing_everything(sock_fd, current_tracker_addr, tracker_addresses,
                                         my_seed_address, my_registrations, seeding_files);
            }
            continue;
        }

        uint16_t command;
        if (!command_for(verb, command)) {
            print_line("Unknown command: " + verb);
            continue;
        }

        string arg, payload; // rest of the line, normalized to single spaces between args
        bool first = true;
        while (iss >> arg) {
            if (!first) payload += " ";
            payload += arg;
            first = false;
        }

        uint16_t resp_status;
        string resp_payload;
        if (!tracker_roundtrip(sock_fd, current_tracker_addr, tracker_addresses,
                                is_logged_in, cached_user_id, cached_password,
                                command, payload, resp_status, resp_payload)) {
            cerr << "Lost connection to all known trackers" << endl;
            break;
        }

        if (verb == "login" && resp_status != STATUS_ERROR) {
            istringstream login_iss(payload);
            login_iss >> cached_user_id >> cached_password;
            is_logged_in = true;
            my_registrations.reset_for_new_session();
        }

        print_line((resp_status == STATUS_ERROR ? "ERROR: " : "") + resp_payload);
    }


    stop_sharing_everything(sock_fd, current_tracker_addr, tracker_addresses,
                             my_seed_address, my_registrations, seeding_files);

    close(sock_fd);
    close(seed_fd);
    return 0;
}
