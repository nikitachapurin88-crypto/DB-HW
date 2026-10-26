#include "database.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <variant>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace {

// Имя становится именем файла и частью формата каталога, поэтому строго.
void check_name(const std::string &name) {
  if (name.empty()) {
    throw InvalidName(name);
  }
  for (unsigned char ch : name) {
    if (!std::isalnum(ch) && ch != '_') {
      throw InvalidName(name);
    }
  }
}

std::size_t column_index(const Schema &schema, const std::string &name);

// Проверка схемы самой по себе (без обращения к другим таблицам).
void validate_schema(const Schema &schema) {
  if (schema.columns.empty()) {
    throw DatabaseError("schema has no columns");
  }
  if (schema.pk >= schema.columns.size()) {
    throw DatabaseError("primary key index is out of range");
  }
  std::unordered_set<std::string> seen;
  for (const auto &col : schema.columns) {
    check_name(col.name);
    if (!seen.insert(col.name).second) {
      throw DatabaseError("duplicate column: " + col.name);
    }
  }

  std::unordered_set<std::string> fk_columns;
  for (const auto &fk : schema.fks) {
    const std::size_t ci = column_index(schema, fk.column);
    if (schema.columns[ci].type != ColumnType::Int) {
      throw DatabaseError("foreign key column must be of type int: " + fk.column);
    }
    if (!fk_columns.insert(fk.column).second) {
      throw DatabaseError("duplicate foreign key on column: " + fk.column);
    }
    check_name(fk.ref_table);
    check_name(fk.ref_column);
  }
}

std::size_t column_index(const Schema &schema, const std::string &name) {
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].name == name) {
      return i;
    }
  }
  throw DatabaseError("no such column: " + name);
}

// Формат каталога: по строке на таблицу
//   experiments;id:int;dataset_id:int;accuracy:float;fk=dataset_id>datasets.id;pk=id
// Части fk= и pk= распознаются по префиксу, порядок среди них не важен.

const char *type_to_string(ColumnType t) {
  switch (t) {
  case ColumnType::Int:
    return "int";
  case ColumnType::Float:
    return "float";
  case ColumnType::String:
    return "text";
  }
  throw CatalogError("unknown column type");
}

ColumnType type_from_string(const std::string &s) {
  if (s == "int")
    return ColumnType::Int;
  if (s == "float")
    return ColumnType::Float;
  if (s == "text")
    return ColumnType::String;
  throw CatalogError("unknown column type in catalog: " + s);
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> parts;
  std::string cur;
  for (char ch : s) {
    if (ch == sep) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  parts.push_back(cur);
  return parts;
}

} // namespace

// ---------- конструктор ----------

Database::Database(const fs::path &dir)
    : path_(dir), catalog_path_(dir / "catalog.txt") {
  fs::create_directories(path_);
  load_catalog();
}

// ---------- схема ----------

void Database::create_table(const std::string &name, const Schema &schema) {
  check_name(name);
  validate_schema(schema);
  validate_foreign_keys(schema);

  auto [it, inserted] = tables_.try_emplace(name, table_file(name), schema);
  if (!inserted) {
    throw TableExists(name);
  }

  try {
    save_catalog();
  } catch (...) {
    // откат: в памяти и на диске не должно остаться таблицы без записи в каталоге
    tables_.erase(it);
    std::error_code ec;
    fs::remove(table_file(name), ec);
    throw;
  }
}

void Database::drop_table(const std::string &name) {
  // нельзя удалить таблицу, на которую ссылается внешний ключ другой таблицы
  for (const auto &entry : tables_) {
    if (entry.first == name) {
      continue;
    }
    for (const auto &fk : entry.second.schema().fks) {
      if (fk.ref_table == name) {
        throw DatabaseError("cannot drop table '" + name + "': referenced by " +
                            entry.first + "." + fk.column);
      }
    }
  }

  auto node = tables_.extract(name);
  if (node.empty()) {
    throw TableNotFound(name);
  }
  try {
    save_catalog();
  } catch (...) {
    tables_.insert(std::move(node)); // вернуть как было
    throw;
  }
  node = {}; // уничтожить Table (закрыть файл) до удаления файла
  std::error_code ec;
  fs::remove(table_file(name), ec);
}

bool Database::has_table(const std::string &name) const {
  return tables_.find(name) != tables_.end();
}

