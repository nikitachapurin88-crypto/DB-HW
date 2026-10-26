// Benchmark индекса MiniDB: «до» (полный проход) и «после» (индекс id -> offset).
//
// Таблица bench(id PK, code, name), где code == id, но code не проиндексирован.
//   запрос «после»: WHERE id   = k  -> идёт через индекс (один seek)
//   запрос «до»   : WHERE code = k  -> полный проход по файлу
// Оба запроса возвращают ровно одну и ту же строку, различается только путь доступа.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "database.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double elapsed_us(Clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

int main() {
  const std::vector<std::size_t> sizes = {1000, 10000, 50000};
  const int kIndexQueries = 10000;
  const int kScanQueries = 100;
  const fs::path dir = fs::temp_directory_path() / "minidb_bench";

  std::cout << std::fixed << std::setprecision(1);
  std::cout << "| Строк | Размер файла | INSERT, мс (всего) | Открытие + индекс, мс | "
               "Без индекса, мкс/запрос | С индексом, мкс/запрос | Ускорение |\n";
  std::cout << "|---|---|---|---|---|---|---|\n";

  for (std::size_t n : sizes) {
    fs::remove_all(dir);
    std::vector<std::int64_t> keys;
    double insert_ms = 0;
    double open_ms = 0;
    double scan_us = 0;
    double index_us = 0;
    std::uintmax_t file_bytes = 0;
    std::size_t checksum = 0;

    {
      Database db(dir);
      db.create_table("bench", Schema{{{"id", ColumnType::Int},
                                       {"code", ColumnType::Int},
                                       {"name", ColumnType::String}},
                                      0});
      const auto t0 = Clock::now();
      for (std::size_t i = 1; i <= n; ++i) {
        db.insert("bench", Row{std::int64_t(i), std::int64_t(i),
                               "user_" + std::to_string(i)});
      }
      insert_ms = elapsed_us(t0) / 1000.0;
      file_bytes = fs::file_size(dir / "bench.dat");
    }

    // повторное открытие: время восстановления индекса из файла
    {
      const auto t0 = Clock::now();
      Database db(dir);
      open_ms = elapsed_us(t0) / 1000.0;

      std::mt19937_64 rng(12345);
      std::uniform_int_distribution<std::int64_t> pick(1, std::int64_t(n));

      // «после»: поиск по PK через индекс
      {
        const auto t1 = Clock::now();
        for (int q = 0; q < kIndexQueries; ++q) {
          const auto k = pick(rng);
          const auto rs = db.select("bench", Condition{"id", Op::Eq, k});
          if (rs.rows.size() != 1) {
            std::cerr << "unexpected result size\n";
            return 1;
          }
          checksum += rs.rows.size();
        }
        index_us = elapsed_us(t1) / kIndexQueries;
      }

      // «до»: тот же поиск по столбцу без индекса
      {
        const auto t1 = Clock::now();
        for (int q = 0; q < kScanQueries; ++q) {
          const auto k = pick(rng);
          const auto rs = db.select("bench", Condition{"code", Op::Eq, k});
          if (rs.rows.size() != 1) {
            std::cerr << "unexpected result size\n";
            return 1;
          }
          checksum += rs.rows.size();
        }
        scan_us = elapsed_us(t1) / kScanQueries;
      }
    }

    std::cout << "| " << n << " | " << std::setprecision(0)
              << (file_bytes / 1024.0) << " КБ" << std::setprecision(1) << " | "
              << insert_ms << " | " << open_ms << " | " << scan_us << " | "
              << index_us << " | x" << std::setprecision(0) << (scan_us / index_us)
              << std::setprecision(1) << " |\n";
    (void)checksum;
  }

  fs::remove_all(dir);
  return 0;
}
