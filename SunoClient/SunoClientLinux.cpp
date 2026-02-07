/**
 * SunoClient implementation for Linux using libcurl (synchronous).
 * Auth: Bearer token. API base: https://api.sunoapi.org
 */
#if defined(__linux__) || defined(__unix__)

#include "SunoClient.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <sstream>
#include <cstring>

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

// Callback for curl to write response data
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// Callback for curl to write binary data
static size_t writeBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::vector<uint8_t>* response = static_cast<std::vector<uint8_t>*>(userp);
    uint8_t* data = static_cast<uint8_t*>(contents);
    response->insert(response->end(), data, data + totalSize);
    return totalSize;
}

SunoClient::SunoClient(std::string apiKey) : apiKey_(std::move(apiKey)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

SunoClient::~SunoClient() {
    curl_global_cleanup();
}

std::string SunoClient::get(const std::string& path) {
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return {}; }
    
    std::string url = std::string(kBaseUrl) + (path.empty() || path[0] != '/' ? "/" : "") + path;
    std::string response;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "Failed to initialize curl";
        return {};
    }
    
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return {};
    }
    
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (httpCode >= 400) {
        lastError_ = "HTTP " + std::to_string(httpCode) + (response.empty() ? "" : " " + response.substr(0, 200));
        return {};
    }
    
    return response;
}

std::string SunoClient::post(const std::string& path, const std::string& jsonBody) {
    lastError_.clear();
    if (apiKey_.empty()) { lastError_ = "No API key"; return {}; }
    
    std::string url = std::string(kBaseUrl) + (path.empty() || path[0] != '/' ? "/" : "") + path;
    std::string response;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "Failed to initialize curl";
        return {};
    }
    
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, jsonBody.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return {};
    }
    
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (httpCode >= 400) {
        lastError_ = "HTTP " + std::to_string(httpCode) + (response.empty() ? "" : " " + response.substr(0, 200));
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
    std::string url = std::string(kUploadBaseUrl) + "/api/file-stream-upload";
    std::string response;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "Failed to initialize curl";
        return {};
    }
    
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());
    
    // Create multipart form
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, name.c_str());
    curl_mime_data(part, reinterpret_cast<const char*>(audioWavOrMp3.data()), audioWavOrMp3.size());
    curl_mime_type(part, "application/octet-stream");
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return {};
    }
    
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (httpCode >= 400) {
        lastError_ = "Upload HTTP " + std::to_string(httpCode);
        return {};
    }
    
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
    std::vector<uint8_t> audioData;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "Failed to initialize curl";
        return {};
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBinaryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &audioData);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        lastError_ = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return {};
    }
    
    curl_easy_cleanup(curl);
    return audioData;
}

std::string SunoClient::postMultipart(const std::string& path, const std::string& filePartName,
                                     const void* fileData, size_t fileSize, const std::string& fileName) {
    // This is used by uploadAudio, implemented there directly
    return {};
}

} // namespace suno

#endif
