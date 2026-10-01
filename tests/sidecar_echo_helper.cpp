#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--exit-immediately") return 0;
#ifndef _WIN32
    if (mode == "--ignore-shutdown") std::signal(SIGTERM, SIG_IGN);
    const long pid = getpid();
#else
    const long pid = _getpid();
#endif

    if (mode == "--exit-with-pid-file" && argc == 3) {
        std::ofstream pid_file(argv[2]);
        if (!pid_file) return 2;
        pid_file << pid << std::endl;
        return 0;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("\"op\":\"shutdown\"") != std::string::npos) {
            if (mode == "--ignore-shutdown") continue;
            return 0;
        }
        if (line.find("\"op\":\"ping\"") != std::string::npos) {
            if (mode == "--exit-on-ping") return 0;
            if (mode == "--close-input-on-ping") {
#ifdef _WIN32
                _close(0);
#else
                close(STDIN_FILENO);
#endif
            }
            std::cout << "{\"op\":\"pong\",\"python_ok\":true,\"pid\":" << pid
                      << '}' << std::endl;
            if (mode == "--close-input-on-ping") return 0;
        } else {
            std::cout << "{\"ok\":true,\"pid\":" << pid << '}' << std::endl;
            if (line.find("\"op\":\"exit\"") != std::string::npos) return 0;
        }
    }
    return 0;
}
