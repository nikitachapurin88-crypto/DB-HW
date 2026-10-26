#include "table.h"

#include <cstring>

namespace fs = std::filesystem;

namespace {

std::size_t column_size(ColumnType t) {
  switch (t) {
  case ColumnType::Int:
  case ColumnType::Float:
    return 8;
  case ColumnType::Text:
    return kMaxTextBytes;
  }
  throw DatabaseError("unknown column type");
}

double to_double(const Value &v) {
  if (const auto *p = std::get_if<std::int64_t>(&v)) {
    return static_cast<double>(*p);
  }
  return std::get<double>(v);
}

// -1 / 0 / 1; числа сравниваются как числа, текст с текстом.
int compare_values(const Value &a, const Value &b) {
  const bool a_text = std::holds_alternative<std::string>(a);
  const bool b_text = std::holds_alternative<std::string>(b);
  if (a_text || b_text) {
    if (a_text != b_text) {
      throw TypeMismatch("cannot compare text with a number");
    }
    const int c = std::get<std::string>(a).compare(std::get<std::string>(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  if (std::holds_alternative<std::int64_t>(a) &&
      std::holds_alternative<std::int64_t>(b)) {
    const auto x = std::get<std::int64_t>(a);
    const auto y = std::get<std::int64_t>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  const double x = to_double(a);
  const double y = to_double(b);
  return x < y ? -1 : (x > y ? 1 : 0);
}

bool satisfies(int cmp, Op op) {
  switch (op) {
  case Op::Eq:
    return cmp == 0;
  case Op::Ne:
    return cmp != 0;
  case Op::Lt:
    return cmp < 0;
  case Op::Le:
    return cmp <= 0;
  case Op::Gt:
    return cmp > 0;
  case Op::Ge:
    return cmp >= 0;
  }
  return false;
}

void check_comparable(const Column &col, const Value &v) {
  const bool col_text = col.type == ColumnType::Text;
  const bool val_text = std::holds_alternative<std::string>(v);
  if (col_text != val_text) {
    throw TypeMismatch("condition value does not match column '" + col.name +
                       "'");
  }
}

} // namespace

// ---------- создание / открытие ----------

Table::Table(const fs::path &file, const Schema &schema)
    : path_(file), schema_(schema) {
  if (schema_.columns.empty() || schema_.pk >= schema_.columns.size()) {
    throw DatabaseError("invalid schema");
  }
  if (schema_.columns[schema_.pk].type != ColumnType::Int) {
    throw DatabaseError("primary key column must be of type int");
  }

  record_size_ = 1; // флаг «жива / удалена»
  for (const auto &col : schema_.columns) {
    col_offsets_.push_back(record_size_);
    record_size_ += column_size(col.type);
  }

  open_file();
  rebuild_index();
}

void Table::open_file() {
  if (!fs::exists(path_)) {
    std::ofstream create(path_, std::ios::binary);
    if (!create) {
      throw DatabaseError("cannot create table file: " + path_.string());
    }
  }
  file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_) {
    throw DatabaseError("cannot open table file: " + path_.string());
  }
}

void Table::rebuild_index() {
  index_.clear();
  const std::uintmax_t size = fs::file_size(path_);
  if (size % record_size_ != 0) {
    throw DatabaseError("corrupted table file (size is not a multiple of "
                        "the record size): " +
                        path_.string());
  }
  record_count_ = static_cast<std::size_t>(size / record_size_);

  file_.clear();
  file_.seekg(0);
  Record rec(record_size_);
  for (std::size_t i = 0; i < record_count_; ++i) {
    file_.read(rec.data(), static_cast<std::streamsize>(rec.size()));
    if (!file_) {
      throw DatabaseError("read failed while building index");
    }
    if (rec[0] != 0) {
      const auto key = std::get<std::int64_t>(decode_column(rec, schema_.pk));
      index_[key] = static_cast<std::uint64_t>(i) * record_size_;
    }
  }
}

// ---------- проверка и приведение значений ----------

std::size_t Table::column_index(const std::string &name) const {
  for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
    if (schema_.columns[i].name == name) {
      return i;
    }
  }
  throw DatabaseError("no such column: " + name);
}

Value Table::normalize_value(const Column &col, const Value &v) const {
  switch (col.type) {
  case ColumnType::Int:
    if (std::holds_alternative<std::int64_t>(v)) {
      return v;
    }
    break;
  case ColumnType::Float:
    if (std::holds_alternative<double>(v)) {
      return v;
    }
    if (const auto *p = std::get_if<std::int64_t>(&v)) {
      return static_cast<double>(*p);
    }
    break;
  case ColumnType::Text:
    if (const auto *s = std::get_if<std::string>(&v)) {
      if (s->size() > kMaxTextBytes) {
        throw DatabaseError("text too long for column '" + col.name +
                            "' (max " + std::to_string(kMaxTextBytes) +
                            " bytes)");
      }
      return v;
    }
    break;
  }
  throw TypeMismatch("wrong value type for column '" + col.name + "'");
}

Row Table::normalize_row(const Row &row) const {
  if (row.size() != schema_.columns.size()) {
    throw DatabaseError("row has " + std::to_string(row.size()) +
                        " values, table has " +
                        std::to_string(schema_.columns.size()) + " columns");
  }
  Row out;
  out.reserve(row.size());
  for (std::size_t i = 0; i < row.size(); ++i) {
    out.push_back(normalize_value(schema_.columns[i], row[i]));
  }
  return out;
}

// ---------- кодирование записи ----------

void Table::encode_column(Record &rec, std::size_t ci, const Value &v) const {
  char *p = rec.data() + col_offsets_[ci];
  switch (schema_.columns[ci].type) {
  case ColumnType::Int: {
    const auto x = std::get<std::int64_t>(v);
    std::memcpy(p, &x, sizeof(x));
    return;
  }
  case ColumnType::Float: {
    const auto x = std::get<double>(v);
    std::memcpy(p, &x, sizeof(x));
    return;
  }
  case ColumnType::Text: {
    const auto &s = std::get<std::string>(v);
    std::memset(p, 0, kMaxTextBytes);
    std::memcpy(p, s.data(), s.size());
    return;
  }
  }
}

Value Table::decode_column(const Record &rec, std::size_t ci) const {
  const char *p = rec.data() + col_offsets_[ci];
  switch (schema_.columns[ci].type) {
  case ColumnType::Int: {
    std::int64_t x;
    std::memcpy(&x, p, sizeof(x));
    return x;
  }
  case ColumnType::Float: {
    double x;
    std::memcpy(&x, p, sizeof(x));
    return x;
  }
  case ColumnType::Text: {
    std::size_t n = 0;
    while (n < kMaxTextBytes && p[n] != '\0') {
      ++n;
    }
    return std::string(p, n);
  }
  }
  throw DatabaseError("unknown column type");
}

Table::Record Table::encode(const Row &row) const {
  Record rec(record_size_, 0);
  rec[0] = 1;
  for (std::size_t i = 0; i < row.size(); ++i) {
    encode_column(rec, i, row[i]);
  }
  return rec;
}

Row Table::decode(const Record &rec) const {
  Row row;
  row.reserve(schema_.columns.size());
  for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
    row.push_back(decode_column(rec, i));
  }
  return row;
}

// ---------- ввод-вывод ----------

Table::Record Table::read_record(std::uint64_t offset) const {
  Record rec(record_size_);
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(offset));
  file_.read(rec.data(), static_cast<std::streamsize>(rec.size()));
  if (!file_) {
    throw DatabaseError("read failed: " + path_.string());
  }
  return rec;
}

