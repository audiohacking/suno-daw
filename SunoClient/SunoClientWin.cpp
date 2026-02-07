/**
 * SunoClient implementation for Windows using WinHTTP (synchronous).
 * Auth: Bearer token. API base: https://api.sunoapi.org
 */
#ifdef _WIN32

#include "SunoClient.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <sstream>

namespace suno {

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if ((unsigned char)c >= 32) out += c;
    }
    return out;
}

static std::wstring stringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

static std::string wstringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;
}

static std::string performRequest(const std::wstring& host, int port, const std::wstring& path,
                                  const std::string& method, const std::string& body,
                                  const std::string& bearerToken) {
    HINTERNET hSession = WinHttpOpen(L"SunoClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }
    
    DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring wmethod = stringToWString(method);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(), path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }
    
    // Add Authorization header
    if (!bearerToken.empty()) {
        std::wstring authHeader = L"Authorization: Bearer " + stringToWString(bearerToken) + L"\r\n";
        WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), (DWORD)authHeader.length(),
                                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    
    // Add Content-Type for POST
    if (method == "POST") {
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json\r\n", -1,
                                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    
    BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)(body.empty() ? nullptr : body.data()),
                                     (DWORD)body.size(), (DWORD)body.size(), 0);
    
    std::string response;
    if (result && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                response.append(buffer.data(), bytesRead);
            }
        }
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

SunoClient::SunoClient(std::string apiKey) : apiKey_(std::move(apiKey)) {}

SunoClient::~SunoClient() = default;

std::string SunoClient::get(const std::string& path) {
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return {}; }
    
    std::string fullPath = (path.empty() || path[0] != '/') ? "/" + path : path;
    std::wstring wpath = stringToWString(fullPath);
    
    std::string response = performRequest(L"api.sunoapi.org", 443, wpath, "GET", "", apiKey_);
    if (response.empty()) {
        lastError_ = "Request failed";
        return {};
    }
    
    // Check for error status in response
    if (response.find("\"code\":") != std::string::npos) {
        size_t pos = response.find("\"code\":");
        if (pos != std::string::npos) {
            pos += 7; // skip "code":
            while (pos < response.size() && std::isspace(response[pos])) pos++;
            if (pos < response.size() && response[pos] != '2') {
                // Not a 2xx code
                lastError_ = "HTTP error in response";
            }
        }
    }
    
    return response;
}

std::string SunoClient::post(const std::string& path, const std::string& jsonBody) {
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return {}; }
    
    std::string fullPath = (path.empty() || path[0] != '/') ? "/" + path : path;
    std::wstring wpath = stringToWString(fullPath);
    
    std::string response = performRequest(L"api.sunoapi.org", 443, wpath, "POST", jsonBody, apiKey_);
    if (response.empty()) {
        lastError_ = "Request failed";
        return {};
    }
    
    return response;
}

bool SunoClient::checkCredits() {
    std::string body = get("/api/v1/generate/credit");
    if (body.empty()) return false;
    return body.find("\"code\":200") != std::string::npos || body.find("\"data\":") != std::string::npos;
}

// Build JSON for generate (callBackUrl required by API; we poll so use placeholder)
static std::string buildGenerateJson(const GenerateParams& p, const std::string* uploadUrl = nullptr) {
    std::ostringstream o;
    o << "{\"customMode\":" << (p.customMode ? "true" : "false")
      << ",\"instrumental\":" << (p.instrumental ? "true" : "false")
      << ",\"model\":\"" << modelToString(p.model) << "\""
      << ",\"callBackUrl\":\"https://example.com/callback\"";
    if (uploadUrl && !uploadUrl->empty())
        o << ",\"uploadUrl\":\"" << escapeJsonString(*uploadUrl) << "\"";
    if (!p.prompt.empty()) o << ",\"prompt\":\"" << escapeJsonString(p.prompt) << "\"";
    if (!p.style.empty()) o << ",\"style\":\"" << escapeJsonString(p.style) << "\"";
    if (!p.title.empty()) o << ",\"title\":\"" << escapeJsonString(p.title) << "\"";
    if (!p.personaId.empty()) o << ",\"personaId\":\"" << escapeJsonString(p.personaId) << "\"";
    if (!p.negativeTags.empty()) o << ",\"negativeTags\":\"" << escapeJsonString(p.negativeTags) << "\"";
    if (!p.vocalGender.empty()) o << ",\"vocalGender\":\"" << escapeJsonString(p.vocalGender) << "\"";
    o << ",\"styleWeight\":" << p.styleWeight;
    o << ",\"weirdnessConstraint\":" << p.weirdnessConstraint;
    o << ",\"audioWeight\":" << p.audioWeight << "}";
    return o.str();
}

