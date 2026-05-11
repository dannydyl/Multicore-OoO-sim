#pragma once

// Multi-thread program manifest (CasimV2). Plain text, key-value
// per line. Example:
//
//     # SPLASH-2 LU.b on 4 threads
//     program: lu_b
//     threads: 4
//     program_uid: 0xC0FFEE     # optional, defaults to 0
//     t0: t0.casim
//     t1: t1.casim
//     t2: t2.casim
//     t3: t3.casim
//
// Conventions:
//   - Blank lines and '#' comments are skipped.
//   - Relative paths in t<N>: entries resolve against the manifest's
//     parent directory.
//   - All t0 .. t(threads-1) must be present; duplicates or gaps
//     raise ManifestError.
//   - thread_count is the source of truth; full_mode requires
//     thread_count == cores for v1 (no scheduler multiplexing).

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace comparch::trace {

class ManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProgramManifest {
    std::string                              name;
    std::uint32_t                            thread_count = 0;
    std::uint64_t                            program_uid  = 0;
    // paths[i] is the .casim file for thread i. Always resolved
    // to an absolute or manifest-relative path that exists on disk.
    std::vector<std::filesystem::path>       paths;
    // For reporting / log lines.
    std::filesystem::path                    source;
};

// Parse manifest file. Throws ManifestError on any parse/validation
// failure (missing keys, bad thread count, missing t<N>: entries,
// duplicate t<N>:, t<N>: file not found on disk).
ProgramManifest parse_program_manifest(const std::filesystem::path& path);

} // namespace comparch::trace
