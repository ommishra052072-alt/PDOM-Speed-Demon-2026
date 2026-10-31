// solution_a05.cpp
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
#include <thread>
#include <chrono>
#include <stdexcept>

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

struct EndpointTable {
    static constexpr size_t BUCKETS = 8192;
    struct Node { std::string endpoint; uint16_t id; Node* next = nullptr; };
    std::vector<Node*> table; std::vector<std::string> endpoints_by_id; std::vector<Node*> all_nodes;
    EndpointTable() : table(BUCKETS, nullptr) { endpoints_by_id.reserve(256); all_nodes.reserve(256); }
    ~EndpointTable() { for (Node* n : all_nodes) delete n; }
    static inline uint64_t hash_endpoint(std::string_view s) {
        uint64_t h = 14695981039346656037ULL; for (char c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ULL; } return h;
    }
    inline uint16_t get_or_intern(std::string_view ep) {
        uint64_t h = hash_endpoint(ep); size_t bucket = h & (BUCKETS - 1);
        Node* curr = table[bucket]; while (curr) { if (curr->endpoint == ep) return curr->id; curr = curr->next; }
        
        size_t next_id = endpoints_by_id.size();
        if (next_id > 65535) throw std::overflow_error("EndpointTable uint16_t ID overflow");
        
        uint16_t new_id = static_cast<uint16_t>(next_id);
        Node* node = new Node(); node->endpoint = std::string(ep); node->id = new_id; node->next = table[bucket];
        table[bucket] = node; endpoints_by_id.push_back(node->endpoint); all_nodes.push_back(node); return new_id;
    }
};

struct HourTable {
    static constexpr size_t BUCKETS = 256;
    struct Node { std::string hour_str; uint16_t id; Node* next = nullptr; };
    std::vector<Node*> table; std::vector<std::string> hours_by_id; std::vector<Node*> all_nodes;
    HourTable() : table(BUCKETS, nullptr) { hours_by_id.reserve(32); all_nodes.reserve(32); }
    ~HourTable() { for (Node* n : all_nodes) delete n; }
    static inline uint64_t hash_hour(std::string_view s) {
        uint64_t h = 14695981039346656037ULL; for (char c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ULL; } return h;
    }
    inline uint16_t get_or_intern(std::string_view hour) {
        uint64_t h = hash_hour(hour); size_t bucket = h & (BUCKETS - 1);
        Node* curr = table[bucket]; while (curr) { if (curr->hour_str == hour) return curr->id; curr = curr->next; }
        
        size_t next_id = hours_by_id.size();
        if (next_id > 65535) throw std::overflow_error("HourTable uint16_t ID overflow");
        
        uint16_t new_id = static_cast<uint16_t>(next_id);
        Node* node = new Node(); node->hour_str = std::string(hour); node->id = new_id; node->next = table[bucket];
        table[bucket] = node; hours_by_id.push_back(node->hour_str); all_nodes.push_back(node); return new_id;
    }
};

struct GroupData { uint16_t hour_id = 0; uint16_t endpoint_id = 0; uint32_t errors = 0; std::vector<uint32_t> latencies; };

