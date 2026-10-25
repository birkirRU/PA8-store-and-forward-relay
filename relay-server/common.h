#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Code shared by tsamserver and tsamclient: framing, command parsing and small helpers.

// ---------------------------------------------------------------------------
// Framing: every command and reply travels as <SOH><length><STX><payload><ETX>.
// <length> is 16 bits, big-endian, and counts the whole frame (framing included).
// ---------------------------------------------------------------------------
const char SOH = 0x01;
const char STX = 0x02;
const char ETX = 0x03;
const size_t FRAME_OVERHEAD = 5;                           // SOH + 2 length bytes + STX + ETX
const size_t MAX_PAYLOAD = 5000;                           // longest command/reply we accept
const size_t MAX_FRAME = MAX_PAYLOAD + FRAME_OVERHEAD;

// payload -> frame bytes. Empty string if the payload is longer than MAX_PAYLOAD.
std::string encode_frame(const std::string& payload);

// Removes every complete frame from the front of `buf` and appends its payload to
// `payloads`. An incomplete frame at the end stays in `buf` until more bytes arrive.
// Malformed bytes are dropped; each drop adds a reason to `problems`.
void extract_frames(std::string& buf, std::vector<std::string>& payloads,
                    std::vector<std::string>& problems);

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Splits "SENDMSG,alice,bob,hi, there" into at most `max_fields` fields; the last
// field keeps any remaining commas: {"SENDMSG","alice","bob","hi, there"}.
std::vector<std::string> split_fields(const std::string& s, size_t max_fields);

// A username: 1-14 ASCII letters + '0' - '9' chars + '_'. Keeps STATUSREQ replies ("bob:2,alice:1") unambiguous.
bool is_valid_name(const std::string& s);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Parses a TCP port (1-65535). Prints an error and exits on invalid input.
unsigned int parse_port(const char* str);

// Local time for the client printout: "2026-10-12 18:23:45".
std::string timestamp();

// Sends all of `data`, retrying partial sends. false = connection broken (errno set).
bool send_all(int fd, const std::string& data);
