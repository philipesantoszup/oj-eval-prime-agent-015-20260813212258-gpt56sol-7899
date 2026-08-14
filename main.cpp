#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {
constexpr uint64_t MAGIC = 0x31564244454c4946ULL; // "FILEDBV1"
constexpr uint32_t VERSION = 1;
constexpr uint32_t BUCKET_COUNT = 1u << 18;
constexpr const char *DATABASE_FILE = "file_storage.db";

struct Header {
    uint64_t magic;
    uint32_t version;
    uint32_t buckets;
    uint32_t node_count;
    uint32_t free_head;
};

struct Node {
    uint32_t next;
    int32_t value;
    char key[65];
};

class Database {
    FILE *file_ = nullptr;
    Header header_{};

    static uint64_t hash_key(const char *s) {
        // FNV-1a followed by an avalanche step.
        uint64_t h = 1469598103934665603ULL;
        while (*s) {
            h ^= static_cast<unsigned char>(*s++);
            h *= 1099511628211ULL;
        }
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    static long bucket_position(uint32_t bucket) {
        return static_cast<long>(sizeof(Header)) +
               static_cast<long>(bucket) * sizeof(uint32_t);
    }

    static long node_position(uint32_t id) {
        return static_cast<long>(sizeof(Header)) +
               static_cast<long>(BUCKET_COUNT) * sizeof(uint32_t) +
               static_cast<long>(id - 1) * sizeof(Node);
    }

    uint32_t read_bucket(uint32_t bucket) {
        uint32_t head = 0;
        std::fseek(file_, bucket_position(bucket), SEEK_SET);
        std::fread(&head, sizeof(head), 1, file_);
        return head;
    }

    void write_bucket(uint32_t bucket, uint32_t head) {
        std::fseek(file_, bucket_position(bucket), SEEK_SET);
        std::fwrite(&head, sizeof(head), 1, file_);
    }

    Node read_node(uint32_t id) {
        Node node{};
        std::fseek(file_, node_position(id), SEEK_SET);
        std::fread(&node, sizeof(node), 1, file_);
        return node;
    }

    void write_node(uint32_t id, const Node &node) {
        std::fseek(file_, node_position(id), SEEK_SET);
        std::fwrite(&node, sizeof(node), 1, file_);
    }

    void initialize() {
        header_ = {MAGIC, VERSION, BUCKET_COUNT, 0, 0};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(&header_, sizeof(header_), 1, file_);
        uint32_t zeros[4096]{};
        for (uint32_t written = 0; written < BUCKET_COUNT; written += 4096)
            std::fwrite(zeros, sizeof(uint32_t), 4096, file_);
        std::fflush(file_);
    }

public:
    Database() {
        file_ = std::fopen(DATABASE_FILE, "r+b");
        if (!file_) {
            file_ = std::fopen(DATABASE_FILE, "w+b");
            initialize();
        } else {
            if (std::fread(&header_, sizeof(header_), 1, file_) != 1 ||
                header_.magic != MAGIC || header_.version != VERSION ||
                header_.buckets != BUCKET_COUNT) {
                std::fclose(file_);
                file_ = std::fopen(DATABASE_FILE, "w+b");
                initialize();
            }
        }
    }

    ~Database() {
        if (file_) {
            std::fseek(file_, 0, SEEK_SET);
            std::fwrite(&header_, sizeof(header_), 1, file_);
            std::fclose(file_);
        }
    }

    void insert(const char *key, int32_t value) {
        uint32_t bucket = static_cast<uint32_t>(hash_key(key)) & (BUCKET_COUNT - 1);
        uint32_t head = read_bucket(bucket);
        uint32_t id;
        if (header_.free_head != 0) {
            id = header_.free_head;
            Node free_node = read_node(id);
            header_.free_head = free_node.next;
        } else {
            id = ++header_.node_count;
        }
        Node node{};
        node.next = head;
        node.value = value;
        std::strncpy(node.key, key, sizeof(node.key) - 1);
        write_node(id, node);
        write_bucket(bucket, id);
    }

    void erase(const char *key, int32_t value) {
        uint32_t bucket = static_cast<uint32_t>(hash_key(key)) & (BUCKET_COUNT - 1);
        uint32_t current = read_bucket(bucket);
        uint32_t previous = 0;
        while (current != 0) {
            Node node = read_node(current);
            if (node.value == value && std::strcmp(node.key, key) == 0) {
                if (previous == 0) {
                    write_bucket(bucket, node.next);
                } else {
                    Node prev_node = read_node(previous);
                    prev_node.next = node.next;
                    write_node(previous, prev_node);
                }
                node.next = header_.free_head;
                header_.free_head = current;
                write_node(current, node);
                return;
            }
            previous = current;
            current = node.next;
        }
    }

    void find(const char *key) {
        uint32_t bucket = static_cast<uint32_t>(hash_key(key)) & (BUCKET_COUNT - 1);
        uint32_t current = read_bucket(bucket);
        std::vector<int32_t> values;
        while (current != 0) {
            Node node = read_node(current);
            if (std::strcmp(node.key, key) == 0)
                values.push_back(node.value);
            current = node.next;
        }
        if (values.empty()) {
            std::puts("null");
            return;
        }
        std::sort(values.begin(), values.end());
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) std::putchar(' ');
            std::printf("%d", values[i]);
        }
        std::putchar('\n');
    }
};
} // namespace

int main() {
    int n;
    if (std::scanf("%d", &n) != 1) return 0;
    Database database;
    char command[8];
    char key[65];
    int value;
    for (int i = 0; i < n; ++i) {
        if (std::scanf("%7s %64s", command, key) != 2) return 0;
        if (command[0] == 'i') {
            std::scanf("%d", &value);
            database.insert(key, value);
        } else if (command[0] == 'd') {
            std::scanf("%d", &value);
            database.erase(key, value);
        } else {
            database.find(key);
        }
    }
    return 0;
}
