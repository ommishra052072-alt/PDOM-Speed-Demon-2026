#include <iostream>
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <vector>
#include <queue>
#include <string>
#include <filesystem>
#include <cstdint>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

inline uint64_t parse_uint64(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    uint64_t val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

struct Edge {
    uint32_t to;
    uint32_t weight;
};

struct DistNode {
    uint64_t dist;
    uint32_t u;
};

// Monotone 64-bit radix heap for Dijkstra's nondecreasing distance keys.
struct RadixHeap {
    std::array<std::vector<DistNode>, 65> buckets;
    uint64_t last = 0;
    size_t count = 0;

    static unsigned bucket_index(uint64_t key, uint64_t base) {
        return static_cast<unsigned>(std::bit_width(key ^ base));
    }

    bool empty() const {
        return count == 0;
    }

    void clear() {
        for (auto& bucket : buckets) bucket.clear();
        last = 0;
        count = 0;
    }

    void push(uint64_t dist, uint32_t u) {
        buckets[bucket_index(dist, last)].push_back({dist, u});
        ++count;
    }

    DistNode pop() {
        if (buckets[0].empty()) {
            unsigned i = 1;
            while (buckets[i].empty()) ++i;

            uint64_t new_last = UINT64_MAX;
            for (const DistNode& item : buckets[i]) {
                new_last = std::min(new_last, item.dist);
            }
            last = new_last;

            std::vector<DistNode> old_bucket;
            old_bucket.swap(buckets[i]);
            for (const DistNode& item : old_bucket) {
                buckets[bucket_index(item.dist, last)].push_back(item);
            }
        }

        DistNode result = buckets[0].back();
        buckets[0].pop_back();
        --count;
        return result;
    }
};

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    auto t_start = clock_type::now();

    std::string data_dir = "/data";
    std::string out_dir = "/out";

    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) out_dir = argv[2];

    fs::path graph_path = fs::path(data_dir) / "graph.txt";
    fs::path queries_path = fs::path(data_dir) / "queries.txt";
    fs::path output_path = fs::path(out_dir) / "distances.txt";

    // 1. Read graph.txt using mmap
    int fd_g = open(graph_path.c_str(), O_RDONLY);
    if (fd_g < 0) return 1;

    struct stat sb_g;
    if (fstat(fd_g, &sb_g) < 0) { close(fd_g); return 1; }
    size_t g_size = sb_g.st_size;

    const char* g_data = static_cast<const char*>(mmap(nullptr, g_size, PROT_READ, MAP_PRIVATE, fd_g, 0));
    madvise(const_cast<char*>(g_data), g_size, MADV_SEQUENTIAL);

    const char* p_g = g_data;
    const char* end_g = g_data + g_size;

    uint32_t n = static_cast<uint32_t>(parse_uint64(p_g, end_g));
    uint32_t m = static_cast<uint32_t>(parse_uint64(p_g, end_g));

    // Parallel two-pass CSR builder. Each worker owns one contiguous input chunk and
    // one private degree row; its second-pass cursor row reserves disjoint CSR slots.
    constexpr uint32_t build_workers = 8;
    const size_t n_nodes = static_cast<size_t>(n);
    const size_t n_words = (n_nodes + 63) / 64;
    const char* graph_begin = p_g;

    std::vector<const char*> chunk_starts(build_workers + 1);
    chunk_starts[0] = graph_begin;
    chunk_starts[build_workers] = end_g;
    for (uint32_t worker = 1; worker < build_workers; ++worker) {
        const char* boundary = graph_begin +
            (static_cast<size_t>(end_g - graph_begin) * worker / build_workers);
        if (boundary > graph_begin && boundary[-1] != '\n') {
            while (boundary < end_g && *boundary != '\n') ++boundary;
            if (boundary < end_g) ++boundary;
        }
        chunk_starts[worker] = boundary;
    }

    std::vector<uint32_t> local_degrees(static_cast<size_t>(build_workers) * n_nodes, 0);
    std::vector<uint64_t> local_incoming(static_cast<size_t>(build_workers) * n_words, 0);
    std::vector<std::thread> build_threads;
    build_threads.reserve(build_workers);

    for (uint32_t worker = 0; worker < build_workers; ++worker) {
        build_threads.emplace_back([&, worker]() {
            const char* p = chunk_starts[worker];
            const char* chunk_end = chunk_starts[worker + 1];
            uint32_t* degree_row = local_degrees.data() + static_cast<size_t>(worker) * n_nodes;
            uint64_t* incoming_row = local_incoming.data() + static_cast<size_t>(worker) * n_words;
            while (p < chunk_end) {
                while (p < chunk_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
                if (p >= chunk_end) break;
                uint32_t u = static_cast<uint32_t>(parse_uint64(p, chunk_end));
                uint32_t v = static_cast<uint32_t>(parse_uint64(p, chunk_end));
                parse_uint64(p, chunk_end); // w
                ++degree_row[u];
                incoming_row[v >> 6] |= (uint64_t(1) << (v & 63));
            }
        });
    }
    for (auto& thread : build_threads) thread.join();
    build_threads.clear();

    std::vector<uint64_t> has_in(n_words, 0);
    for (uint32_t worker = 0; worker < build_workers; ++worker) {
        const uint64_t* incoming_row =
            local_incoming.data() + static_cast<size_t>(worker) * n_words;
        for (size_t word = 0; word < n_words; ++word) has_in[word] |= incoming_row[word];
    }

    // Prefix each worker's local degree into its disjoint per-vertex CSR cursor.
    // Reusing local_degrees avoids a second worker-by-vertex scratch matrix.
    std::vector<uint32_t> offsets(n_nodes + 1, 0);
    uint32_t edge_prefix = 0;
    for (uint32_t u = 0; u < n; ++u) {
        offsets[u] = edge_prefix;
        for (uint32_t worker = 0; worker < build_workers; ++worker) {
            uint32_t& count_or_cursor =
                local_degrees[static_cast<size_t>(worker) * n_nodes + u];
            uint32_t degree = count_or_cursor;
            count_or_cursor = edge_prefix;
            edge_prefix += degree;
        }
    }
    offsets[n] = edge_prefix;
    if (edge_prefix != m) return 1;

    std::vector<Edge> edges(m);
    for (uint32_t worker = 0; worker < build_workers; ++worker) {
        build_threads.emplace_back([&, worker]() {
            const char* p = chunk_starts[worker];
            const char* chunk_end = chunk_starts[worker + 1];
            uint32_t* cursor_row = local_degrees.data() + static_cast<size_t>(worker) * n_nodes;
            while (p < chunk_end) {
                while (p < chunk_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
                if (p >= chunk_end) break;
                uint32_t u = static_cast<uint32_t>(parse_uint64(p, chunk_end));
                uint32_t v = static_cast<uint32_t>(parse_uint64(p, chunk_end));
                uint32_t w = static_cast<uint32_t>(parse_uint64(p, chunk_end));
                uint32_t pos = cursor_row[u]++;
                edges[pos] = {v, w};
            }
        });
    }
    for (auto& thread : build_threads) thread.join();

    munmap(const_cast<char*>(g_data), g_size);
    close(fd_g);

    std::vector<uint32_t>().swap(local_degrees);
    std::vector<uint64_t>().swap(local_incoming);

    // 2. Read queries.txt
    int fd_q = open(queries_path.c_str(), O_RDONLY);
    if (fd_q < 0) return 1;

    struct stat sb_q;
    if (fstat(fd_q, &sb_q) < 0) { close(fd_q); return 1; }
    size_t q_size = sb_q.st_size;

    const char* q_data = static_cast<const char*>(mmap(nullptr, q_size, PROT_READ, MAP_PRIVATE, fd_q, 0));
    const char* p_q = q_data;
    const char* end_q = q_data + q_size;

    struct Query {
        uint32_t s;
        uint32_t t;
    };
    std::vector<Query> queries;
    queries.reserve(4096);

    while (p_q < end_q) {
        while (p_q < end_q && (*p_q == ' ' || *p_q == '\t' || *p_q == '\r' || *p_q == '\n')) {
            p_q++;
        }
        if (p_q >= end_q) break;
        uint32_t s = static_cast<uint32_t>(parse_uint64(p_q, end_q));
        uint32_t t = static_cast<uint32_t>(parse_uint64(p_q, end_q));
        queries.push_back({s, t});
    }

    munmap(const_cast<char*>(q_data), q_size);
    close(fd_q);

    // 3. Parallel Dijkstra queries
    // Eight workers gave the best measured time/RSS trade-off.
    uint32_t num_threads = 8;

    std::vector<int64_t> answers(queries.size(), -1);
    std::atomic<size_t> query_idx{0};

    auto worker = [&]() {
        // Per-thread distance and epoch state (compact 16-bit epoch saves 3.5 MB per thread)
        std::vector<uint64_t> dist(n, UINT64_MAX);
        std::vector<uint16_t> visited_epoch(n, 0);
        uint16_t current_epoch = 0;
        RadixHeap qh;

        while (true) {
            size_t idx = query_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= queries.size()) break;

            uint32_t s = queries[idx].s;
            uint32_t t = queries[idx].t;

            if (s == t) {
                answers[idx] = 0;
                continue;
            }

            // No incoming edge at t and s != t: t is unreachable from s.
            if (((has_in[t >> 6] >> (t & 63)) & 1) == 0) {
                answers[idx] = -1;
                continue;
            }

            // No outgoing edge from s and s != t: cannot reach anything from s.
            if (offsets[s] == offsets[s + 1]) {
                answers[idx] = -1;
                continue;
            }

            current_epoch++;
            if (current_epoch == 0) {
                std::fill(visited_epoch.begin(), visited_epoch.end(), 0);
                current_epoch = 1;
            }

            qh.clear();
            visited_epoch[s] = current_epoch;
            dist[s] = 0;
            qh.push(0, s);

            int64_t ans = -1;

            while (!qh.empty()) {
                DistNode top = qh.pop();
                uint64_t d = top.dist;
                uint32_t u = top.u;

                if (d > dist[u]) continue;

                if (u == t) {
                    ans = static_cast<int64_t>(d);
                    break;
                }

                uint32_t start_edge = offsets[u];
                uint32_t end_edge = offsets[u + 1];

                for (uint32_t e = start_edge; e < end_edge; ++e) {
                    uint32_t v = edges[e].to;
                    uint64_t nd = d + edges[e].weight;

                    if (visited_epoch[v] != current_epoch) {
                        visited_epoch[v] = current_epoch;
                        dist[v] = nd;
                        qh.push(nd, v);
                    } else if (nd < dist[v]) {
                        dist[v] = nd;
                        qh.push(nd, v);
                    }
                }
            }

            answers[idx] = ans;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& th : threads) {
        th.join();
    }

    // 4. Output results
    fs::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) return 1;

    std::string out_buf;
    out_buf.reserve(64 * 1024);

    for (int64_t d : answers) {
        out_buf += std::to_string(d);
        out_buf += '\n';
        if (out_buf.size() >= 32 * 1024) {
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

    std::cerr << "TRACK_D_PARALLEL_RESULT: exec_ms=" << exec_ms
              << " peak_rss_kb=" << usage.ru_maxrss
              << " queries_count=" << queries.size()
              << " threads=" << num_threads
              << "\n";

    return 0;
}
