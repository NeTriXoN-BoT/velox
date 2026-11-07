// HTTPS downloads with checksums. Used by the first-run setup, root setup and self-update.
#pragma once

#include <atomic>

#include "common.hpp"

namespace phone::net {

// GETs a URL and passes the body to `sink` in pieces (return false to stop). HTTPS only.
// `from` > 0 asks the server to resume from that byte; `total` then reports the full size.
bool get(const std::wstring& url, uint64_t maxBytes, uint64_t from,
         const std::function<bool(const char* data, size_t n, uint64_t total)>& sink, std::wstring& error);

bool getText(const std::wstring& url, uint64_t maxBytes, std::string& out, std::wstring& error);

enum class Hash { Sha1, Sha256 };
std::string hashFile(const std::wstring& path, Hash kind, const std::atomic<bool>* cancel = nullptr);  // lowercase hex, empty on failure/cancel
std::string hashBytes(const void* data, size_t n, Hash kind);

// Downloads to `path`, resuming a `.part` file left by an interrupted try, and checks the size
// and checksum before the file gets its final name.
bool download(const std::wstring& url, const std::wstring& path, uint64_t size, const std::string& hexHash, Hash kind,
              const std::function<void(uint64_t done, uint64_t total)>& progress, const std::atomic<bool>* cancel,
              std::wstring& error);

}  // namespace phone::net
