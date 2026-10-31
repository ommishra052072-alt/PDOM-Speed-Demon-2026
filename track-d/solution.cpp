#include <iostream>
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
    uint64_t weight;
};

struct DistNode {
    uint64_t dist;
    uint32_t u;
};

// 4-ary Min-Heap
struct QuadHeap {
    std::vector<DistNode> data;

    QuadHeap() {
        data.reserve(16384);
    }

    bool empty() const {
        return data.empty();
    }

    void clear() {
        data.clear();
    }

    void push(uint64_t dist, uint32_t u) {
        size_t idx = data.size();
        data.push_back({dist, u});
        while (idx > 0) {
            size_t parent = (idx - 1) / 4;
            if (data[idx].dist < data[parent].dist) {
                std::swap(data[idx], data[parent]);
                idx = parent;
            } else {
                break;
            }
        }
    }

    DistNode pop() {
        DistNode min_item = data[0];
        DistNode last = data.back();
        data.pop_back();
        if (!data.empty()) {
            data[0] = last;
            size_t idx = 0;
            size_t sz = data.size();
            while (true) {
                size_t first_child = 4 * idx + 1;
                if (first_child >= sz) break;

                size_t best_child = first_child;
                uint64_t best_dist = data[first_child].dist;

                size_t c2 = first_child + 1;
                if (c2 < sz && data[c2].dist < best_dist) {
                    best_child = c2;
                    best_dist = data[c2].dist;
                }
                size_t c3 = first_child + 2;
                if (c3 < sz && data[c3].dist < best_dist) {
                    best_child = c3;
                    best_dist = data[c3].dist;
                }
                size_t c4 = first_child + 3;
                if (c4 < sz && data[c4].dist < best_dist) {
                    best_child = c4;
                    best_dist = data[c4].dist;
                }

                if (best_dist < data[idx].dist) {
                    std::swap(data[idx], data[best_child]);
                    idx = best_child;
                } else {
                    break;
                }
            }
        }
        return min_item;
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

    std::vector<uint32_t> head(n + 1, 0);

    struct RawEdge {
        uint32_t u;
        uint32_t v;
        uint64_t w;
    };
    std::vector<RawEdge> raw_edges;
    raw_edges.resize(m);

    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = static_cast<uint32_t>(parse_uint64(p_g, end_g));
        uint32_t v = static_cast<uint32_t>(parse_uint64(p_g, end_g));
        uint64_t w = parse_uint64(p_g, end_g);
        raw_edges[i] = {u, v, w};
        head[u]++;
    }

    munmap(const_cast<char*>(g_data), g_size);
    close(fd_g);

    std::vector<uint32_t> offsets(n + 1, 0);
    uint32_t cur = 0;
    for (uint32_t i = 0; i < n; ++i) {
        offsets[i] = cur;
        cur += head[i];
    }
    offsets[n] = cur;

    std::vector<Edge> edges(m);
    std::vector<uint32_t> cur_pos = offsets;
    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = raw_edges[i].u;
        uint32_t pos = cur_pos[u]++;
        edges[pos] = {raw_edges[i].v, raw_edges[i].w};
    }
    std::vector<RawEdge>().swap(raw_edges);
    std::vector<uint32_t>().swap(cur_pos);
    std::vector<uint32_t>().swap(head);

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
    // Use bounded number of threads (e.g. 6 P-cores or hardware concurrency capped at 8)
    uint32_t num_threads = std::min<uint32_t>(8, std::thread::hardware_concurrency());
    if (num_threads == 0) num_threads = 4;

    std::vector<int64_t> answers(queries.size(), -1);
    std::atomic<size_t> query_idx{0};

    auto worker = [&]() {
        // Per-thread distance and epoch state
        std::vector<uint64_t> dist(n, UINT64_MAX);
        std::vector<uint32_t> visited_epoch(n, 0);
        uint32_t current_epoch = 0;
        QuadHeap qh;

        while (true) {
            size_t idx = query_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= queries.size()) break;

            uint32_t s = queries[idx].s;
            uint32_t t = queries[idx].t;

            if (s == t) {
                answers[idx] = 0;
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