std::vector<std::string> Database::list_tables() const {
  std::vector<std::string> names;
  names.reserve(tables_.size());
  for (const auto &entry : tables_) {
    names.push_back(entry.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

const Schema &Database::schema(const std::string &name) const {
  return get_table(name).schema();
}

// ---------- CRUD ----------

void Database::check_parent_exists(const ForeignKey &fk,
                                   const Value &value) const {
  if (!std::holds_alternative<std::int64_t>(value)) {
    throw TypeMismatch("foreign key " + fk.column + " must be an integer");
  }
  const Condition cond{fk.ref_column, Op::Eq, value};
  if (get_table(fk.ref_table).select(cond).rows.empty()) {
    throw ForeignKeyError("foreign key violation: " + fk.column + " = " +
                          std::to_string(std::get<std::int64_t>(value)) +
                          " does not exist in " + fk.ref_table + "." +
                          fk.ref_column);
  }
}

void Database::check_not_referenced(
    const std::string &parent_name,
    const std::optional<Condition> &cond) const {
  const ResultSet doomed = get_table(parent_name).select(cond);
  if (doomed.rows.empty()) {
    return;
  }
  for (const auto &entry : tables_) {
    for (const auto &fk : entry.second.schema().fks) {
      if (fk.ref_table != parent_name) {
        continue;
      }
      // индекс ссылочного столбца в родителе (это PK)
      const Schema &ps = get_table(parent_name).schema();
      for (const auto &row : doomed.rows) {
        const Value &v = row[ps.pk];
        const Condition c{fk.column, Op::Eq, v};
        if (!entry.second.select(c).rows.empty()) {
          throw ForeignKeyError(
              "cannot delete " + parent_name + "." + fk.ref_column + " = " +
              std::to_string(std::get<std::int64_t>(v)) + ": referenced by " +
              entry.first + "." + fk.column);
        }
      }
    }
  }
}

void Database::insert(const std::string &name, const Row &row) {
  Table &t = get_table(name);
  const Schema &s = t.schema();
  if (row.size() == s.columns.size()) {
    for (const auto &fk : s.fks) {
      for (std::size_t i = 0; i < s.columns.size(); ++i) {
        if (s.columns[i].name == fk.column) {
          check_parent_exists(fk, row[i]);
        }
      }
    }
  }
  t.insert(row);
}

ResultSet Database::select(const std::string &name,
                           const std::optional<Condition> &cond,
                           const std::vector<std::string> &columns) const {
  ResultSet full = get_table(name).select(cond);
  if (columns.empty()) {
    return full;
  }
  std::vector<std::size_t> idx;
  for (const auto &c : columns) {
    const auto it = std::find(full.columns.begin(), full.columns.end(), c);
    if (it == full.columns.end()) {
      throw DatabaseError("no such column: " + c);
    }
    idx.push_back(static_cast<std::size_t>(it - full.columns.begin()));
  }
  ResultSet out;
  out.columns = columns;
  for (const auto &row : full.rows) {
    Row r;
    for (auto i : idx) {
      r.push_back(row[i]);
    }
    out.rows.push_back(std::move(r));
  }
  return out;
}

std::size_t Database::row_count(const std::string &name) const {
  return get_table(name).row_count();
}

std::size_t Database::update(const std::string &name,
                             const std::optional<Condition> &cond,
                             const Changes &changes) {
  Table &t = get_table(name);
  for (const auto &fk : t.schema().fks) {
    for (const auto &ch : changes) {
      if (ch.first == fk.column) {
        check_parent_exists(fk, ch.second);
      }
    }
  }
  return t.update(cond, changes);
}

std::size_t Database::remove(const std::string &name,
                             const std::optional<Condition> &cond) {
  Table &t = get_table(name);
  check_not_referenced(name, cond);
  return t.remove(cond);
}

// ---------- JOIN ----------

ResultSet Database::join(const std::string &left, const std::string &right,
                         const std::string &left_col,
                         const std::string &right_col) const {
  const Table &l = get_table(left);
  const Table &r = get_table(right);
  const std::size_t li = column_index(l.schema(), left_col);
  const std::size_t ri = column_index(r.schema(), right_col);

  const ResultSet lrows = l.select(std::nullopt);
  const ResultSet rrows = r.select(std::nullopt);

  ResultSet out;
  for (const auto &c : l.schema().columns) {
    out.columns.push_back(left + "." + c.name);
  }
  for (const auto &c : r.schema().columns) {
    out.columns.push_back(right + "." + c.name);
  }

  for (const Row &a : lrows.rows) {
    for (const Row &b : rrows.rows) {
      if (a[li] == b[ri]) {
        Row joined = a;
        joined.insert(joined.end(), b.begin(), b.end());
        out.rows.push_back(std::move(joined));
      }
    }
  }
  return out;
}

// ---------- backup ----------

void Database::backup(const fs::path &dest) const {
  const fs::path src_abs = fs::weakly_canonical(path_);
  const fs::path dest_abs = fs::weakly_canonical(dest);
  const fs::path rel = dest_abs.lexically_relative(src_abs);
  if (!rel.empty() && *rel.begin() != "..") {
    throw DatabaseError("backup destination is inside the database directory");
  }
  fs::create_directories(dest);
  fs::copy(path_, dest,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

void Database::compact(const std::string &name) {
  get_table(name).compact();
}

// ---------- приватное ----------

Table &Database::get_table(const std::string &name) {
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    throw TableNotFound(name);
  }
  return it->second;
}

const Table &Database::get_table(const std::string &name) const {
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    throw TableNotFound(name);
  }
  return it->second;
}

fs::path Database::table_file(const std::string &name) const {
  return path_ / (name + ".dat");
}

void Database::save_catalog() const {
  // пишем во временный файл и переименовываем: падение посреди записи
  // не оставит битый каталог
  fs::path tmp = catalog_path_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      throw CatalogError("cannot write catalog: " + tmp.string());
    }
    for (const auto &entry : tables_) {
      const Schema &s = entry.second.schema();
      out << entry.first;
      for (const auto &col : s.columns) {
        out << ';' << col.name << ':' << type_to_string(col.type);
      }
      for (const auto &fk : s.fks) {
        out << ";fk=" << fk.column << '>' << fk.ref_table << '.' << fk.ref_column;
      }
      out << ";pk=" << s.columns[s.pk].name << '\n';
    }
    out.flush();
    if (!out) {
      throw CatalogError("failed while writing catalog");
    }
  }
  fs::rename(tmp, catalog_path_);
}

void Database::load_catalog() {
  std::ifstream in(catalog_path_);
  if (!in) {
    return; // первый запуск: каталога ещё нет
  }
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const std::string where = " (catalog line " + std::to_string(lineno) + ")";
    const auto parts = split(line, ';');
    if (parts.size() < 3) {
      throw CatalogError("malformed entry" + where);
    }

    const std::string &name = parts[0];
    check_name(name);

    Schema schema;
    std::string pk_name;
    for (std::size_t i = 1; i < parts.size(); ++i) {
      if (parts[i].rfind("pk=", 0) == 0) {
        pk_name = parts[i].substr(3);
        continue;
      }
      if (parts[i].rfind("fk=", 0) == 0) {
        // fk=<столбец>><таблица>.<столбец>
        const std::string spec = parts[i].substr(3);
        const auto arrow = spec.find('>');
        const auto dot = spec.find('.', arrow == std::string::npos ? 0 : arrow);
        if (arrow == std::string::npos || dot == std::string::npos) {
          throw CatalogError("malformed foreign key" + where);
        }
        schema.fks.push_back(ForeignKey{spec.substr(0, arrow),
                                        spec.substr(arrow + 1, dot - arrow - 1),
                                        spec.substr(dot + 1)});
        continue;
      }
      const auto pos = parts[i].find(':');
      if (pos == std::string::npos) {
        throw CatalogError("malformed column" + where);
      }
      schema.columns.push_back(Column{parts[i].substr(0, pos),
                                      type_from_string(parts[i].substr(pos + 1))});
    }
    if (pk_name.empty()) {
      throw CatalogError("missing primary key" + where);
    }
    schema.pk = column_index(schema, pk_name);
    validate_schema(schema);

    if (!tables_.try_emplace(name, table_file(name), schema).second) {
      throw CatalogError("duplicate table" + where);
    }
  }

  // ссылки проверяем, когда загружены все таблицы: порядок строк в каталоге
  // произвольный, и родительская таблица может встретиться позже дочерней
  for (const auto &entry : tables_) {
    validate_foreign_keys(entry.second.schema());
  }
}

void Database::validate_foreign_keys(const Schema &schema) const {
  for (const auto &fk : schema.fks) {
    const auto it = tables_.find(fk.ref_table);
    if (it == tables_.end()) {
      throw DatabaseError("foreign key " + fk.column +
                          " references unknown table: " + fk.ref_table);
    }
    const Schema &ref = it->second.schema();
    if (ref.columns[ref.pk].name != fk.ref_column) {
      throw DatabaseError("foreign key " + fk.column +
                          " must reference the primary key of " + fk.ref_table +
                          " (" + ref.columns[ref.pk].name + ")");
    }
  }
}