struct GroupStorage {
    static constexpr uint32_t SENTINEL = 0xFFFFFFFF;
    size_t hour_cap = 8; size_t endpoint_cap = 128;
    std::vector<uint32_t> index_table; std::vector<GroupData> active_groups;
    GroupStorage() { index_table.assign(hour_cap * endpoint_cap, SENTINEL); active_groups.reserve(256); }
    void grow_table(size_t needed_hour, size_t needed_ep) {
        size_t new_h_cap = hour_cap;
        size_t new_ep_cap = endpoint_cap;
        while (needed_hour >= new_h_cap) {
            new_h_cap *= 2;
        }
        while (needed_ep >= new_ep_cap) {
            new_ep_cap *= 2;
        }
        std::vector<uint32_t> new_index_table(new_h_cap * new_ep_cap, SENTINEL);
        for (uint32_t g_idx = 0; g_idx < active_groups.size(); ++g_idx) {
            const GroupData& g = active_groups[g_idx]; size_t new_idx = static_cast<size_t>(g.hour_id) * new_ep_cap + g.endpoint_id; new_index_table[new_idx] = g_idx;
        }
        index_table = std::move(new_index_table); hour_cap = new_h_cap; endpoint_cap = new_ep_cap;
    }
    inline GroupData& get(uint16_t hour_id, uint16_t ep_id) {
        if (__builtin_expect(hour_id >= hour_cap || ep_id >= endpoint_cap, 0)) grow_table(hour_id, ep_id);
        size_t table_idx = static_cast<size_t>(hour_id) * endpoint_cap + ep_id; uint32_t g_idx = index_table[table_idx];
        if (g_idx == SENTINEL) {
            g_idx = static_cast<uint32_t>(active_groups.size()); index_table[table_idx] = g_idx; active_groups.emplace_back();
            GroupData& g = active_groups.back(); g.hour_id = hour_id; g.endpoint_id = ep_id; return g;
        }
        return active_groups[g_idx];
    }
};

inline uint32_t get_percentile(const std::vector<uint32_t>& sorted_vals, uint32_t p) {
    size_t n = sorted_vals.size(); size_t rank = (p * n + 99) / 100; return sorted_vals[rank - 1];
}

struct ThreadLocalData { EndpointTable ep_table; HourTable hour_table; GroupStorage storage; double processing_ms = 0; };

void process_chunk(const char* ptr, const char* end, ThreadLocalData* local) {
    auto t_start = clock_type::now();
    char hour_buf[21]; hour_buf[20] = '\0'; memcpy(hour_buf + 13, ":00:00Z", 7);
    
    while (ptr < end) {
        const char* p_nl = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!p_nl) p_nl = end;

        const char* p_space1 = static_cast<const char*>(memchr(ptr, ' ', p_nl - ptr)); 
        if (!p_space1) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }

        size_t ts_len = p_space1 - ptr;
        bool ts_valid = false;
        
        if (ts_len == 27 || ts_len == 20) {
            ts_valid = true;
            for (int i = 0; i < 19; ++i) {
                if (i == 4 || i == 7) { if (ptr[i] != '-') { ts_valid = false; break; } }
                else if (i == 10) { if (ptr[i] != 'T') { ts_valid = false; break; } }
                else if (i == 13 || i == 16) { if (ptr[i] != ':') { ts_valid = false; break; } }
                else { if (ptr[i] < '0' || ptr[i] > '9') { ts_valid = false; break; } }
            }
            if (ts_valid) {
                if (ts_len == 20) {
                    if (ptr[19] != 'Z') ts_valid = false;
                } else {
                    if (ptr[19] != '.') {
                        ts_valid = false;
                    } else {
                        for (int i = 20; i < 26; ++i) {
                            if (ptr[i] < '0' || ptr[i] > '9') { ts_valid = false; break; }
                        }
                        if (ptr[26] != 'Z') ts_valid = false;
                    }
                }
            }
        }
        
        if (!ts_valid) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        
        const char* p_method = p_space1 + 1;
        if (p_method >= p_nl) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        const char* p_space2 = static_cast<const char*>(memchr(p_method, ' ', p_nl - p_method)); 
        if (!p_space2 || p_space2 == p_method) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        
        const char* p_ep = p_space2 + 1;
        if (p_ep >= p_nl) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        const char* p_space3 = static_cast<const char*>(memchr(p_ep, ' ', p_nl - p_ep)); 
        if (!p_space3 || p_space3 == p_ep) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        std::string_view endpoint(p_ep, p_space3 - p_ep);
        
        const char* p_status = p_space3 + 1;
        if (p_status >= p_nl) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        const char* p_space4 = static_cast<const char*>(memchr(p_status, ' ', p_nl - p_status)); 
        if (!p_space4 || p_space4 == p_status) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        
        uint32_t status = 0; 
        bool status_valid = true;
        for (const char* sp = p_status; sp < p_space4; ++sp) {
            if (*sp < '0' || *sp > '9') { status_valid = false; break; }
            uint32_t digit = *sp - '0';
            if (status > (UINT32_MAX - digit) / 10) { status_valid = false; break; }
            status = status * 10 + digit;
        }
        if (!status_valid) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        
        const char* p_lat = p_space4 + 1;
        if (p_lat >= p_nl) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        uint32_t latency = 0; 
        bool lat_valid = true;
        bool has_latency = false;
        for (const char* lp = p_lat; lp < p_nl; ++lp) {
            if (*lp < '0' || *lp > '9') { lat_valid = false; break; }
            uint32_t digit = *lp - '0';
            if (latency > (UINT32_MAX - digit) / 10) { lat_valid = false; break; }
            latency = latency * 10 + digit;
            has_latency = true;
        }
        if (!lat_valid || !has_latency) { ptr = (p_nl < end) ? p_nl + 1 : end; continue; }
        
        memcpy(hour_buf, ptr, 13);
        std::string_view hour(hour_buf, 20);

        static thread_local char last_hour_str[21] = {0}; static thread_local uint16_t cached_h_id = 0; uint16_t h_id;
        if (std::memcmp(last_hour_str, hour_buf, 13) == 0) { h_id = cached_h_id; } 
        else { h_id = local->hour_table.get_or_intern(hour); std::memcpy(last_hour_str, hour_buf, 13); cached_h_id = h_id; }
        
        uint16_t ep_id = local->ep_table.get_or_intern(endpoint);
        auto& group = local->storage.get(h_id, ep_id);
        group.latencies.push_back(latency);
        if (status >= 500 && status <= 599) group.errors++;
        
        ptr = (p_nl < end) ? p_nl + 1 : end;
    }
    local->processing_ms = std::chrono::duration<double, std::milli>(clock_type::now() - t_start).count();
}