void Table::write_record(std::uint64_t offset, const Record &rec) {
  file_.clear();
  file_.seekp(static_cast<std::streamoff>(offset));
  file_.write(rec.data(), static_cast<std::streamsize>(rec.size()));
  file_.flush();
  if (!file_) {
    throw DatabaseError("write failed: " + path_.string());
  }
}

Table::Records
Table::matching_records(const std::optional<Condition> &cond) const {
  Records out;
  std::size_t ci = 0;
  if (cond) {
    ci = column_index(cond->column);
    check_comparable(schema_.columns[ci], cond->value);
    if (ci == schema_.pk && cond->op == Op::Eq) {
      if (const auto *key = std::get_if<std::int64_t>(&cond->value)) {
        const auto it = index_.find(*key);
        if (it != index_.end()) {
          out.emplace_back(it->second, read_record(it->second));
        }
        return out;
      }
    }
  }

  file_.clear();
  file_.seekg(0);
  Record rec(record_size_);
  for (std::size_t i = 0; i < record_count_; ++i) {
    file_.read(rec.data(), static_cast<std::streamsize>(rec.size()));
    if (!file_) {
      throw DatabaseError("read failed: " + path_.string());
    }
    if (rec[0] == 0) {
      continue;
    }
    if (cond && !satisfies(compare_values(decode_column(rec, ci), cond->value),
                           cond->op)) {
      continue;
    }
    out.emplace_back(static_cast<std::uint64_t>(i) * record_size_, rec);
  }
  return out;
}

