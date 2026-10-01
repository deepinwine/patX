#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--exit-immediately") return 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("\"op\":\"shutdown\"") != std::string::npos) return 0;
        if (line.find("\"op\":\"ping\"") != std::string::npos) {
            std::cout << "{\"op\":\"pong\",\"python_ok\":true}" << std::endl;
        } else {
            std::cout << "{\"ok\":true}" << std::endl;
        }
    }
    return 0;
}
