#include "patx/text_extractor.hpp"
#include "patx/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace patx {

namespace {

std::string ReadFileToString(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return content;
}

bool RunCommand(const std::string& cmd, const std::string& args, const std::string& out_path) {
    std::string full = "\"" + cmd + "\" " + args;
    int rc = std::system(full.c_str());
    return rc == 0 && std::filesystem::exists(out_path);
}

std::string FindPdftotext() {
    // PATH check without executing anything risky
    const char* path_env = std::getenv("PATH");
    std::string path = path_env ? path_env : "";
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
#ifdef _WIN32
            std::string exe = dir + (dir.back() == '\\' || dir.back() == '/' ? "" : "\\") + "pdftotext.exe";
#else
            std::string exe = dir + (dir.back() == '/' ? "" : "/") + "pdftotext";
#endif
            if (std::filesystem::exists(exe)) return exe;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
#ifdef _WIN32
    for (const char* candidate : {"C:\\Program Files\\poppler\\pdftotext.exe",
                                  "C:\\Program Files (x86)\\poppler\\pdftotext.exe",
                                  "C:\\poppler\\pdftotext.exe"}) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
#endif
    return "";
}

} // namespace

bool DocumentTextExtractor::PdftotextAvailable() {
    return !FindPdftotext().empty();
}

TextExtractionResult DocumentTextExtractor::Extract(const std::string& file_path,
                                                    const std::string& mime_type) {
    TextExtractionResult result;
    if (file_path.empty() || !std::filesystem::exists(file_path)) {
        result.error = "file not found: " + file_path;
        return result;
    }

    std::string ext = std::filesystem::path(file_path).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // ---- Plain text / XML payloads: use directly ----
    if (ext == ".txt" || ext == ".xml" || ext == ".json") {
        std::string text = ReadFileToString(file_path);
        if (!text.empty()) {
            result.success = true;
            result.text = text;
            result.source = ext == ".xml" ? "official_xml" : "pdf_text";
            result.confidence = 0.95;
            return result;
        }
    }

    // ---- DOCX: contains word/document.xml; extract text between tags ----
    // DOCX is a zip archive. Without a zip library we shell out to unzip when
    // present; otherwise report the limitation instead of guessing.
    if (ext == ".docx" || mime_type == "DOCX" || mime_type == "MS_WORD") {
#ifdef _WIN32
        const char* unzip_cmd = "tar -x -f";
#else
        const char* unzip_cmd = "unzip -p";
#endif
        std::string tmp_out = (std::filesystem::temp_directory_path() / "patx_docx_doc.xml").string();
        std::string cmd = std::string(unzip_cmd) + " \"" + file_path + "\" word/document.xml > \"" + tmp_out + "\"";
        if (std::system(cmd.c_str()) == 0) {
            std::string xml = ReadFileToString(tmp_out);
            std::filesystem::remove(tmp_out);
            // Very light tag stripping: <w:p> becomes newline, tags dropped
            std::string text;
            std::string tag;
            bool in_tag = false;
            for (char c : xml) {
                if (c == '<') { in_tag = true; tag.clear(); continue; }
                if (c == '>') {
                    in_tag = false;
                    if (tag == "/w:p" || tag == "w:p") text += '\n';
                    continue;
                }
                if (in_tag) tag += c;
                else text += c;
            }
            if (!text.empty()) {
                result.success = true;
                result.text = text;
                result.source = "docx";
                result.confidence = 0.85;
                return result;
            }
        }
        result.error = "DOCX text extraction requires unzip support";
        return result;
    }

    // ---- PDF: pdftotext (text layer) ----
    if (ext == ".pdf" || mime_type == "PDF") {
        std::string tool = FindPdftotext();
        if (!tool.empty()) {
            std::string tmp_out = (std::filesystem::temp_directory_path() /
                                   ("patx_pdf_" + std::to_string(std::rand()) + ".txt")).string();
            std::string args = "-layout \"" + file_path + "\" \"" + tmp_out + "\"";
            if (RunCommand(tool, args, tmp_out)) {
                std::string text = ReadFileToString(tmp_out);
                std::filesystem::remove(tmp_out);
                if (!text.empty()) {
                    result.success = true;
                    result.text = text;
                    result.source = "pdftotext";
                    result.confidence = 0.8;
                    return result;
                }
            }
            std::filesystem::remove(tmp_out);
            result.error = "pdftotext produced no text (scanned document?)";
        } else {
            result.error = "pdftotext is not installed; install poppler-utils for PDF text extraction";
        }
        // OCR is deliberately not auto-attempted: mark for manual review
        return result;
    }

    result.error = "unsupported file type: " + ext;
    return result;
}

} // namespace patx
