#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

#include "../common/network.h"
#include "../common/protocol.h"
#include "tracker_state.h"

using namespace std;

static mutex print_mutex; // guards cout so log lines from different threads don't interleave

static void log_line(const string& msg) {
    lock_guard<mutex> lock(print_mutex);
    cout << msg << endl;
}

class PeerLink {
public:
    void set_fd(int new_fd) {
        lock_guard<mutex> lock(mtx);
        fd = new_fd;
    }

    void clear() {
        lock_guard<mutex> lock(mtx);
        fd = -1;
    }

    bool send_raw(uint16_t command, uint16_t status, const string& payload) {
        lock_guard<mutex> lock(mtx);
        if (fd < 0) return false;
        return send_message(fd, command, status, payload);
    }

    void replicate(const string& op) {
        send_raw(MSG_SYNC_OP, STATUS_OK, op);
    }

private:
    mutex mtx;
    int fd = -1;
};

// tracker_info.txt: one "IP:PORT" line per tracker.
static vector<string> read_tracker_info(const string& path) {
    vector<string> lines;
    ifstream file(path);
    if (!file.is_open()) {
        cerr << "Could not open tracker info file: " << path << endl;
        exit(1);
    }
    string line;
    while (getline(file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

static void dispatch_client_command(TrackerState& state, PeerLink& peer, string& current_user,
                                     uint16_t command, const string& payload,
                                     uint16_t& out_status, string& out_payload) {
    out_status = STATUS_OK;
    istringstream iss(payload);

    switch (command) {
        case MSG_PING: {
            out_payload = "echo: " + payload;
            break;
        }
        case MSG_CREATE_USER: {
            string uid, pw;
            if (!(iss >> uid >> pw)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: create_user <user_id> <password>";
                break;
            }
            string err;
            if (state.create_user(uid, pw, false, err)) {
                out_payload = "user '" + uid + "' created";
                peer.replicate("CREATE_USER " + uid + " " + pw);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_LOGIN: {
            string uid, pw;
            if (!(iss >> uid >> pw)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: login <user_id> <password>";
                break;
            }
            if (!current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "already logged in as '" + current_user + "'; logout first";
                break;
            }
            string err;
            if (state.check_login(uid, pw, err)) {
                current_user = uid;
                out_payload = "welcome, " + uid;
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_LOGOUT: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "not logged in";
                break;
            }
            out_payload = "goodbye, " + current_user;
            current_user.clear();
            break;
        }
        case MSG_CREATE_GROUP: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid;
            if (!(iss >> gid)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: create_group <group_id>";
                break;
            }
            string err;
            if (state.create_group(gid, current_user, false, err)) {
                out_payload = "group '" + gid + "' created, you are the owner";
                peer.replicate("CREATE_GROUP " + gid + " " + current_user + " " + to_string(state.get_group_created_at(gid)));
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_JOIN_GROUP: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid;
            if (!(iss >> gid)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: join_group <group_id>";
                break;
            }
            string err;
            if (state.join_group(gid, current_user, false, err)) {
                out_payload = "join request submitted for '" + gid + "', waiting for owner approval";
                peer.replicate("JOIN_GROUP " + gid + " " + current_user);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_LEAVE_GROUP: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid;
            if (!(iss >> gid)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: leave_group <group_id>";
                break;
            }
            string err;
            if (state.leave_group(gid, current_user, false, err)) {
                out_payload = "left group '" + gid + "'";
                peer.replicate("LEAVE_GROUP " + gid + " " + current_user);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_LIST_GROUPS: {
            out_payload = state.list_groups();
            break;
        }
        case MSG_LIST_REQUESTS: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid;
            if (!(iss >> gid)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: list_requests <group_id>";
                break;
            }
            string result;
            if (state.list_requests(gid, current_user, result)) {
                out_payload = result;
            } else {
                out_status = STATUS_ERROR;
                out_payload = result;
            }
            break;
        }
        case MSG_ACCEPT_REQUEST: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid, target;
            if (!(iss >> gid >> target)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: accept_request <group_id> <user_id>";
                break;
            }
            string err;
            if (state.accept_request(gid, current_user, target, false, err)) {
                out_payload = "accepted '" + target + "' into '" + gid + "'";
                peer.replicate("ACCEPT_REQUEST " + gid + " " + target);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_UPLOAD_FILE: {
            if (current_user.empty()) {
                out_status = STATUS_ERROR;
                out_payload = "you must be logged in";
                break;
            }
            string gid, filename, size_str, file_hash, address, pieces_csv;
            if (!(iss >> gid >> filename >> size_str >> file_hash >> address >> pieces_csv)) {
                out_status = STATUS_ERROR;
                out_payload = "malformed upload_file request";
                break;
            }
            uint64_t size;
            try {
                size = stoull(size_str);
            } catch (...) {
                out_status = STATUS_ERROR;
                out_payload = "malformed file size in upload_file request";
                break;
            }
            string err;
            if (state.upload_file(gid, filename, current_user, address, size, file_hash, pieces_csv, false, err)) {
                out_payload = "file '" + filename + "' shared in '" + gid + "'";
                uint64_t uploaded_at = state.get_file_uploaded_at(gid, filename);
                peer.replicate("UPLOAD_FILE " + gid + " " + filename + " " + current_user + " " + address + " "
                                + size_str + " " + file_hash + " " + to_string(uploaded_at) + " " + pieces_csv);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_LIST_FILES: {
            string gid;
            if (!(iss >> gid)) {
                out_status = STATUS_ERROR;
                out_payload = "usage: list_files <group_id>";
                break;
            }
            string result;
            if (state.list_files(gid, result)) {
                out_payload = result;
            } else {
                out_status = STATUS_ERROR;
                out_payload = result;
            }
            break;
        }
        case MSG_GET_FILE_INFO: {
            string gid, filename;
            if (!(iss >> gid >> filename)) {
                out_status = STATUS_ERROR;
                out_payload = "malformed get_file_info request";
                break;
            }
            string result;
            if (state.get_file_info(gid, filename, result)) {
                out_payload = result;
            } else {
                out_status = STATUS_ERROR;
                out_payload = result;
            }
            break;
        }
        case MSG_REGISTER_SEEDER: {
            string gid, filename, address;
            if (!(iss >> gid >> filename >> address)) {
                out_status = STATUS_ERROR;
                out_payload = "malformed register_seeder request";
                break;
            }
            string err;
            if (state.add_seeder(gid, filename, address, false, err)) {
                out_payload = "registered as a seeder of '" + filename + "' in '" + gid + "'";
                peer.replicate("ADD_SEEDER " + gid + " " + filename + " " + address);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        case MSG_STOP_SHARE: {
            string gid, filename, address;
            if (!(iss >> gid >> filename >> address)) {
                out_status = STATUS_ERROR;
                out_payload = "malformed stop_share request";
                break;
            }
            string err;
            if (state.remove_seeder(gid, filename, address, false, err)) {
                out_payload = "stopped sharing '" + filename + "' in '" + gid + "'";
                peer.replicate("REMOVE_SEEDER " + gid + " " + filename + " " + address);
            } else {
                out_status = STATUS_ERROR;
                out_payload = err;
            }
            break;
        }
        default:
            out_status = STATUS_ERROR;
            out_payload = "unknown command";
    }
}

static void handle_client(int client_fd, int client_num, TrackerState& state, PeerLink& peer,
                           uint16_t first_command, const string& first_payload) {
    log_line("[tracker] client #" + to_string(client_num) + " connected");

    string current_user; // per-connection login state; local only, never synced (see README)

    uint16_t command = first_command;
    string payload = first_payload;
    bool have_message = true;

    while (have_message) {
        uint16_t out_status;
        string out_payload;
        dispatch_client_command(state, peer, current_user, command, payload, out_status, out_payload);

        if (!send_message(client_fd, MSG_RESPONSE, out_status, out_payload)) break;

        uint16_t unused_status;
        have_message = recv_message(client_fd, command, unused_status, payload);
    }

    string note = current_user.empty() ? "" : " (was logged in as '" + current_user + "')";
    log_line("[tracker] client #" + to_string(client_num) + " disconnected" + note);
    close(client_fd);
}

// Applies one replicated operation from the peer tracker. Always is_replay=true.
static void apply_sync_op(TrackerState& state, const string& op) {
    istringstream iss(op);
    string kind;
    iss >> kind;
    string err; // discarded -- sync is best-effort, self-healing via the next snapshot

    if (kind == "CREATE_USER") {
        string uid, pw;
        iss >> uid >> pw;
        state.create_user(uid, pw, true, err);
    } else if (kind == "CREATE_GROUP") {
        string gid, owner, created_str;
        iss >> gid >> owner >> created_str;
        uint64_t created_at = created_str.empty() ? 0 : stoull(created_str);
        state.create_group(gid, owner, true, err, created_at);
    } else if (kind == "JOIN_GROUP") {
        string gid, uid;
        iss >> gid >> uid;
        state.join_group(gid, uid, true, err);
    } else if (kind == "LEAVE_GROUP") {
        string gid, uid;
        iss >> gid >> uid;
        state.leave_group(gid, uid, true, err);
    } else if (kind == "ACCEPT_REQUEST") {
        string gid, uid;
        iss >> gid >> uid;
        state.accept_request(gid, "", uid, true, err); // no owner_id to check on replay
    } else if (kind == "UPLOAD_FILE") {
        string gid, filename, uploader, address, size_str, file_hash, uploaded_str, pieces_csv;
        iss >> gid >> filename >> uploader >> address >> size_str >> file_hash >> uploaded_str >> pieces_csv;
        uint64_t size = size_str.empty() ? 0 : stoull(size_str);
        uint64_t uploaded_at = uploaded_str.empty() ? 0 : stoull(uploaded_str);
        state.upload_file(gid, filename, uploader, address, size, file_hash, pieces_csv, true, err, uploaded_at);
    } else if (kind == "ADD_SEEDER") {
        string gid, filename, address;
        iss >> gid >> filename >> address;
        state.add_seeder(gid, filename, address, true, err);
    } else if (kind == "REMOVE_SEEDER") {
        string gid, filename, address;
        iss >> gid >> filename >> address;
        state.remove_seeder(gid, filename, address, true, err);
    }
}

static void handle_peer_tracker(int fd, TrackerState& state, PeerLink& peer) {
    log_line("[tracker] peer tracker link established, exchanging state...");
    peer.set_fd(fd);
    peer.send_raw(MSG_SYNC_SNAPSHOT, STATUS_OK, state.serialize_snapshot());

    uint16_t command, status;
    string payload;

    while (recv_message(fd, command, status, payload)) {
        if (command == MSG_SYNC_SNAPSHOT) {
            state.merge_snapshot(payload);
            log_line("[tracker] state synced with peer tracker");
        } else if (command == MSG_SYNC_OP) {
            apply_sync_op(state, payload);
        }
    }

    log_line("[tracker] peer tracker link lost");
    peer.clear();
    close(fd);
}

static void dial_peer_loop(string peer_host, int peer_port, int my_tracker_no,
                            TrackerState& state, PeerLink& peer) {
    while (true) {
        int fd = connect_to(peer_host, peer_port);
        if (fd < 0) {
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }
        send_message(fd, MSG_TRACKER_HELLO, STATUS_OK, to_string(my_tracker_no));
        handle_peer_tracker(fd, state, peer); // blocks until this connection drops
        this_thread::sleep_for(chrono::seconds(2));
    }
}

static void console_watcher() {
    string line;
    while (getline(cin, line)) {
        if (line == "quit") {
            log_line("[tracker] shutting down");
            exit(0);
        }
    }
}


static void accept_dispatch(int fd, TrackerState& state, PeerLink& peer, atomic<int>& client_count) {
    uint16_t command, status;
    string payload;
    if (!recv_message(fd, command, status, payload)) {
        close(fd);
        return;
    }

    if (command == MSG_TRACKER_HELLO) {
        handle_peer_tracker(fd, state, peer);
    } else {
        int num = ++client_count;
        handle_client(fd, num, state, peer, command, payload);
    }
}

int main(int argc, char* argv[]) {
 
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <tracker_info.txt> <tracker_no>" << endl;
        return 1;
    }

    string info_path = argv[1];
    int tracker_no = atoi(argv[2]);

    vector<string> trackers = read_tracker_info(info_path);
    if (trackers.size() != 2) {
        cerr << "tracker_info.txt must list exactly 2 trackers (found " << trackers.size() << ")" << endl;
        return 1;
    }
    if (tracker_no < 1 || tracker_no > 2) {
        cerr << "tracker_no must be 1 or 2" << endl;
        return 1;
    }

    string my_host;
    int my_port;
    if (!parse_host_port(trackers[static_cast<size_t>(tracker_no - 1)], my_host, my_port)) {
        cerr << "Bad entry in tracker info file: " << trackers[static_cast<size_t>(tracker_no - 1)] << endl;
        return 1;
    }

    int peer_no = (tracker_no == 1) ? 2 : 1;
    string peer_host;
    int peer_port;
    if (!parse_host_port(trackers[static_cast<size_t>(peer_no - 1)], peer_host, peer_port)) {
        cerr << "Bad entry in tracker info file: " << trackers[static_cast<size_t>(peer_no - 1)] << endl;
        return 1;
    }

    int listen_fd = create_listening_socket(my_port);
    if (listen_fd < 0) {
        cerr << "Failed to start tracker" << endl;
        return 1;
    }

    cout << "[tracker " << tracker_no << "] listening on " << my_host << ":" << my_port << endl;
    cout << "[tracker] type 'quit' and press Enter to shut down" << endl;

    TrackerState state;
    PeerLink peer;

    thread(console_watcher).detach();

   
    if (tracker_no == 1) {
        cout << "[tracker] dialing peer tracker at " << peer_host << ":" << peer_port << "..." << endl;
        thread(dial_peer_loop, peer_host, peer_port, tracker_no, ref(state), ref(peer)).detach();
    }

    atomic<int> client_count{0};
    while (true) {
        int fd = accept_connection(listen_fd);
        if (fd < 0) continue;
        thread(accept_dispatch, fd, ref(state), ref(peer), ref(client_count)).detach();
    }
}
