#pragma once

#include <fstream>
#include <string>
#include <iostream>
#include <cstdlib>   // setenv / putenv

// Reads a .env file (KEY=VALUE lines, # comments ignored) and populates
// the process environment via setenv().  Call once at startup before any
// code reads environment variables.
//
// Lines that are empty or start with '#' are skipped.
// Values are not quoted — strip surrounding quotes if present.
inline void load_dotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        // .env is optional — no error if absent
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Strip inline comments (# preceded by space)
        auto comment = value.find(" #");
        if (comment != std::string::npos) value.resize(comment);

        // Strip surrounding whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(key);
        trim(value);

        // Strip surrounding quotes (" or ')
        if (value.size() >= 2 &&
            ((value.front() == '"'  && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (key.empty()) continue;

        // Only set if not already set in the environment (env vars win)
#ifdef _WIN32
        if (getenv(key.c_str()) == nullptr)
            _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
#endif
    }
}
