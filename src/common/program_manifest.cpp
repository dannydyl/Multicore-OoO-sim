#include "comparch/program_manifest.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

namespace comparch::trace {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

std::uint64_t parse_uint(const std::string& s, const std::string& key) {
    if (s.empty()) {
        throw ManifestError("empty value for key '" + key + "'");
    }
    try {
        size_t pos = 0;
        const auto v = std::stoull(s, &pos, 0);  // base=0 → auto-detect 0x prefix
        // Allow trailing whitespace only.
        for (; pos < s.size(); ++pos) {
            if (!std::isspace(static_cast<unsigned char>(s[pos]))) {
                throw ManifestError("trailing garbage in '" + key + "' value: " + s);
            }
        }
        return v;
    } catch (const std::invalid_argument&) {
        throw ManifestError("bad integer for '" + key + "': " + s);
    } catch (const std::out_of_range&) {
        throw ManifestError("integer out of range for '" + key + "': " + s);
    }
}

// Recognize "t<N>" keys and return N, else nullopt.
std::optional<std::uint32_t> parse_thread_key(const std::string& key) {
    if (key.size() < 2 || key[0] != 't') return std::nullopt;
    for (size_t i = 1; i < key.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) return std::nullopt;
    }
    try {
        const auto v = std::stoul(key.substr(1));
        if (v > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        return static_cast<std::uint32_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

ProgramManifest parse_program_manifest(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw ManifestError("cannot open program manifest: " + path.string());
    }

    ProgramManifest m;
    m.source = path;
    const auto base = path.parent_path();

    std::map<std::uint32_t, fs::path> thread_paths;  // sorted for gap check
    bool saw_threads = false;

    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // Strip inline comment, then trim.
        if (auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        line = trim(line);
        if (line.empty()) continue;

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            throw ManifestError(path.string() + ":" + std::to_string(lineno) +
                                ": expected 'key: value', got: " + line);
        }
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));

        if (key == "program") {
            m.name = val;
        } else if (key == "threads") {
            const auto v = parse_uint(val, "threads");
            if (v == 0 || v > 65536) {
                throw ManifestError("threads must be in [1, 65536], got " + val);
            }
            m.thread_count = static_cast<std::uint32_t>(v);
            saw_threads = true;
        } else if (key == "program_uid") {
            m.program_uid = parse_uint(val, "program_uid");
        } else if (auto tid = parse_thread_key(key)) {
            fs::path p = val;
            if (p.is_relative()) p = base / p;
            if (!fs::exists(p)) {
                throw ManifestError(path.string() + ":" + std::to_string(lineno) +
                                    ": thread file not found: " + p.string());
            }
            auto [_, inserted] = thread_paths.emplace(*tid, std::move(p));
            if (!inserted) {
                throw ManifestError(path.string() + ":" + std::to_string(lineno) +
                                    ": duplicate t" + std::to_string(*tid));
            }
        } else {
            throw ManifestError(path.string() + ":" + std::to_string(lineno) +
                                ": unknown key '" + key + "'");
        }
    }

    if (!saw_threads) {
        throw ManifestError(path.string() + ": missing required key 'threads'");
    }
    if (thread_paths.size() != m.thread_count) {
        throw ManifestError(path.string() + ": threads=" +
                            std::to_string(m.thread_count) + " but found " +
                            std::to_string(thread_paths.size()) + " t<N> entries");
    }
    // Verify dense 0..N-1.
    m.paths.reserve(m.thread_count);
    for (std::uint32_t i = 0; i < m.thread_count; ++i) {
        auto it = thread_paths.find(i);
        if (it == thread_paths.end()) {
            throw ManifestError(path.string() + ": missing t" + std::to_string(i));
        }
        m.paths.push_back(std::move(it->second));
    }

    return m;
}

} // namespace comparch::trace
