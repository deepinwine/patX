// Document text extraction with a priority chain (task: never OCR first):
//   1. official XML/JSON text (USPTO structured payloads)
//   2. document OCR/text fields returned by the API
//   3. DOCX raw text
//   4. PDF text layer / pdftotext external tool
//   5. OCR fallback (not implemented in this iteration - reported as such)
#pragma once

#include <string>

namespace patx {

struct TextExtractionResult {
    bool success = false;
    std::string text;
    std::string source;      // official_xml / official_ocr / docx / pdf_text / pdftotext / none
    double confidence = 0.0;
    std::string error;
};

class DocumentTextExtractor {
public:
    // Extracts text from a local file according to the priority chain.
    static TextExtractionResult Extract(const std::string& file_path,
                                        const std::string& mime_type = "");

    // True when a usable pdftotext binary is available (PATH or common
    // install locations).
    static bool PdftotextAvailable();
};

} // namespace patx
