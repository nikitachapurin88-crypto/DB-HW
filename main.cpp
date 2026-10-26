// MiniDB: консольная программа.
//
//   ./minidb                          интерактивный режим (база в ./minidb_data)
//   ./minidb --script файл.txt        выполнить команды из файла и выйти
//   ./minidb --db папка               использовать другую папку базы
//   ./minidb --reset                  удалить базу (можно вместе с --script)

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "cli.h"
#include "database.h"

namespace fs = std::filesystem;

namespace {

void usage() {
  std::cout << "Usage: minidb [--db <dir>] [--script <file>] [--reset]\n"
               "  (no arguments)   interactive mode, database in ./minidb_data\n"
               "  --script <file>  run commands from the file and exit\n"
               "  --db <dir>       database directory\n"
               "  --reset          delete the database directory first\n";
}

} // namespace

int main(int argc, char **argv) {
  fs::path dir = "minidb_data";
  std::string script;
  bool reset = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc) {
      dir = argv[++i];
    } else if (arg == "--script" && i + 1 < argc) {
      script = argv[++i];
    } else if (arg == "--reset") {
      reset = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      usage();
      return 2;
    }
  }

  try {
    if (reset) {
      fs::remove_all(dir);
      std::cout << "Database removed: " << dir.string() << '\n';
      if (script.empty()) {
        return 0;
      }
    }

    Database db(dir);

    if (!script.empty()) {
      std::ifstream in(script);
      if (!in) {
        std::cerr << "cannot open script: " << script << '\n';
        return 1;
      }
      const int errors = run_cli(db, in, std::cout, false, true);
      if (errors > 0) {
        std::cout << '\n' << errors << " command(s) failed\n";
      }
      return 0;
    }

    std::cout << "MiniDB, database: " << dir.string()
              << "  (type 'help' for commands, 'exit' to quit)\n";
    run_cli(db, std::cin, std::cout, true, false);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
