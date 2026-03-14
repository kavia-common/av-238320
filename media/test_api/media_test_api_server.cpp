#define LOG_TAG "media_test_api_server"
//#define LOG_NDEBUG 0

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

/**
 * Escape a string for safe inclusion in JSON string values.
 * This is intentionally minimal (sufficient for file names) and avoids pulling JSON deps.
 */
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Control chars
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static bool IsDotOrDotDot(const char* name) {
    return (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0);
}

static std::string JoinPath(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    if (a.back() == '/') return std::string(a) + std::string(b);
    return std::string(a) + "/" + std::string(b);
}

/**
 * Resolve and validate that `candidate` stays under `base`.
 * We use realpath for both to prevent traversal via symlinks/../.
 */
static std::optional<std::string> CanonicalizeUnderBase(const std::string& base,
                                                        const std::string& candidate) {
    char base_real[PATH_MAX];
    char cand_real[PATH_MAX];

    if (realpath(base.c_str(), base_real) == nullptr) {
        PLOG(ERROR) << "realpath(base) failed: " << base;
        return std::nullopt;
    }
    if (realpath(candidate.c_str(), cand_real) == nullptr) {
        PLOG(ERROR) << "realpath(candidate) failed: " << candidate;
        return std::nullopt;
    }

    std::string base_s(base_real);
    std::string cand_s(cand_real);

    // Ensure base ends with '/' for prefix check.
    if (!base_s.empty() && base_s.back() != '/') base_s.push_back('/');

    if (cand_s == base_real) {
        // candidate exactly equals base directory path without trailing slash
        return std::string(cand_real);
    }

    if (android::base::StartsWith(cand_s, base_s)) return cand_s;

    LOG(WARNING) << "Rejected path outside base. base=" << base_s << " cand=" << cand_s;
    return std::nullopt;
}

struct DirEntry {
    std::string name;
    bool is_dir = false;
    int64_t size_bytes = -1;
};

static std::optional<std::vector<DirEntry>> ListDir(const std::string& dir_path) {
    DIR* d = opendir(dir_path.c_str());
    if (!d) {
        PLOG(ERROR) << "opendir failed: " << dir_path;
        return std::nullopt;
    }

    std::vector<DirEntry> out;
    for (;;) {
        errno = 0;
        dirent* ent = readdir(d);
        if (!ent) break;

        if (IsDotOrDotDot(ent->d_name)) continue;

        DirEntry e;
        e.name = ent->d_name;

        std::string full = JoinPath(dir_path, e.name);
        struct stat st {};
        if (lstat(full.c_str(), &st) == 0) {
            e.is_dir = S_ISDIR(st.st_mode);
            if (S_ISREG(st.st_mode)) e.size_bytes = static_cast<int64_t>(st.st_size);
        }

        out.push_back(std::move(e));
    }

    int saved_errno = errno;
    closedir(d);
    if (saved_errno != 0) {
        errno = saved_errno;
        PLOG(ERROR) << "readdir failed: " << dir_path;
        return std::nullopt;
    }

    // Sort by name for stable output (useful in tests/debugging).
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.name < b.name;
    });
    return out;
}

static std::string BuildListingJson(const std::string& base, const std::string& requested_rel,
                                    const std::string& canonical_dir,
                                    const std::vector<DirEntry>& entries) {
    std::string json;
    json += "{";
    json += "\"base\":\"" + JsonEscape(base) + "\",";
    json += "\"path\":\"" + JsonEscape(requested_rel) + "\",";
    json += "\"canonical\":\"" + JsonEscape(canonical_dir) + "\",";
    json += "\"entries\":[";
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        json += "{";
        json += "\"name\":\"" + JsonEscape(e.name) + "\",";
        json += "\"type\":\"";
        json += (e.is_dir ? "dir" : "file");
        json += "\"";
        if (!e.is_dir && e.size_bytes >= 0) {
            json += ",\"sizeBytes\":" + std::to_string(e.size_bytes);
        }
        json += "}";
        if (i + 1 < entries.size()) json += ",";
    }
    json += "]";
    json += "}";
    return json;
}

static std::string HttpResponse(int code, std::string_view content_type, std::string_view body) {
    std::string status = "500 Internal Server Error";
    if (code == 200) status = "200 OK";
    else if (code == 400) status = "400 Bad Request";
    else if (code == 404) status = "404 Not Found";

    std::string resp;
    resp += "HTTP/1.1 " + status + "\r\n";
    resp += "Content-Type: " + std::string(content_type) + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += std::string(body);
    return resp;
}