std::string SunoClient::startGenerate(const GenerateParams& params) {
    std::string body = post("/api/v1/generate", buildGenerateJson(params));
    if (body.empty()) return {};
    // Parse data.taskId
    size_t i = body.find("\"taskId\"");
    if (i == std::string::npos) { lastError_ = "No taskId in response"; return {}; }
    i = body.find(':', i);
    if (i == std::string::npos) return {};
    i = body.find('"', i);
    if (i == std::string::npos) return {};
    size_t j = body.find('"', i + 1);
    if (j == std::string::npos) return {};
    return body.substr(i + 1, j - (i + 1));
}

std::string SunoClient::startUploadCover(const std::string& uploadUrl, const GenerateParams& params) {
    std::string json = buildGenerateJson(params, &uploadUrl);
    std::string body = post("/api/v1/generate/upload-cover", json);
    if (body.empty()) return {};
    size_t i = body.find("\"taskId\"");
    if (i == std::string::npos) { lastError_ = "No taskId in response"; return {}; }
    i = body.find(':', i); if (i == std::string::npos) return {};
    i = body.find('"', i); if (i == std::string::npos) return {};
    size_t j = body.find('"', i + 1); if (j == std::string::npos) return {};
    return body.substr(i + 1, j - (i + 1));
}

std::string SunoClient::startAddVocals(const AddVocalsParams& params) {
    std::ostringstream o;
    o << "{\"uploadUrl\":\"" << escapeJsonString(params.uploadUrl) << "\""
      << ",\"prompt\":\"" << escapeJsonString(params.prompt) << "\""
      << ",\"title\":\"" << escapeJsonString(params.title) << "\""
      << ",\"negativeTags\":\"" << escapeJsonString(params.negativeTags) << "\""
      << ",\"style\":\"" << escapeJsonString(params.style) << "\""
      << ",\"callBackUrl\":\"https://example.com/callback\"";
    if (!params.vocalGender.empty()) o << ",\"vocalGender\":\"" << escapeJsonString(params.vocalGender) << "\"";
    o << ",\"styleWeight\":" << params.styleWeight
      << ",\"weirdnessConstraint\":" << params.weirdnessConstraint
      << ",\"audioWeight\":" << params.audioWeight
      << ",\"model\":\"" << modelToString(params.model) << "\"}";
    std::string body = post("/api/v1/generate/add-vocals", o.str());
    if (body.empty()) return {};
    size_t i = body.find("\"taskId\"");
    if (i == std::string::npos) { lastError_ = "No taskId in response"; return {}; }
    i = body.find(':', i); if (i == std::string::npos) return {};
    i = body.find('"', i); if (i == std::string::npos) return {};
    size_t j = body.find('"', i + 1); if (j == std::string::npos) return {};
    return body.substr(i + 1, j - (i + 1));
}

// Parse GET record-info response for status and audioUrls
static void parseRecordInfo(const std::string& body, TaskStatus* out) {
    if (!out) return;
    auto pos = body.find("\"status\"");
    if (pos != std::string::npos) {
        pos = body.find('"', body.find(':', pos) + 1);
        size_t end = body.find('"', pos + 1);
        if (pos != std::string::npos && end != std::string::npos)
            out->status = body.substr(pos + 1, end - (pos + 1));
    }
    pos = body.find("\"errorMessage\"");
    if (pos != std::string::npos) {
        pos = body.find('"', body.find(':', pos) + 1);
        size_t end = body.find('"', pos + 1);
        if (pos != std::string::npos && end != std::string::npos)
            out->errorMessage = body.substr(pos + 1, end - (pos + 1));
    }
    pos = body.find("\"taskId\"");
    if (pos != std::string::npos) {
        pos = body.find('"', body.find(':', pos) + 1);
        size_t end = body.find('"', pos + 1);
        if (pos != std::string::npos && end != std::string::npos)
            out->taskId = body.substr(pos + 1, end - (pos + 1));
    }
    // sunoData array: each element has "audioUrl"
    size_t idx = 0;
    while ((idx = body.find("\"audioUrl\"", idx)) != std::string::npos) {
        idx = body.find(':', idx);
        if (idx == std::string::npos) break;
        size_t q = body.find('"', idx);
        if (q == std::string::npos) break;
        size_t r = body.find('"', q + 1);
        if (r != std::string::npos)
            out->audioUrls.push_back(body.substr(q + 1, r - (q + 1)));
        idx = r + 1;
    }
}