// ---------- CRUD ----------

void Table::insert(const Row &row) {
  const Row r = normalize_row(row);
  const auto key = std::get<std::int64_t>(r[schema_.pk]);
  if (index_.count(key) != 0) {
    throw DuplicateKey(key);
  }
  const std::uint64_t offset =
      static_cast<std::uint64_t>(record_count_) * record_size_;
  write_record(offset, encode(r));
  ++record_count_;
  index_[key] = offset;
}

ResultSet Table::select(const std::optional<Condition> &cond) const {
  ResultSet out;
  for (const auto &col : schema_.columns) {
    out.columns.push_back(col.name);
  }
  for (const auto &entry : matching_records(cond)) {
    out.rows.push_back(decode(entry.second));
  }
  return out;
}

std::size_t Table::update(const std::optional<Condition> &cond,
                          const Changes &changes) {
  // сначала проверяем все изменения, и только потом трогаем файл
  std::vector<std::pair<std::size_t, Value>> prepared;
  for (const auto &change : changes) {
    const std::size_t ci = column_index(change.first);
    if (ci == schema_.pk) {
      throw DatabaseError("primary key cannot be changed");
    }
    prepared.emplace_back(ci,
                          normalize_value(schema_.columns[ci], change.second));
  }

  auto matches = matching_records(cond);
  for (auto &entry : matches) {
    for (const auto &p : prepared) {
      encode_column(entry.second, p.first, p.second);
    }
    write_record(entry.first, entry.second);
  }
  return matches.size();
}

std::size_t Table::remove(const std::optional<Condition> &cond) {
  const auto matches = matching_records(cond);
  for (const auto &entry : matches) {
    Record rec = entry.second;
    rec[0] = 0; // tombstone
    write_record(entry.first, rec);
    index_.erase(std::get<std::int64_t>(decode_column(rec, schema_.pk)));
  }
  return matches.size();
}

// ---------- обслуживание ----------

void Table::compact() {
  Record live;
  file_.clear();
  file_.seekg(0);
  Record rec(record_size_);
  for (std::size_t i = 0; i < record_count_; ++i) {
    file_.read(rec.data(), static_cast<std::streamsize>(rec.size()));
    if (!file_) {
      throw DatabaseError("read failed: " + path_.string());
    }
    if (rec[0] != 0) {
      live.insert(live.end(), rec.begin(), rec.end());
    }
  }

  fs::path tmp = path_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw DatabaseError("cannot write: " + tmp.string());
    }
    out.write(live.data(), static_cast<std::streamsize>(live.size()));
    out.flush();
    if (!out) {
      throw DatabaseError("write failed: " + tmp.string());
    }
  }

  file_.close();
  fs::rename(tmp, path_);
  file_.clear();
  open_file();
  rebuild_index();
}
