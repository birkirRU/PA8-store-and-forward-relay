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

bool parse_server_entry(const std::string& entry, std::string& id, std::string& ip, uint16_t& port) {
    std::vector<std::string> parts = split_fields(entry);
    if (parts.size() != 3) return false;

    id = parts[0];
    ip = parts[1];
    const std::string& port_text = parts[2];

    if (!is_valid_name(id)) return false;

    in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;

    if (port_text.empty() || port_text.size() > 5) return false;
    if (port_text.find_first_not_of("0123456789") != std::string::npos) return false;
    int value = std::stoi(port_text);
    if (value < 1 || value > 65535) return false;

    port = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

void Server::handle_command(int fd, const std::string& payload) {
    Client& c = clients_[fd];
    std::string cmd = payload.substr(0, payload.find(','));
    if (c.type.value == ConnType::unknown)
        c.type.value = (cmd == "HELO") ? ConnType::peer : ConnType::client;

    const bool is_peer = c.type.value == ConnType::peer;
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

    log_event_bytes("RECV", c.ip.c_str(), c.port, id, payload.data(), payload.size());
    if (!answer->empty()) queue_send(fd, *answer);

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

    return answer;
}

// SENDMSG,<from>,<to>,<message>   -> "OK"
std::string Server::c_sendmsg(const std::vector<std::string>& f) {
    if (f.size() != 4 || !is_valid_name(f[1]) || !is_valid_name(f[2])) return "";

    Message msg{f[1], f[2], std::time(nullptr), f[3]};

    // --------------- MAY DELETE (push behvaiour) -------------------------
    // // Before deciding wether to 
    // // store or forward the message; check if recepiant peer exists (is connected to server)
    // int peer_fd = -1;
    // for (const auto& kv : clients_) {
    //     const Client& c = kv.second;
    //     if (c.type.value == ConnType::peer && // its a peer connection
    //         !c.connecting && // peer connection is established
    //         !c.name.empty() && // name is set
    //         c.name == f[2]
    //     )
    //         peer_fd = kv.first;
    // }
    
    // if (peer_fd != -1 ) { // it exists
    //     Client& c = clients_[peer_fd];

    //     // SENDMSG,<FROM_GROUP_ID>,<TO_GROUP_ID>,<TIMESTAMP>,<message content>
    //     std::string msg_string = f[0] + "," + msg.from + "," + msg.to + "," + std::to_string(msg.timestamp) + "," + msg.text;
    //     queue_send(peer_fd, msg_string);
    // }

    // else {  // no connected peer was found to deliver the message to, store it for time being, waiting for other peers to fetch it
    //     mailboxes_[f[2]].push_back(msg);
    //     stored_++;
    //     log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
    //         f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
    // }
    // --------------- END OF MAY DELETE -------------------------
    mailboxes_[f[2]].push_back(msg);
    stored_++;
    log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
        f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
    return "OK";
}

// GETMSG,<user>   -> "<from>,<message>", or "NO_MESSAGE"
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
    if (cmd == "STATUSRESP") return p_statusresp(split_fields(payload));
    if (cmd == "STATUSREQ")  return payload == "STATUSREQ" ? p_statusreq() : Answer{};
    if (cmd == "ERROR") {
        log_note("peer %s sent %s", c.name.c_str(), payload.c_str());
        return std::string();
    }
    return std::nullopt;
}

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
        if (is_new && known_.size() >= 500) continue; // Cap on known server, a hostile server can fill our memory

        known_[id] = Endpoint{ip, port};
        if (is_new) log_note("learned server %s at %s:%u", id.c_str(), ip.c_str(), port);
    }
    return std::string();
}

Server::Answer Server::p_statusreq() { return "STATUSRESP," + c_statusreq(); }

Server::Answer Server::p_keepalive(const std::vector<std::string>& f) {
    return f.size() == 2 ? Answer{""} : Answer{};
}

Server::Answer Server::p_statusresp(const std::vector<std::string>& f) {
    return f.size() >= 2 ? Answer{""} : Answer{};
}

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
Server::Answer Server::p_sendmsg(const std::vector<std::string>& f) { 
    if (f.size() != 5 || !is_valid_name(f[1])  || !is_valid_name(f[2])) return std::nullopt; 

    if (f[3].find_first_not_of("0123456789") != std::string::npos
        || f[3].empty()
        || f[3].size() > 10
        ) return std::nullopt;

    time_t ts = std::stoll(f[3]);
    
    if (f[2] == GROUP_ID) {
        Message msg{f[1], f[2], ts, f[4]};
        mailboxes_[GROUP_ID].push_back(msg);
        stored_++;
        log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
            f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
        return "";
    }

    return std::nullopt;
}