TaskStatus SunoClient::getTaskStatus(const std::string& taskId) {
    TaskStatus out;
    out.taskId = taskId;
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return out; }
    std::string path = "/api/v1/generate/record-info?taskId=" + taskId;
    std::string body = get(path);
    if (body.empty()) return out;
    parseRecordInfo(body, &out);
    return out;
}

std::string SunoClient::uploadAudio(const std::vector<uint8_t>& audioWavOrMp3, const std::string& fileName) {
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return {}; }
    
    std::string name = fileName.empty() ? "audio.wav" : fileName;
    std::string boundary = "----SunoUploadBoundary";
    
    // Build multipart body
    std::ostringstream bodyStream;
    bodyStream << "--" << boundary << "\r\n";
    bodyStream << "Content-Disposition: form-data; name=\"file\"; filename=\"" << name << "\"\r\n";
    bodyStream << "Content-Type: application/octet-stream\r\n\r\n";
    bodyStream.write(reinterpret_cast<const char*>(audioWavOrMp3.data()), audioWavOrMp3.size());
    bodyStream << "\r\n--" << boundary << "--\r\n";
    
    std::string body = bodyStream.str();
    std::wstring wpath = L"/api/file-stream-upload";
    
    // Custom request with multipart content-type
    HINTERNET hSession = WinHttpOpen(L"SunoClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { lastError_ = "Failed to open session"; return {}; }
    
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.sunoapi.org", 443, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        lastError_ = "Failed to connect";
        return {};
    }
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(),
                                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        lastError_ = "Failed to open request";
        return {};
    }
    
    // Add headers
    std::wstring authHeader = L"Authorization: Bearer " + stringToWString(apiKey_) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), (DWORD)authHeader.length(),
                            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    
    std::wstring contentType = L"Content-Type: multipart/form-data; boundary=" + stringToWString(boundary) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, contentType.c_str(), (DWORD)contentType.length(),
                            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    
    BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    
    std::string response;
    if (result && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                response.append(buffer.data(), bytesRead);
            }
        }
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    if (response.empty()) { lastError_ = "Upload failed"; return {}; }
    
    // Parse data.fileUrl from response
    size_t i = response.find("\"fileUrl\"");
    if (i == std::string::npos) { i = response.find("\"downloadUrl\""); }
    if (i == std::string::npos) { lastError_ = "No fileUrl in upload response"; return {}; }
    i = response.find('"', response.find(':', i) + 1);
    size_t j = response.find('"', i + 1);
    if (j != std::string::npos)
        return response.substr(i + 1, j - (i + 1));
    return {};
}

std::vector<uint8_t> SunoClient::fetchAudio(const std::string& url) {
    lastError_.clear();
    
    // Parse URL to extract host and path
    std::string host, path;
    bool isHttps = false;
    
    if (url.find("https://") == 0) {
        isHttps = true;
        size_t hostStart = 8; // after "https://"
        size_t pathStart = url.find('/', hostStart);
        if (pathStart != std::string::npos) {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        } else {
            host = url.substr(hostStart);
            path = "/";
        }
    } else if (url.find("http://") == 0) {
        size_t hostStart = 7; // after "http://"
        size_t pathStart = url.find('/', hostStart);
        if (pathStart != std::string::npos) {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        } else {
            host = url.substr(hostStart);
            path = "/";
        }
    } else {
        lastError_ = "Invalid URL";
        return {};
    }
    
    std::wstring whost = stringToWString(host);
    std::wstring wpath = stringToWString(path);
    int port = isHttps ? 443 : 80;
    
    HINTERNET hSession = WinHttpOpen(L"SunoClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }
    
    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
                                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }
    
    BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    
    std::vector<uint8_t> audioData;
    if (result && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            size_t oldSize = audioData.size();
            audioData.resize(oldSize + bytesAvailable);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, audioData.data() + oldSize, bytesAvailable, &bytesRead)) {
                audioData.resize(oldSize + bytesRead);
            }
        }
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return audioData;
}

std::string SunoClient::postMultipart(const std::string& path, const std::string& filePartName,
                                     const void* fileData, size_t fileSize, const std::string& fileName) {
    // This is used by uploadAudio, implemented there directly
    return {};
}

} // namespace suno

#endif
