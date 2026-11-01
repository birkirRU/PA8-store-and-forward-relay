// The protocol layer: what each command does. server.cpp owns the sockets.
//
//   c_*  commands from our own client (A6 protocol, replies are plain text)
//   p_*  commands from other servers (A8 protocol)
//
// Peer handlers return an Answer, which decides both the log line and the reply:
//   std::nullopt  rejected: logged as DROP, nothing sent back (never ERROR, to avoid ping-pong)
//   ""            accepted, no reply: logged as RECV
//   "text"        accepted: logged as RECV, "text" is sent back
// A handler that must send more than one frame (GETMSGS, STATUSRESP) gets the fd
// and calls queue_send() itself.
#include <arpa/inet.h>

#include <ctime>
#include <string>
#include <vector>

#include "common.h"
#include "identity.h"
#include "log_event.h"
#include "server.h"

namespace {

std::vector<std::string> split_on(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0, pos;
    while ((pos = s.find(sep, start)) != std::string::npos) {
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    out.push_back(s.substr(start));
    return out;
}

// One "<id>,<ip>,<port>" entry of a SERVERS list. Everything in it comes from another
// server, so it is checked before it can end up in known_ and be dialled.
bool parse_server_entry(const std::string& entry, std::string& id, std::string& ip, uint16_t& port) {
    std::vector<std::string> parts = split_fields(entry);
    if (parts.size() != 3) return false;

    id = parts[0];
    ip = parts[1];
    const std::string& port_text = parts[2];

    if (!is_valid_name(id)) return false;

    in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;

    // Some servers advertise 0.0.0.0 (their bind address). Dialling it, or 127.x,
    // reaches our own machine, not them.
    if (ip == "0.0.0.0" || ip.rfind("127.", 0) == 0) return false;

    if (port_text.empty() || port_text.size() > 5) return false;
    if (port_text.find_first_not_of("0123456789") != std::string::npos) return false;
    int value = std::stoi(port_text);
    if (value < 1 || value > 65535) return false;

    port = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

// Every complete frame from any connection ends up here.
void Server::handle_command(int fd, const std::string& payload) {
    Client& c = clients_[fd];
    std::string cmd = payload.substr(0, payload.find(','));

    // Clients and servers share one listening port. The first command decides what is
    // at the other end, once: a server always starts with HELO, our client never does.
    if (c.type.value == ConnType::unknown)
        c.type.value = (cmd == "HELO") ? ConnType::peer : ConnType::client;

    const bool is_peer = c.type.value == ConnType::peer;
    // The RECV line of a HELO carries "-" as group ID (log_event.h): the name is only
    // known after the HELO has been handled, so remember it from before.
    const std::string name_before = c.name;
    const char* id = name_before.empty() ? nullptr : name_before.c_str();

    Answer answer;
    if (is_peer) {
        answer = p_handle(fd, c, payload, cmd);
    } else {
        std::string client_answer = c_handle(payload, cmd);
        if (!client_answer.empty()) answer = client_answer;
    }

    if (!answer) {
        std::string detail = tag(fd) + ": rejected unknown or malformed command " + cmd;
        log_event("DROP", c.ip.c_str(), c.port, id, detail.c_str());
        if (!is_peer) queue_send(fd, "ERROR,command does not exist or is malformed");
        return;
    }

    // RECV is logged before the reply in *answer. Handlers that send themselves
    // (GETMSGS, STATUSRESP) have already sent, so their SEND lines come before this RECV.
    log_event_bytes("RECV", c.ip.c_str(), c.port, id, payload.data(), payload.size());

    // An incoming server only becomes a peer with its HELO, so the 8-peer limit is
    // checked here. count_peers() includes this connection, hence ">".
    if (cmd == "HELO" && is_peer && !c.outgoing && count_peers() > MAX_PEERS) {
        log_note("%s: already %zu peers, closing the connection from %s", tag(fd).c_str(), MAX_PEERS, c.name.c_str());
        close_client(fd, "peer limit reached");
        return;
    }

    if (!answer->empty()) queue_send(fd, *answer);

    // Accepting side of the handshake: their HELO -> our SERVERS -> our HELO.
    // On connections we opened, our HELO already went out in finish_connect().
    // queue_send() may have closed the connection, hence clients_.count(fd).
    if (cmd == "HELO" && is_peer && clients_.count(fd) && !clients_[fd].sent_helo) {
        clients_[fd].sent_helo = true;
        queue_send(fd, "HELO," GROUP_ID);
    }
}

std::string Server::c_handle(const std::string& payload, const std::string& cmd) {
    std::string answer = "";

    if (cmd == "SENDMSG")        answer = c_sendmsg(split_fields(payload, 4));
    else if (cmd == "GETMSG")    answer = c_getmsg(split_fields(payload, 2));
    else if (cmd == "STATUSREQ") answer = (payload == "STATUSREQ") ? c_statusreq() : "";
    else if (cmd == "LISTSERVERS") answer = (payload == "LISTSERVERS") ? servers_list() : "";

    return answer;
}

// SENDMSG,<from>,<to>,<message>   -> "OK"
// The client's SENDMSG has no timestamp: the message is created here, so we stamp it.
// It is only stored (pull model): the recipient, or a server relaying for it, sees the
// count in our KEEPALIVE / STATUSRESP and collects it with GETMSGS.
std::string Server::c_sendmsg(const std::vector<std::string>& f) {
    if (f.size() != 4 || !is_valid_name(f[1]) || !is_valid_name(f[2])) return "";

    Message msg{f[1], f[2], std::time(nullptr), f[3]};
    mailboxes_[f[2]].push_back(msg);
    stored_++;
    log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
        f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
    return "OK";
}

// GETMSG,<user>   -> "<from>,<message>", or "NO_MESSAGE"
// Messages from other groups to us are stored under our own GROUP_ID, so our client
// reads its mail with GETMSG,<GROUP_ID>.
std::string Server::c_getmsg(const std::vector<std::string>& f) {
    if (f.size() != 2 || !is_valid_name(f[1])) return "";
    auto it = mailboxes_.find(f[1]);
    if (it == mailboxes_.end()) return "NO_MESSAGE";

    Message m = it->second.front();
    it->second.pop_front();
    stored_--;
    size_t left = it->second.size();
    if (left == 0) mailboxes_.erase(it);
    log_note("delivered message from %s to %s (%zu left for them, %zu stored in total)",
         m.from.c_str(), f[1].c_str(), left, stored_);
    return m.from + "," + m.text;
}

// STATUSREQ   -> "<user1>:<count1>,<user2>:<count2>,...", or "EMPTY"
std::string Server::c_statusreq() {
    std::string out;
    for (const auto& [user, box] : mailboxes_) {
        if (!out.empty()) out += ',';
        out += user + ":" + std::to_string(box.size());
    }
    return out.empty() ? "EMPTY" : out;
}

Server::Answer Server::p_handle(int fd, Client& c, const std::string& payload, const std::string& cmd) {
    if (cmd == "HELO")       return p_helo(c, split_fields(payload, 2));
    if (cmd == "SERVERS")    return p_servers(payload);
    if (cmd == "KEEPALIVE")  return p_keepalive(split_fields(payload, 2));
    if (cmd == "GETMSGS")    return p_getmsgs(fd, split_fields(payload, 2));
    if (cmd == "SENDMSG")    return p_sendmsg(split_fields(payload, 5));
    if (cmd == "STATUSRESP") return p_statusresp(fd, split_fields(payload));
    if (cmd == "STATUSREQ")  return payload == "STATUSREQ" ? p_statusreq() : Answer{};
    if (cmd == "ERROR") {
        log_note("peer %s sent %s", c.name.c_str(), payload.c_str());
        return std::string();
    }
    return std::nullopt;
}

// HELO,<id>   -> SERVERS,<us first>;<peers>...
// The ID is taken on trust: nothing in the protocol proves who a server is.
Server::Answer Server::p_helo(Client& c, const std::vector<std::string>& f) {
    if (f.size() != 2 || !is_valid_name(f[1])) return std::nullopt;
    c.name = f[1];
    return "SERVERS," + servers_list();
}

// SERVERS,<id>,<ip>,<port>;<id>,<ip>,<port>;... (a reply to our HELO: nothing to send back)
Server::Answer Server::p_servers(const std::string& payload) {
    const std::string prefix = "SERVERS,";
    if (payload.compare(0, prefix.size(), prefix) != 0) return std::nullopt;

    std::string list = payload.substr(prefix.size());
    for (const std::string& entry : split_on(list, ';')) {
        if (entry.empty()) continue;

        std::string id, ip;
        uint16_t port = 0;
        if (!parse_server_entry(entry, id, ip, port)) {
            log_note("ignored bad SERVERS entry: %s", entry.c_str());
            continue;
        }
        if (id == GROUP_ID) continue;

        bool is_new = known_.count(id) == 0;
        if (is_new && known_.size() >= MAX_KNOWN_SERVERS) continue; // Cap on known server, a hostile server can fill our memory

        Endpoint& ep = known_[id];   // update in place: keeps next_try of a server we already knew
        ep.ip = ip;
        ep.port = port;
        if (is_new) log_note("learned server %s at %s:%u", id.c_str(), ip.c_str(), port);
    }
    return std::string();
}

Server::Answer Server::p_statusreq() { return "STATUSRESP," + c_statusreq(); }

// KEEPALIVE,<n>: the peer holds n messages for us. n > 0 means fetch them (pull model).
Server::Answer Server::p_keepalive(const std::vector<std::string>& f) {
    if (f.size() != 2) return std::nullopt;

    // At most 6 digits, so stoi cannot throw on a hostile number.
    if (f[1].find_first_not_of("0123456789") != std::string::npos
        || f[1].empty()
        || f[1].size() > 6) return std::nullopt;

    if (std::stoi(f[1]) > 0) return "GETMSGS," GROUP_ID;

    return "";

}
// STATUSRESP,<group>:<n>,...   (the answer to the STATUSREQ we send every 90 s)
// This is how we relay: if the peer holds mail for us, or for a server we are
// connected to, we fetch it with GETMSGS. Mail for a connected peer then goes out on
// our next KEEPALIVE to that peer, and the peer fetches it from us.
Server::Answer Server::p_statusresp(int fd, const std::vector<std::string>& f) {
    if (f.size() < 2) return std::nullopt;
    if (f[1] == "EMPTY") return "";

    for (size_t i = 1; i< f.size(); i++) {
        std::vector<std::string> parts = split_on(f[i], ':');
        if (parts.size() != 2 || !is_valid_name(parts[0])) continue;

        // only groups it really holds messages for
        if (parts[1].empty() || parts[1] == "0" ||
            parts[1].find_first_not_of("0123456789") != std::string::npos) continue;

        if (parts[0] == clients_[fd].name) continue; // its own messages: it delivers those itself
        if (parts[0] != GROUP_ID && !is_connected_peer(parts[0])) continue;

        if (!clients_.count(fd)) break;
        queue_send(fd, "GETMSGS," + parts[0]);
    }

    return "";
}

// GETMSGS,<group>   -> one SENDMSG per message we hold for <group>, then they are deleted.
// No answer at all if we hold none. There is no acknowledgement in the protocol, so a
// message is lost if the connection drops right after sending (best effort, as agreed).
Server::Answer Server::p_getmsgs(int fd, const std::vector<std::string>& f) {
    if (f.size() != 2 || !is_valid_name(f[1])) return std::nullopt; 
    auto it = mailboxes_.find(f[1]);
    if (it == mailboxes_.end()) return "";

    for (const Message& msg : it->second){
        if (!clients_.count(fd))break; 
        std::string msg_string = "SENDMSG," + msg.from + "," + msg.to + "," + std::to_string(msg.timestamp) + "," + msg.text;
        queue_send(fd, msg_string);
    }

    log_note("sent %zu held message(s) to %s", it->second.size(), f[1].c_str());
    stored_ -= it->second.size();
    mailboxes_.erase(it);
    return "";
}

// SENDMSG,<FROM_GROUP_ID>,<TO_GROUP_ID>,<TIMESTAMP>,<message content>
// Accepted on any connection. Every message is stored under its recipient: mail for us
// waits for our client's GETMSG, mail for others waits until the recipient (or a relay)
// fetches it. The timestamp is kept as the sender set it. split_fields(payload, 5)
// leaves any commas inside the content.
Server::Answer Server::p_sendmsg(const std::vector<std::string>& f) {
    if (f.size() != 5 || !is_valid_name(f[1])  || !is_valid_name(f[2])) return std::nullopt; 

    if (f[3].find_first_not_of("0123456789") != std::string::npos
        || f[3].empty()
        || f[3].size() > 10
        ) return std::nullopt;

    time_t ts = std::stoll(f[3]);   // safe: at most 10 digits

    // Caps per recipient and in total, so a flooding server cannot exhaust our memory.
    auto it = mailboxes_.find(f[2]);
    size_t held = (it == mailboxes_.end()) ? 0 : it->second.size();
    if (held >= MAX_PER_GROUP || stored_ >= MAX_STORED) {
        log_note("store full, discarded message from %s for %s", f[1].c_str(), f[2].c_str()); // we recieved but couldn't store due to possible flooding of messages per client.
        return ""; 
    }
    
    Message msg{f[1], f[2], ts, f[4]};
    mailboxes_[f[2]].push_back(msg);
    stored_++;
    log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
        f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
    return "";
}