int main(int argc, char* argv[]) {
    auto t_main_start = clock_type::now();
    std::string data_dir = "/data", out_dir = "/out";
    if (argc >= 2) {
        data_dir = argv[1];
    }
    if (argc >= 3) {
        out_dir = argv[2];
    }
    
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (argc >= 4) {
        try {
            int parsed = std::stoi(argv[3]);
            num_threads = std::max(1, std::min(64, parsed));
        } catch (...) {
            num_threads = num_threads > 0 ? num_threads : 4;
        }
    } else {
        num_threads = num_threads > 0 ? num_threads : 4;
    }
    
    fs::path input_path = fs::path(data_dir) / "requests.log";
    fs::path output_path = fs::path(out_dir) / "summary.txt";
    int fd = open(input_path.c_str(), O_RDONLY);
    if (fd < 0) { fs::create_directories(output_path.parent_path()); std::ofstream out(output_path, std::ios::binary); return 0; }
    struct stat sb; if (fstat(fd, &sb) < 0) { close(fd); return 1; }
    size_t file_size = sb.st_size;
    if (file_size == 0) { close(fd); fs::create_directories(output_path.parent_path()); std::ofstream out(output_path, std::ios::binary); return 0; }
    const char* data = static_cast<const char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    
    if (data == MAP_FAILED) {
        close(fd);
        return 1;
    }
    
    madvise(const_cast<char*>(data), file_size, MADV_SEQUENTIAL);

    auto t_init_start = clock_type::now();
    
    std::vector<std::thread> threads;
    std::vector<ThreadLocalData> thread_data(num_threads);
    size_t chunk_size = file_size / num_threads;
    const char* chunk_start = data;

    for (unsigned int i = 0; i < num_threads; ++i) {
        const char* chunk_end = (i == num_threads - 1) ? data + file_size : chunk_start + chunk_size;
        if (i < num_threads - 1) {
            while (chunk_end < data + file_size && *chunk_end != '\n') ++chunk_end;
            if (chunk_end < data + file_size) ++chunk_end; 
        }
        if (chunk_start < chunk_end) {
            threads.emplace_back(process_chunk, chunk_start, chunk_end, &thread_data[i]);
        }
        chunk_start = chunk_end;
    }
    auto t_init_end = clock_type::now();

    for (auto& t : threads) { if (t.joinable()) t.join(); }
    
    auto t_merge_start = clock_type::now();
    EndpointTable global_ep_table; HourTable global_hour_table; GroupStorage global_storage;
    for (auto& local : thread_data) {
        for (GroupData& lg : local.storage.active_groups) {
            std::string_view h_str = local.hour_table.hours_by_id[lg.hour_id];
            std::string_view ep_str = local.ep_table.endpoints_by_id[lg.endpoint_id];
            uint16_t global_h_id = global_hour_table.get_or_intern(h_str);
            uint16_t global_ep_id = global_ep_table.get_or_intern(ep_str);
            
            auto& gg = global_storage.get(global_h_id, global_ep_id);
            gg.errors += lg.errors;
            gg.latencies.insert(gg.latencies.end(), lg.latencies.begin(), lg.latencies.end());
        }
    }
    auto t_merge_end = clock_type::now();
    
    munmap(const_cast<char*>(data), file_size); close(fd);

    struct ActiveItem {
        std::string_view hour_str; std::string_view endpoint_str; GroupData* entry;
        bool operator<(const ActiveItem& o) const { if (hour_str != o.hour_str) return hour_str < o.hour_str; return endpoint_str < o.endpoint_str; }
    };
    std::vector<ActiveItem> active_items; active_items.reserve(global_storage.active_groups.size());
    for (GroupData& g : global_storage.active_groups) active_items.push_back({ global_hour_table.hours_by_id[g.hour_id], global_ep_table.endpoints_by_id[g.endpoint_id], &g });
    std::sort(active_items.begin(), active_items.end());

    fs::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    std::string out_buf; out_buf.reserve(1024 * 1024);

    for (const auto& item : active_items) {
        auto* g = item.entry;
        std::sort(g->latencies.begin(), g->latencies.end());
        size_t count = g->latencies.size();
        uint32_t p50 = get_percentile(g->latencies, 50);
        uint32_t p95 = get_percentile(g->latencies, 95);
        uint32_t p99 = get_percentile(g->latencies, 99);
        uint32_t errs = g->errors;
        uint64_t ppm = (static_cast<uint64_t>(errs) * 1000000ULL) / count;

        out_buf += item.hour_str; out_buf += ' '; out_buf += item.endpoint_str; out_buf += ' ';
        out_buf += std::to_string(count); out_buf += ' '; out_buf += std::to_string(p50); out_buf += ' ';
        out_buf += std::to_string(p95); out_buf += ' '; out_buf += std::to_string(p99); out_buf += ' ';
        out_buf += std::to_string(errs); out_buf += ' '; out_buf += std::to_string(ppm); out_buf += '\n';
        if (out_buf.size() >= 512 * 1024) { out.write(out_buf.data(), out_buf.size()); out_buf.clear(); }
    }
    if (!out_buf.empty()) out.write(out_buf.data(), out_buf.size());
    out.flush();
    
    auto t_main_end = clock_type::now();
    
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    
    std::cerr << "A04 PROFILE | Workers: " << num_threads << "\n"
              << "Init MS: " << std::chrono::duration<double, std::milli>(t_init_end - t_init_start).count() << "\n"
              << "Merge MS: " << std::chrono::duration<double, std::milli>(t_merge_end - t_merge_start).count() << "\n"
              << "Total End-to-End MS: " << std::chrono::duration<double, std::milli>(t_main_end - t_main_start).count() << "\n"
              << "Peak RSS (KB): " << usage.ru_maxrss << "\n";
    for (size_t i = 0; i < threads.size(); i++) {
        std::cerr << "Worker " << i << " Process MS: " << thread_data[i].processing_ms << "\n";
    }

    return 0;
}
