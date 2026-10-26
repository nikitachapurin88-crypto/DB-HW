#include "database.h"

#include <algorithm>
#include <cctype>
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
//   students;id:int;name:text;score:float;pk=id

const char *type_to_string(ColumnType t) {
  switch (t) {
  case ColumnType::Int:
    return "int";
  case ColumnType::Float:
    return "float";
  case ColumnType::Text:
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
    return ColumnType::Text;
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

  auto [it, inserted] = tables_.try_emplace(name, table_file(name), schema);
  if (!inserted) {
    throw TableExists(name);
  }

  try {
    save_catalog();
  } catch (...) {
    // откат: в памяти и на диске не должно остаться таблицы без записи в
    // каталоге
    tables_.erase(it);
    std::error_code ec;
    fs::remove(table_file(name), ec);
    throw;
  }
}

void Database::drop_table(const std::string &name) {
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

void Database::insert(const std::string &name, const Row &row) {
  get_table(name).insert(row);
}

ResultSet Database::select(const std::string &name,
                           const std::optional<Condition> &cond) const {
  return get_table(name).select(cond);
}

std::size_t Database::update(const std::string &name,
                             const std::optional<Condition> &cond,
                             const Changes &changes) {
  return get_table(name).update(cond, changes);
}

std::size_t Database::remove(const std::string &name,
                             const std::optional<Condition> &cond) {
  return get_table(name).remove(cond);
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

void Database::compact(const std::string &name) { get_table(name).compact(); }

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
      const auto pos = parts[i].find(':');
      if (pos == std::string::npos) {
        throw CatalogError("malformed column" + where);
      }
      schema.columns.push_back(Column{
          parts[i].substr(0, pos), type_from_string(parts[i].substr(pos + 1))});
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
}
