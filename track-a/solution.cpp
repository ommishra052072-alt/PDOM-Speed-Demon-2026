#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <chrono>

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

// 1. Endpoint Interning Table
struct EndpointTable {
    static constexpr size_t BUCKETS = 8192;
    struct Node {
        std::string endpoint;
        uint16_t id;
        Node* next = nullptr;
    };

    std::vector<Node*> table;
    std::vector<std::string> endpoints_by_id;
    std::vector<Node*> all_nodes;

    EndpointTable() : table(BUCKETS, nullptr) {
        endpoints_by_id.reserve(4096);
        all_nodes.reserve(4096);
    }

    ~EndpointTable() {
        for (Node* n : all_nodes) delete n;
    }

    static inline uint64_t hash_endpoint(std::string_view s) {
        uint64_t h = 14695981039346656037ULL;
        for (char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    inline uint16_t get_or_intern(std::string_view ep) {
        uint64_t h = hash_endpoint(ep);
        size_t bucket = h & (BUCKETS - 1);

        Node* curr = table[bucket];
        while (curr) {
            if (curr->endpoint == ep) {
                return curr->id;
            }
            curr = curr->next;
        }

        uint16_t new_id = static_cast<uint16_t>(endpoints_by_id.size());
        Node* node = new Node();
        node->endpoint = std::string(ep);
        node->id = new_id;
        node->next = table[bucket];
        table[bucket] = node;

        endpoints_by_id.push_back(node->endpoint);
        all_nodes.push_back(node);
        return new_id;
    }
};

// 2. Hour Interning Table
struct HourTable {
    static constexpr size_t BUCKETS = 256;
    struct Node {
        std::string hour_str;
        uint16_t id;
        Node* next = nullptr;
    };

    std::vector<Node*> table;
    std::vector<std::string> hours_by_id;
    std::vector<Node*> all_nodes;

    HourTable() : table(BUCKETS, nullptr) {
        hours_by_id.reserve(256);
        all_nodes.reserve(256);
    }

    ~HourTable() {
        for (Node* n : all_nodes) delete n;
    }

    static inline uint64_t hash_hour(std::string_view s) {
        uint64_t h = 14695981039346656037ULL;
        for (char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    inline uint16_t get_or_intern(std::string_view hour) {
        uint64_t h = hash_hour(hour);
        size_t bucket = h & (BUCKETS - 1);

        Node* curr = table[bucket];
        while (curr) {
            if (curr->hour_str == hour) {
                return curr->id;
            }
            curr = curr->next;
        }

        uint16_t new_id = static_cast<uint16_t>(hours_by_id.size());
        Node* node = new Node();
        node->hour_str = std::string(hour);
        node->id = new_id;
        node->next = table[bucket];
        table[bucket] = node;

        hours_by_id.push_back(node->hour_str);
        all_nodes.push_back(node);
        return new_id;
    }
};

// 3. Compact Integer Index Table + Dense Active Groups
struct GroupData {
    uint16_t hour_id = 0;
    uint16_t endpoint_id = 0;
    uint32_t errors = 0;
    std::vector<uint32_t> latencies;
};

struct GroupStorage {
    static constexpr uint32_t SENTINEL = 0xFFFFFFFF;

    size_t hour_cap = 80;
    size_t endpoint_cap = 2048;
    std::vector<uint32_t> index_table;
    std::vector<GroupData> active_groups;

    GroupStorage() {
        index_table.assign(hour_cap * endpoint_cap, SENTINEL);
        active_groups.reserve(131072);
    }

    void grow_table(size_t needed_hour, size_t needed_ep) {
        size_t new_h_cap = hour_cap;
        size_t new_ep_cap = endpoint_cap;
        while (needed_hour >= new_h_cap) new_h_cap *= 2;
        while (needed_ep >= new_ep_cap) new_ep_cap *= 2;

        std::vector<uint32_t> new_index_table(new_h_cap * new_ep_cap, SENTINEL);

        for (uint32_t g_idx = 0; g_idx < active_groups.size(); ++g_idx) {
            const GroupData& g = active_groups[g_idx];
            size_t new_idx = static_cast<size_t>(g.hour_id) * new_ep_cap + g.endpoint_id;
            new_index_table[new_idx] = g_idx;
        }

        index_table = std::move(new_index_table);
        hour_cap = new_h_cap;
        endpoint_cap = new_ep_cap;
    }

    inline GroupData& get(uint16_t hour_id, uint16_t ep_id) {
        if (__builtin_expect(hour_id >= hour_cap || ep_id >= endpoint_cap, 0)) {
            grow_table(hour_id, ep_id);
        }
        size_t table_idx = static_cast<size_t>(hour_id) * endpoint_cap + ep_id;
        uint32_t g_idx = index_table[table_idx];

        if (g_idx == SENTINEL) {
            g_idx = static_cast<uint32_t>(active_groups.size());
            index_table[table_idx] = g_idx;
            active_groups.emplace_back();
            GroupData& g = active_groups.back();
            g.hour_id = hour_id;
            g.endpoint_id = ep_id;
            return g;
        }

        return active_groups[g_idx];
    }
};

// Fast percentile using nearest rank rule: 1-indexed ceil(p * n / 100)
// With 0-indexed vector: ((p * n + 99) / 100) - 1
inline uint32_t get_percentile(const std::vector<uint32_t>& sorted_vals, uint32_t p) {
    size_t n = sorted_vals.size();
    size_t rank = (p * n + 99) / 100;
    return sorted_vals[rank - 1];
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    auto t_start = clock_type::now();

    std::string data_dir = "/data";
    std::string out_dir = "/out";

    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) out_dir = argv[2];

    fs::path input_path = fs::path(data_dir) / "requests.log";
    fs::path output_path = fs::path(out_dir) / "summary.txt";

    int fd = open(input_path.c_str(), O_RDONLY);
    if (fd < 0) {
        fs::create_directories(output_path.parent_path());
        std::ofstream out(output_path, std::ios::binary);
        return 0;
    }

    struct stat sb;
    if (fstat(fd, &sb) < 0) { close(fd); return 1; }

    size_t file_size = sb.st_size;
    if (file_size == 0) {
        close(fd);
        fs::create_directories(output_path.parent_path());
        std::ofstream out(output_path, std::ios::binary);
        return 0;
    }

    const char* data = static_cast<const char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) {
        close(fd);
        return 1;
    }

    madvise(const_cast<char*>(data), file_size, MADV_SEQUENTIAL);

    EndpointTable ep_table;
    HourTable hour_table;
    GroupStorage storage;

    const char* ptr = data;
    const char* end = data + file_size;

    char hour_buf[21];
    hour_buf[20] = '\0';
    memcpy(hour_buf + 13, ":00:00Z", 7);

    while (ptr < end) {
        if (end - ptr < 14) break;

        memcpy(hour_buf, ptr, 13);
        std::string_view hour(hour_buf, 20);

        const char* p_space1 = static_cast<const char*>(memchr(ptr, ' ', end - ptr));
        if (!p_space1) break;

        const char* p_method = p_space1 + 1;
        const char* p_space2 = static_cast<const char*>(memchr(p_method, ' ', end - p_method));
        if (!p_space2) break;

        const char* p_ep = p_space2 + 1;
        const char* p_space3 = static_cast<const char*>(memchr(p_ep, ' ', end - p_ep));
        if (!p_space3) break;
        std::string_view endpoint(p_ep, p_space3 - p_ep);

        const char* p_status = p_space3 + 1;
        const char* p_space4 = static_cast<const char*>(memchr(p_status, ' ', end - p_status));
        if (!p_space4) break;

        uint32_t status = 0;
        for (const char* sp = p_status; sp < p_space4; ++sp) {
            status = status * 10 + (*sp - '0');
        }

        const char* p_lat = p_space4 + 1;
        const char* p_nl = static_cast<const char*>(memchr(p_lat, '\n', end - p_lat));
        if (!p_nl) p_nl = end;

        uint32_t latency = 0;
        for (const char* lp = p_lat; lp < p_nl; ++lp) {
            if (*lp >= '0' && *lp <= '9') {
                latency = latency * 10 + (*lp - '0');
            }
        }

        uint16_t h_id = hour_table.get_or_intern(hour);
        uint16_t ep_id = ep_table.get_or_intern(endpoint);

        auto& group = storage.get(h_id, ep_id);
        group.latencies.push_back(latency);
        if (status >= 500 && status <= 599) {
            group.errors++;
        }

        ptr = (p_nl < end) ? p_nl + 1 : end;
    }

    munmap(const_cast<char*>(data), file_size);
    close(fd);

    struct ActiveItem {
        std::string_view hour_str;
        std::string_view endpoint_str;
        GroupData* entry;

        bool operator<(const ActiveItem& o) const {
            if (hour_str != o.hour_str) return hour_str < o.hour_str;
            return endpoint_str < o.endpoint_str;
        }
    };

    std::vector<ActiveItem> active_items;
    active_items.reserve(storage.active_groups.size());

    for (GroupData& g : storage.active_groups) {
        active_items.push_back({
            hour_table.hours_by_id[g.hour_id],
            ep_table.endpoints_by_id[g.endpoint_id],
            &g
        });
    }

    std::sort(active_items.begin(), active_items.end());

    fs::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) return 1;

    std::string out_buf;
    out_buf.reserve(1024 * 1024);

    for (const auto& item : active_items) {
        auto* g = item.entry;
        std::sort(g->latencies.begin(), g->latencies.end());
        size_t count = g->latencies.size();
        uint32_t p50 = get_percentile(g->latencies, 50);
        uint32_t p95 = get_percentile(g->latencies, 95);
        uint32_t p99 = get_percentile(g->latencies, 99);
        uint32_t errs = g->errors;
        uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;

        out_buf += item.hour_str;
        out_buf += ' ';
        out_buf += item.endpoint_str;
        out_buf += ' ';
        out_buf += std::to_string(count);
        out_buf += ' ';
        out_buf += std::to_string(p50);
        out_buf += ' ';
        out_buf += std::to_string(p95);
        out_buf += ' ';
        out_buf += std::to_string(p99);
        out_buf += ' ';
        out_buf += std::to_string(errs);
        out_buf += ' ';
        out_buf += std::to_string(ppm);
        out_buf += '\n';

        if (out_buf.size() >= 512 * 1024) {
            out.write(out_buf.data(), out_buf.size());
            out_buf.clear();
        }
    }

    if (!out_buf.empty()) {
        out.write(out_buf.data(), out_buf.size());
    }
    out.flush();

    auto t_end = clock_type::now();
    double exec_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    std::cerr << "PROD_RUN_RESULT: exec_ms=" << exec_ms
              << " peak_rss_kb=" << usage.ru_maxrss
              << " active_groups=" << storage.active_groups.size()
              << "\n";

    return 0;
}
