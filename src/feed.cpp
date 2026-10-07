#include "feed.h"

#include <chrono>
#include <cstring>
#include <fstream>

namespace ob {
namespace {

// bounds-checked little-endian readers. memcpy-free, no alignment worries.
bool read_u64(const std::vector<char>& buf, size_t& pos, uint64_t& out) {
    if (pos + 8 > buf.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i)
        out |= static_cast<uint64_t>(static_cast<unsigned char>(buf[pos + i])) << (8 * i);
    pos += 8;
    return true;
}

bool read_i64(const std::vector<char>& buf, size_t& pos, int64_t& out) {
    uint64_t u;
    if (!read_u64(buf, pos, u)) return false;
    std::memcpy(&out, &u, 8);
    return true;
}

bool read_u8(const std::vector<char>& buf, size_t& pos, uint8_t& out) {
    if (pos + 1 > buf.size()) return false;
    out = static_cast<uint8_t>(buf[pos]);
    pos += 1;
    return true;
}

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

}  // namespace

bool replay_feed(const std::string& path, OrderBook& book,
                 std::vector<uint64_t>& latencies_ns, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "can't open file: " + path;
        return false;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());

    if (buf.size() < 4 || std::string(buf.data(), 4) != "OBF1") {
        error = "bad magic, not an OBF1 feed file";
        return false;
    }

    size_t pos = 4;
    size_t msg_no = 0;
    while (pos < buf.size()) {
        uint8_t type;
        if (!read_u8(buf, pos, type)) {
            error = "truncated message header at message " + std::to_string(msg_no);
            return false;
        }
        uint64_t ts, id;
        uint8_t side_raw;
        int64_t price, qty;

        uint64_t t0 = now_ns();
        if (type == 'A') {
            if (!read_u64(buf, pos, ts) || !read_u64(buf, pos, id) ||
                !read_u8(buf, pos, side_raw) || !read_i64(buf, pos, price) ||
                !read_i64(buf, pos, qty)) {
                error = "truncated 'A' message at message " + std::to_string(msg_no);
                return false;
            }
            if (side_raw > 1) {
                error = "bad side byte at message " + std::to_string(msg_no);
                return false;
            }
            book.add_limit(id, static_cast<Side>(side_raw), price, qty);
        } else if (type == 'X') {
            if (!read_u64(buf, pos, ts) || !read_u64(buf, pos, id)) {
                error = "truncated 'X' message at message " + std::to_string(msg_no);
                return false;
            }
            book.cancel(id);
        } else if (type == 'M') {
            if (!read_u64(buf, pos, ts) || !read_u64(buf, pos, id) ||
                !read_u8(buf, pos, side_raw) || !read_i64(buf, pos, qty)) {
                error = "truncated 'M' message at message " + std::to_string(msg_no);
                return false;
            }
            if (side_raw > 1) {
                error = "bad side byte at message " + std::to_string(msg_no);
                return false;
            }
            book.add_market(id, static_cast<Side>(side_raw), qty);
        } else {
            error = "unknown message type '" + std::string(1, static_cast<char>(type)) +
                    "' at message " + std::to_string(msg_no);
            return false;
        }
        // (void)ts — timestamps are in the file for realism; the replay
        // doesn't do anything with them yet. a real feed handler would use
        // them for gap detection / sequencing. (void) to keep -Wall happy.
        (void)ts;
        latencies_ns.push_back(now_ns() - t0);
        ++msg_no;
    }
    return true;
}

}  // namespace ob
