// Quick test for Excel import detection
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <OpenXLSX.hpp>

static std::string CellToString(OpenXLSX::XLCellValue value) {
    if (value.type() == OpenXLSX::XLValueType::Empty) return "";
    if (value.type() == OpenXLSX::XLValueType::String) return value.get<std::string>();
    if (value.type() == OpenXLSX::XLValueType::Integer) return std::to_string(value.get<int64_t>());
    if (value.type() == OpenXLSX::XLValueType::Float) {
        double d = value.get<double>();
        if (d == static_cast<int64_t>(d)) return std::to_string(static_cast<int64_t>(d));
        std::ostringstream oss; oss << d; return oss.str();
    }
    try { return value.get<std::string>(); }
    catch (...) { return ""; }
}

#include <sstream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_import.exe <xlsx_file>" << std::endl;
        return 1;
    }

    try {
        OpenXLSX::XLDocument doc(argv[1]);
        auto wb = doc.workbook();

        for (const auto& sheet_name : wb.worksheetNames()) {
            auto ws = wb.worksheet(sheet_name);
            uint32_t rowCount = ws.rowCount();

            std::cout << "\n=== Sheet: " << sheet_name << " (rows: " << rowCount << ") ===" << std::endl;

            // Read headers
            std::vector<std::string> headers;
            for (uint16_t col = 1; col <= 30; col++) {
                std::string header = CellToString(ws.cell(1, col).value());
                if (header.empty() && col > 5) break;
                headers.push_back(header);
            }

            std::cout << "Headers (" << headers.size() << "):" << std::endl;
            for (size_t i = 0; i < headers.size(); i++) {
                std::cout << "  [" << i << "] " << headers[i] << std::endl;
            }

            // Show first 3 data rows
            for (uint32_t row = 2; row <= std::min(rowCount, (uint32_t)4); row++) {
                std::cout << "Row " << row << ":" << std::endl;
                for (size_t col = 0; col < headers.size() && col < 5; col++) {
                    std::cout << "  [" << headers[col] << "] = " << CellToString(ws.cell(row, col + 1).value()) << std::endl;
                }
            }
        }

        doc.close();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
