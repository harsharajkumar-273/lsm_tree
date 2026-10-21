#include "../src/db/lsm_engine.h"
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>

void printHelp() {
    std::cout << "\n=======================================================\n";
    std::cout << "  ⚡ LSM-Tree Engine Interactive Command-Line Tool\n";
    std::cout << "=======================================================\n";
    std::cout << " Commands:\n";
    std::cout << "  put <key> <value>   - Insert or update a key-value pair\n";
    std::cout << "  get <key>           - Query value for a key\n";
    std::cout << "  del <key>           - Delete key (writes a tombstone)\n";
    std::cout << "  scan <start> [end]  - List live keys in [start, end), up to 100\n";
    std::cout << "  flush               - Manually flush MemTable to SSTable on disk\n";
    std::cout << "  stats               - Show current RAM MemTable & Disk SSTable stats\n";
    std::cout << "  help                - Display this help message\n";
    std::cout << "  exit / quit         - Exit interactive CLI\n";
    std::cout << "=======================================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string db_path = "/tmp/lsm_interactive_db";
    if (argc > 1) {
        db_path = argv[1];
    }

    std::cout << "Opening LSM-Tree Database at: " << db_path << " ...\n";
    LSMEngine db(db_path);
    std::cout << "Database opened successfully!\n";

    printHelp();

    std::string line;
    while (true) {
        std::cout << "lsm-db> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            std::cout << "Closing database. Goodbye!\n";
            break;
        } else if (cmd == "help") {
            printHelp();
        } else if (cmd == "put") {
            std::string key, val;
            ss >> key;
            std::getline(ss >> std::ws, val);
            if (key.empty() || val.empty()) {
                std::cout << "Usage: put <key> <value>\n";
                continue;
            }
            db.put(key, val);
            std::cout << "  \033[32m[OK]\033[0m Stored '" << key << "' => '" << val << "' (WAL logged & MemTable updated)\n";
        } else if (cmd == "get") {
            std::string key;
            ss >> key;
            if (key.empty()) {
                std::cout << "Usage: get <key>\n";
                continue;
            }
            auto res = db.get(key);
            if (res.has_value()) {
                std::cout << "  \033[32m[FOUND]\033[0m Key '" << key << "' => '" << *res << "'\n";
            } else {
                std::cout << "  \033[31m[NOT FOUND]\033[0m Key '" << key << "' does not exist (or was deleted)\n";
            }
        } else if (cmd == "del") {
            std::string key;
            ss >> key;
            if (key.empty()) {
                std::cout << "Usage: del <key>\n";
                continue;
            }
            db.del(key);
            std::cout << "  \033[33m[DELETED]\033[0m Key '" << key << "' marked for deletion\n";
        } else if (cmd == "scan") {
            std::string start, end;
            ss >> start >> end;
            if (start.empty()) {
                std::cout << "Usage: scan <start> [end]\n";
                continue;
            }
            constexpr size_t kMaxRows = 100;
            auto rows = db.scan(start, end.empty() ? std::nullopt : std::optional<std::string>(end), kMaxRows);
            for (const auto& [k, v] : rows) {
                std::cout << "  " << k << " => '" << v << "'\n";
            }
            std::cout << "  \033[32m[" << rows.size() << " KEYS]\033[0m"
                      << (rows.size() == kMaxRows ? " (limit reached)" : "") << "\n";
        } else if (cmd == "flush") {
            db.flush();
            std::cout << "  \033[32m[FLUSHED]\033[0m Active MemTable written out to SSTable on disk!\n";
        } else if (cmd == "stats") {
            std::cout << "  - MemTable Entries: " << db.memtableSize() << "\n";
            std::cout << "  - Active SSTables on Disk: " << db.sstableCount() << "\n";
        } else {
            std::cout << "Unknown command: '" << cmd << "'. Type 'help' for command list.\n";
        }
    }

    return 0;
}
