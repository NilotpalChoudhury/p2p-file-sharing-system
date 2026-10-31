// Wire format shared by every TCP connection: tracker<->client, client<->client (later), tracker<->tracker.
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <cstddef>

// Every message is [fixed header][payload_length bytes of payload].
struct MsgHeader {
    uint32_t payload_length;
    uint16_t command;  // one of MessageType below
    uint16_t status;   // 0 on requests; a StatusCode on responses
};

// Sent as 3 separate fields, not a raw struct copy, so on-wire size stays exactly 8 bytes
// regardless of compiler struct-padding rules.
constexpr size_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t);

enum MessageType : uint16_t {
    MSG_PING     = 1, // connectivity test, echoes payload back
    MSG_RESPONSE = 2, // tracker -> client: generic reply to any request

    // client <-> tracker, user & group management 
    MSG_CREATE_USER    = 10, // "<user_id> <password>"
    MSG_LOGIN          = 11, // "<user_id> <password>"
    MSG_LOGOUT         = 12, // "" (acts on this connection's session)
    MSG_CREATE_GROUP   = 13, // "<group_id>"
    MSG_JOIN_GROUP     = 14, // "<group_id>"
    MSG_LEAVE_GROUP    = 15, // "<group_id>"
    MSG_LIST_GROUPS    = 16, // ""
    MSG_LIST_REQUESTS  = 17, // "<group_id>"
    MSG_ACCEPT_REQUEST = 18, // "<group_id> <user_id>"

    
    MSG_UPLOAD_FILE   = 19, // "<group_id> <filename> <file_size> <file_hash> <uploader_address> <piece_hashes_csv>"
    MSG_LIST_FILES    = 20, // "<group_id>"
    MSG_GET_FILE_INFO = 21, // "<group_id> <filename>" 

    // client <-> tracker
    MSG_REGISTER_SEEDER = 22, // "<group_id> <filename> <address>" -- sent automatically right
                              // after a download finishes whole-file verification, so this
                              // client can now also serve the file it just fetched.
    MSG_STOP_SHARE      = 23, // "<group_id> <filename> <address>" -- backs the spec's
                              // "stop_share <group_id> <file_name>" command.

    // tracker <-> tracker synchronization (spec 7)
    MSG_TRACKER_HELLO = 50, // handshake identifying this connection as a peer tracker, not a client
    MSG_SYNC_OP       = 51, // replicate one state-changing operation
    MSG_SYNC_SNAPSHOT = 52, // full-state dump, exchanged on every tracker-tracker (re)connect

    // client <-> client, direct piece transfer 
    MSG_PEER_PIECE_REQUEST = 70, // "<filename> <piece_index>"
                                 
};

enum StatusCode : uint16_t {
    STATUS_OK    = 0,
    STATUS_ERROR = 1,
};

#endif // PROTOCOL_H
