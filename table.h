#ifndef TABLE_H
#define TABLE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "exceptions.h"

// ---------- типы данных ----------

enum class ColumnType { Int, Float, Text };

struct Column {
  std::string name;
  ColumnType type;
};

struct Schema {
  std::vector<Column> columns;
  std::size_t pk = 0;
};

using Value = std::variant<std::int64_t, double, std::string>;
using Row = std::vector<Value>;

enum class Op { Eq, Ne, Lt, Le, Gt, Ge };

// Простое условие: <column> <op> <value>
struct Condition {
  std::string column;
  Op op;
  Value value;
};

// Список присваиваний для UPDATE: {столбец, новое значение}
using Changes = std::vector<std::pair<std::string, Value>>;

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

// Максимальная длина текстового поля в байтах (поле фиксированной ширины).
constexpr std::size_t kMaxTextBytes = 64;

// ---------- таблица ----------
//
// Файл данных: записи фиксированной длины подряд, без заголовка.
//   запись = [1 байт флаг: 1 живая / 0 удалена] + значения столбцов
//   Int / Float -> 8 байт, Text -> kMaxTextBytes байт (добивается нулями)
// Смещение записи в файле = номер_слота * размер_записи.
// Индекс в памяти: значение PK -> смещение; строится при открытии файла.
// DELETE ставит флаг 0 (tombstone), compact() физически убирает такие записи.

class Table {
public:
  Table(const std::filesystem::path &file, const Schema &schema);

  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;
  Table(Table &&) = default;
  Table &operator=(Table &&) = default;

  const Schema &schema() const { return schema_; }
  std::size_t row_count() const { return index_.size(); }

  void insert(const Row &row);
  ResultSet select(const std::optional<Condition> &cond = std::nullopt) const;
  // PK менять нельзя. Возвращает число изменённых строк.
  std::size_t update(const std::optional<Condition> &cond,
                     const Changes &changes);
  // Возвращает число удалённых строк.
  std::size_t remove(const std::optional<Condition> &cond);

  // Переписывает файл без удалённых записей и перестраивает индекс.
  void compact();

private:
  using Record = std::vector<char>;
  using Records = std::vector<std::pair<std::uint64_t, Record>>;

  std::size_t column_index(const std::string &name) const;
  Value normalize_value(const Column &col, const Value &v) const;
  Row normalize_row(const Row &row) const;

  Record encode(const Row &row) const;
  Row decode(const Record &rec) const;
  Value decode_column(const Record &rec, std::size_t ci) const;
  void encode_column(Record &rec, std::size_t ci, const Value &v) const;

  Record read_record(std::uint64_t offset) const;
  void write_record(std::uint64_t offset, const Record &rec);
  Records matching_records(const std::optional<Condition> &cond) const;

  void open_file();
  void rebuild_index();

  std::filesystem::path path_;
  Schema schema_;
  std::size_t record_size_ = 0;
  std::vector<std::size_t> col_offsets_;
  std::size_t record_count_ = 0;
  mutable std::fstream file_;
  std::unordered_map<std::int64_t, std::uint64_t> index_;
};

#endif // TABLE_H