static std::string ParseRequestLineTarget(const std::string& request) {
    // Expect: METHOD SP TARGET SP HTTP/...
    // Keep it simple: split first line.
    auto pos = request.find("\r\n");
    std::string first = (pos == std::string::npos) ? request : request.substr(0, pos);
    std::vector<std::string> parts = android::base::Split(first, " ");
    if (parts.size() < 2) return "";
    return parts[1];
}

static std::string UrlDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            char hex[3] = {static_cast<char>(in[i + 1]), static_cast<char>(in[i + 2]), 0};
            char* end = nullptr;
            long v = strtol(hex, &end, 16);
            if (end && *end == 0) {
                out.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

static std::optional<std::string> GetQueryParam(std::string_view target, std::string_view key) {
    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) return std::nullopt;
    std::string_view query = target.substr(qpos + 1);
    for (const auto& kv : android::base::Split(std::string(query), "&")) {
        auto eq = kv.find('=');
        std::string k = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        std::string v = (eq == std::string::npos) ? "" : kv.substr(eq + 1);
        if (k == key) return UrlDecode(v);
    }
    return std::nullopt;
}

static std::string StripQuery(std::string_view target) {
    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) return std::string(target);
    return std::string(target.substr(0, qpos));
}

static bool ContainsPathTraversal(const std::string& s) {
    // Reject absolute paths and traversal sequences. We'll also canonicalize later.
    if (!s.empty() && s[0] == '/') return true;
    if (s.find("..") != std::string::npos) return true;
    if (s.find('\\') != std::string::npos) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    android::base::SetMinimumLogSeverity(android::base::INFO);

    int port = 7999;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            LOG(ERROR) << "Invalid port: " << argv[1];
            return 2;
        }
    }

    // In-tree default: listing for frameworks/av/media folder; in this repo snapshot it is "av/media".
    // Keep the base path configurable by env var for test/debug usage.
    // NOTE: ask the user/orchestrator to set MEDIA_TEST_API_BASE_DIR if needed.
    const char* env_base = getenv("MEDIA_TEST_API_BASE_DIR");
    std::string base_dir = env_base ? env_base : "av/media";

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        PLOG(ERROR) << "socket() failed";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        PLOG(ERROR) << "bind() failed (port " << port << ")";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 8) != 0) {
        PLOG(ERROR) << "listen() failed";
        close(server_fd);
        return 1;
    }

    LOG(INFO) << "media_test_api_server listening on 127.0.0.1:" << port;
    LOG(INFO) << "Endpoint: GET /av/media/test/list-media?path=<relative_subdir>";
    LOG(INFO) << "Base dir: " << base_dir << " (override with MEDIA_TEST_API_BASE_DIR)";

    for (;;) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            PLOG(ERROR) << "accept() failed";
            continue;
        }

        std::string request;
        request.resize(8192);
        ssize_t n = read(client_fd, request.data(), request.size());
        if (n <= 0) {
            close(client_fd);
            continue;
        }
        request.resize(static_cast<size_t>(n));

        std::string target = ParseRequestLineTarget(request);
        std::string path_only = StripQuery(target);

        if (path_only != "/av/media/test/list-media") {
            std::string resp = HttpResponse(404, "text/plain", "not found\n");
            write(client_fd, resp.data(), resp.size());
            close(client_fd);
            continue;
        }

        std::string rel = GetQueryParam(target, "path").value_or("");
        if (rel.empty()) rel = ".";

        if (ContainsPathTraversal(rel)) {
            std::string resp = HttpResponse(400, "application/json",
                                            "{\"error\":\"invalid path\"}\n");
            write(client_fd, resp.data(), resp.size());
            close(client_fd);
            continue;
        }

        std::string requested_path = JoinPath(base_dir, rel);
        auto canonical = CanonicalizeUnderBase(base_dir, requested_path);
        if (!canonical.has_value()) {
            std::string resp = HttpResponse(400, "application/json",
                                            "{\"error\":\"path outside base\"}\n");
            write(client_fd, resp.data(), resp.size());
            close(client_fd);
            continue;
        }

        auto entries = ListDir(*canonical);
        if (!entries.has_value()) {
            std::string resp = HttpResponse(404, "application/json",
                                            "{\"error\":\"cannot list directory\"}\n");
            write(client_fd, resp.data(), resp.size());
            close(client_fd);
            continue;
        }

        std::string body = BuildListingJson(base_dir, rel, *canonical, *entries);
        body.push_back('\n');
        std::string resp = HttpResponse(200, "application/json", body);
        write(client_fd, resp.data(), resp.size());
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
