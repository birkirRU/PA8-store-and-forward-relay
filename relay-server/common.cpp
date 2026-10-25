#include "common.h"

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

std::string encode_frame(const std::string& payload) {
    if (payload.size() > MAX_PAYLOAD) return "";
    size_t total = payload.size() + FRAME_OVERHEAD;

    std::string frame;
    frame.reserve(total);
    frame += SOH;
    frame += static_cast<char>((total >> 8) & 0xFF);  // high byte first = network byte order
    frame += static_cast<char>(total & 0xFF);
    frame += STX;
    frame += payload;
    frame += ETX;
    return frame;
}

void extract_frames(std::string& buf, std::vector<std::string>& payloads,
                    std::vector<std::string>& problems) {
    while (!buf.empty()) {
        // A frame must start with SOH: skip anything before the next one.
        if (buf[0] != SOH) {
            size_t next = buf.find(SOH);
            size_t n = (next == std::string::npos) ? buf.size() : next;
            buf.erase(0, n);
            problems.push_back("garbage outside a frame (" + std::to_string(n) + " bytes)");
            continue;
        }
        if (buf.size() < 3) break;  // length bytes not here yet

        size_t len = (static_cast<unsigned char>(buf[1]) << 8) | static_cast<unsigned char>(buf[2]);

        // On a bad header drop just the SOH and look for the next frame.
        if (len < FRAME_OVERHEAD || len > MAX_FRAME) {
            buf.erase(0, 1);
            problems.push_back("invalid frame length " + std::to_string(len));
            continue;
        }
        if (buf.size() >= 4 && buf[3] != STX) {
            buf.erase(0, 1);
            problems.push_back("missing STX");
            continue;
        }
        if (buf.size() < len) break;  // rest of the frame not here yet

        if (buf[len - 1] != ETX) {
            buf.erase(0, 1);
            problems.push_back("missing ETX");
            continue;
        }

        payloads.push_back(buf.substr(4, len - FRAME_OVERHEAD));
        buf.erase(0, len);
    }
}

std::vector<std::string> split_fields(const std::string& s, size_t max_fields) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (fields.size() + 1 < max_fields) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) break;
        fields.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    fields.push_back(s.substr(start));  // the rest, commas included
    return fields;
}

bool is_valid_name(const std::string& s) {
    if (s.size() < 1 || s.size() > 14) return false;
    for (char c : s)
        if (!(
            (c >= 'a' && c <= 'z') ||  
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || 
            (c == '_') ||
            (c == '-')))
            return false;
    return true;
}

unsigned int parse_port(const char* str) {
    char* end;
    errno = 0;
    unsigned long port = std::strtoul(str, &end, 10);
    if (end == str || *end != '\0' || errno == ERANGE || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port '%s' (must be 1-65535)\n", str);
        exit(1);
    }
    return static_cast<unsigned int>(port);
}

static std::string format_now(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    size_t n = std::strftime(buf, sizeof buf, fmt, &tm);
    return std::string(buf, n);
}

std::string timestamp()     { return format_now("%Y-%m-%d %H:%M:%S"); }
std::string log_timestamp() { return format_now("%d/%b/%Y:%H:%M:%S %z"); }

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}
