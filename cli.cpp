#include "cli.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <istream>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

namespace {

// ---------- лексер ----------

enum class Kind { Word, Str, Op, Punct };

struct Token {
  Kind kind;
  std::string text;
};

std::string lower(std::string s) {
  for (auto &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// Слово: имена, числа (в том числе отрицательные и дробные), пути.
bool is_word_char(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '.' || c == '+' || c == '-' ||
         c == '/' || c == '\\' || c == ':' || c == '~' || c >= 128;
}

std::vector<Token> tokenize(const std::string &line) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  const std::size_t n = line.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (std::isspace(c) || c == ';') {
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      const char quote = line[i++];
      std::string text;
      while (i < n && line[i] != quote) {
        text += line[i++];
      }
      if (i >= n) {
        throw CliError("unterminated string literal");
      }
      ++i; // закрывающая кавычка
      tokens.push_back({Kind::Str, text});
      continue;
    }
    if (c == '(' || c == ')' || c == ',' || c == '*') {
      tokens.push_back({Kind::Punct, std::string(1, line[i])});
      ++i;
      continue;
    }
    if (c == '=' || c == '<' || c == '>' || c == '!') {
      std::string op(1, line[i++]);
      if (i < n && line[i] == '=') {
        op += line[i++];
      }
      if (op != "=" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
          op != ">=") {
        throw CliError("unknown operator '" + op + "'");
      }
      tokens.push_back({Kind::Op, op});
      continue;
    }
    if (is_word_char(c)) {
      std::string word;
      while (i < n && is_word_char(static_cast<unsigned char>(line[i]))) {
        word += line[i++];
      }
      tokens.push_back({Kind::Word, word});
      continue;
    }
    throw CliError(std::string("unexpected character '") + line[i] + "'");
  }
  return tokens;
}

// ---------- разбор ----------

class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  bool at_end() const { return pos_ >= tokens_.size(); }

  Token next() {
    if (at_end()) {
      throw CliError("unexpected end of command");
    }
    return tokens_[pos_++];
  }

  bool peek_word(const char *keyword) const {
    return !at_end() && tokens_[pos_].kind == Kind::Word &&
           lower(tokens_[pos_].text) == keyword;
  }

  bool accept_word(const char *keyword) {
    if (peek_word(keyword)) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect_word(const char *keyword) {
    if (!accept_word(keyword)) {
      throw CliError(std::string("expected '") + keyword + "'");
    }
  }

  bool accept_punct(char c) {
    if (!at_end() && tokens_[pos_].kind == Kind::Punct &&
        tokens_[pos_].text[0] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect_punct(char c) {
    if (!accept_punct(c)) {
      throw CliError(std::string("expected '") + c + "'");
    }
  }

  std::string expect_name() {
    const Token t = next();
    if (t.kind != Kind::Word) {
      throw CliError("expected a name, got '" + t.text + "'");
    }
    return t.text;
  }

  void expect_end() const {
    if (at_end()) {
      return;
    }
    if (peek_word("and") || peek_word("or")) {
      throw CliError("AND/OR are not supported: use a single condition");
    }
    throw CliError("unexpected '" + tokens_[pos_].text + "'");
  }

private:
  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

ColumnType parse_type(const std::string &word) {
  const std::string t = lower(word);
  if (t == "int" || t == "integer") {
    return ColumnType::Int;
  }
  if (t == "float" || t == "double") {
    return ColumnType::Float;
  }
  if (t == "string" || t == "text") {
    return ColumnType::String;
  }
  throw CliError("unknown type '" + word + "' (use INT, FLOAT or STRING)");
}

const char *type_name(ColumnType t) {
  switch (t) {
  case ColumnType::Int:
    return "INT";
  case ColumnType::Float:
    return "FLOAT";
  case ColumnType::String:
    return "STRING";
  }
  return "?";
}

std::size_t index_of(const Schema &schema, const std::string &name) {
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].name == name) {
      return i;
    }
  }
  throw CliError("no such column: " + name);
}

std::int64_t parse_int(const std::string &s, const std::string &column) {
  std::size_t pos = 0;
  long long v = 0;
  try {
    v = std::stoll(s, &pos);
  } catch (const std::exception &) {
    pos = 0;
  }
  if (pos == 0 || pos != s.size()) {
    throw CliError("'" + s + "' is not an integer (column " + column + ")");
  }
  return static_cast<std::int64_t>(v);
}

double parse_double(const std::string &s, const std::string &column) {
  std::size_t pos = 0;
  double v = 0;
  try {
    v = std::stod(s, &pos);
  } catch (const std::exception &) {
    pos = 0;
  }
  if (pos == 0 || pos != s.size()) {
    throw CliError("'" + s + "' is not a number (column " + column + ")");
  }
  return v;
}

// Значение приводится к типу столбца. `loose` (для WHERE) разрешает
// сравнивать целый столбец с дробным числом.
Value parse_value(const Token &tok, ColumnType type, const std::string &column,
                  bool loose = false) {
  switch (type) {
  case ColumnType::String:
    if (tok.kind == Kind::Str || tok.kind == Kind::Word) {
      return tok.text;
    }
    break;
  case ColumnType::Int:
    if (tok.kind == Kind::Word) {
      if (loose && tok.text.find_first_of(".eE") != std::string::npos) {
        return parse_double(tok.text, column);
      }
      return parse_int(tok.text, column);
    }
    break;
  case ColumnType::Float:
    if (tok.kind == Kind::Word) {
      return parse_double(tok.text, column);
    }
    break;
  }
  throw CliError("invalid value for column '" + column + "'");
}

Op parse_op(const std::string &s) {
  if (s == "=") return Op::Eq;
  if (s == "!=") return Op::Ne;
  if (s == "<") return Op::Lt;
  if (s == "<=") return Op::Le;
  if (s == ">") return Op::Gt;
  return Op::Ge;
}

// where <столбец> <операция> <значение>
std::optional<Condition> parse_where(Parser &p, const Schema &schema) {
  if (!p.accept_word("where")) {
    return std::nullopt;
  }
  const std::string column = p.expect_name();
  const Column &col = schema.columns[index_of(schema, column)];
  const Token op = p.next();
  if (op.kind != Kind::Op) {
    throw CliError("expected a comparison operator (= != < <= > >=)");
  }
  const Value value = parse_value(p.next(), col.type, column, true);
  return Condition{column, parse_op(op.text), value};
}

std::pair<std::string, std::string> split_qualified(const std::string &name) {
  const auto dot = name.find('.');
  if (dot == std::string::npos) {
    throw CliError("expected <table>.<column>, got '" + name + "'");
  }
  return {name.substr(0, dot), name.substr(dot + 1)};
}

// ---------- вывод ----------

std::string to_text(const Value &v) {
  if (const auto *i = std::get_if<std::int64_t>(&v)) {
    return std::to_string(*i);
  }
  if (const auto *d = std::get_if<double>(&v)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g", *d);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) {
      s += ".0"; // чтобы float было видно: 1.0, а не 1
    }
    return s;
  }
  return std::get<std::string>(v);
}

// Ширина строки в символах (UTF-8: байты-продолжения не считаются).
std::size_t display_width(const std::string &s) {
  std::size_t w = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) {
      ++w;
    }
  }
  return w;
}

void print_result(std::ostream &out, const ResultSet &rs) {
  const std::size_t n = rs.columns.size();
  std::vector<std::size_t> width(n);
  std::vector<std::vector<std::string>> cells;
  for (std::size_t i = 0; i < n; ++i) {
    width[i] = display_width(rs.columns[i]);
  }
  for (const auto &row : rs.rows) {
    std::vector<std::string> line;
    for (std::size_t i = 0; i < n; ++i) {
      line.push_back(to_text(row[i]));
      width[i] = std::max(width[i], display_width(line.back()));
    }
    cells.push_back(std::move(line));
  }

  const auto print_line = [&](const std::vector<std::string> &values) {
    for (std::size_t i = 0; i < n; ++i) {
      out << values[i];
      if (i + 1 < n) {
        out << std::string(width[i] - display_width(values[i]), ' ') << " | ";
      }
    }
    out << '\n';
  };

  print_line(rs.columns);
  for (std::size_t i = 0; i < n; ++i) {
    out << std::string(width[i], '-');
    if (i + 1 < n) {
      out << "-+-";
    }
  }
  out << '\n';
  for (const auto &line : cells) {
    print_line(line);
  }
  out << '(' << rs.rows.size() << (rs.rows.size() == 1 ? " row)\n" : " rows)\n");
}

// ---------- команды ----------

void cmd_create(Database &db, Parser &p, std::ostream &out) {
  p.expect_word("table");
  const std::string name = p.expect_name();
  p.expect_punct('(');
  Schema schema;
  while (true) {
    const std::string col = p.expect_name();
    schema.columns.push_back(Column{col, parse_type(p.expect_name())});
    if (p.accept_punct(',')) {
      continue;
    }
    p.expect_punct(')');
    break;
  }
  p.expect_word("primary");
  p.expect_word("key");
  schema.pk = index_of(schema, p.expect_name());
  while (p.accept_word("foreign")) {
    p.expect_word("key");
    const std::string col = p.expect_name();
    p.expect_word("references");
    const auto ref = split_qualified(p.expect_name());
    schema.fks.push_back(ForeignKey{col, ref.first, ref.second});
  }
  p.expect_end();
  db.create_table(name, schema);
  out << "table " << name << " created\n";
}

void cmd_insert(Database &db, Parser &p, std::ostream &out) {
  p.accept_word("into");
  const std::string name = p.expect_name();
  const Schema &schema = db.schema(name);
  p.expect_word("values");
  p.expect_punct('(');
  Row row;
  while (true) {
    if (row.size() >= schema.columns.size()) {
      throw CliError("too many values: table " + name + " has " +
                     std::to_string(schema.columns.size()) + " columns");
    }
    const Column &col = schema.columns[row.size()];
    row.push_back(parse_value(p.next(), col.type, col.name));
    if (p.accept_punct(',')) {
      continue;
    }
    p.expect_punct(')');
    break;
  }
  p.expect_end();
  if (row.size() != schema.columns.size()) {
    throw CliError("expected " + std::to_string(schema.columns.size()) +
                   " values, got " + std::to_string(row.size()));
  }
  db.insert(name, row);
  out << "1 row inserted\n";
}

void cmd_select(Database &db, Parser &p, std::ostream &out) {
  bool star = false;
  std::vector<std::string> columns;
  if (p.accept_punct('*')) {
    star = true;
  } else {
    do {
      columns.push_back(p.expect_name());
    } while (p.accept_punct(','));
  }
  p.expect_word("from");
  const std::string table = p.expect_name();

  if (p.accept_word("join")) {
    if (!star) {
      throw CliError("JOIN supports only SELECT *");
    }
    const std::string right = p.expect_name();
    p.expect_word("on");
    const auto a = split_qualified(p.expect_name());
    const Token eq = p.next();
    if (eq.kind != Kind::Op || eq.text != "=") {
      throw CliError("expected '=' in JOIN condition");
    }
    const auto b = split_qualified(p.expect_name());
    p.expect_end();
    std::string left_col;
    std::string right_col;
    if (a.first == table && b.first == right) {
      left_col = a.second;
      right_col = b.second;
    } else if (a.first == right && b.first == table) {
      left_col = b.second;
      right_col = a.second;
    } else {
      throw CliError("JOIN condition must be <" + table + ">.<column> = <" +
                     right + ">.<column>");
    }
    print_result(out, db.join(table, right, left_col, right_col));
    return;
  }

  const Schema &schema = db.schema(table);
  const auto cond = parse_where(p, schema);
  p.expect_end();
  print_result(out, db.select(table, cond, star ? std::vector<std::string>{} : columns));
}

void cmd_update(Database &db, Parser &p, std::ostream &out) {
  const std::string name = p.expect_name();
  const Schema &schema = db.schema(name);
  p.expect_word("set");
  Changes changes;
  do {
    const std::string column = p.expect_name();
    const Column &col = schema.columns[index_of(schema, column)];
    const Token eq = p.next();
    if (eq.kind != Kind::Op || eq.text != "=") {
      throw CliError("expected '=' after column name");
    }
    changes.emplace_back(column, parse_value(p.next(), col.type, column));
  } while (p.accept_punct(','));
  const auto cond = parse_where(p, schema);
  p.expect_end();
  const std::size_t n = db.update(name, cond, changes);
  out << n << " row(s) updated\n";
}

void cmd_delete(Database &db, Parser &p, std::ostream &out) {
  p.expect_word("from");
  const std::string name = p.expect_name();
  const Schema &schema = db.schema(name);
  const auto cond = parse_where(p, schema);
  p.expect_end();
  const std::size_t n = db.remove(name, cond);
  out << n << " row(s) deleted\n";
}

void cmd_show(Database &db, Parser &p, std::ostream &out) {
  p.expect_word("tables");
  p.expect_end();
  ResultSet rs;
  rs.columns = {"table", "rows"};
  for (const auto &name : db.list_tables()) {
    rs.rows.push_back(Row{name, static_cast<std::int64_t>(db.row_count(name))});
  }
  print_result(out, rs);
}

void cmd_describe(Database &db, Parser &p, std::ostream &out) {
  const std::string name = p.expect_name();
  p.expect_end();
  const Schema &schema = db.schema(name);
  ResultSet rs;
  rs.columns = {"column", "type", "key"};
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    const Column &col = schema.columns[i];
    std::string key;
    if (i == schema.pk) {
      key = "PRIMARY KEY";
    }
    for (const auto &fk : schema.fks) {
      if (fk.column == col.name) {
        key += std::string(key.empty() ? "" : ", ") + "FOREIGN KEY -> " +
               fk.ref_table + "." + fk.ref_column;
      }
    }
    rs.rows.push_back(Row{col.name, std::string(type_name(col.type)), key});
  }
  print_result(out, rs);
}

void cmd_help(std::ostream &out) {
  out << "Commands (keywords are case-insensitive):\n"
         "  create table <name> (<col> <type>, ...) primary key <col>\n"
         "        [foreign key <col> references <table>.<col>] ...\n"
         "        types: int, float, string\n"
         "  insert into <table> values (<v1>, <v2>, ...)\n"
         "  select * | <col>, <col> from <table> [where <col> <op> <value>]\n"
         "  select * from <a> join <b> on <a>.<col> = <b>.<col>\n"
         "  update <table> set <col> = <v> [, <col> = <v>] [where <col> <op> <value>]\n"
         "  delete from <table> [where <col> <op> <value>]\n"
         "  show tables | describe <table> | drop table <table>\n"
         "  compact <table> | backup <dir> | help | exit\n"
         "Operators: = != < <= > >=    Strings with spaces: 'two words'\n"
         "Lines starting with # or -- are comments.\n";
}

} // namespace

bool execute_command(Database &db, const std::string &line, std::ostream &out) {
  Parser p(tokenize(line));
  if (p.at_end()) {
    return true;
  }
  const Token first = p.next();
  const std::string cmd = first.kind == Kind::Word ? lower(first.text) : "";

  if (cmd == "exit" || cmd == "quit") {
    return false;
  }
  if (cmd == "help") {
    cmd_help(out);
  } else if (cmd == "create") {
    cmd_create(db, p, out);
  } else if (cmd == "insert") {
    cmd_insert(db, p, out);
  } else if (cmd == "select") {
    cmd_select(db, p, out);
  } else if (cmd == "update") {
    cmd_update(db, p, out);
  } else if (cmd == "delete") {
    cmd_delete(db, p, out);
  } else if (cmd == "show") {
    cmd_show(db, p, out);
  } else if (cmd == "describe") {
    cmd_describe(db, p, out);
  } else if (cmd == "drop") {
    p.expect_word("table");
    const std::string name = p.expect_name();
    p.expect_end();
    db.drop_table(name);
    out << "table " << name << " dropped\n";
  } else if (cmd == "compact") {
    const std::string name = p.expect_name();
    p.expect_end();
    db.compact(name);
    out << "table " << name << " compacted\n";
  } else if (cmd == "backup") {
    const std::string dest = p.next().text;
    p.expect_end();
    db.backup(dest);
    out << "backup saved to " << dest << '\n';
  } else {
    throw CliError("unknown command: " + first.text + " (type 'help')");
  }
  return true;
}

int run_cli(Database &db, std::istream &in, std::ostream &out, bool prompt,
            bool echo) {
  int errors = 0;
  std::string line;
  while (true) {
    if (prompt) {
      out << "minidb> " << std::flush;
    }
    if (!std::getline(in, line)) {
      if (prompt) {
        out << '\n';
      }
      break;
    }
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    const bool skip =
        line.empty() || line[0] == '#' || line.compare(0, 2, "--") == 0;
    if (skip) {
      if (echo) {
        out << line << '\n'; // комментарии и пустые строки сохраняют читаемость скрипта
      }
      continue;
    }
    if (echo) {
      out << "minidb> " << line << '\n';
    }
    try {
      if (!execute_command(db, line, out)) {
        break;
      }
    } catch (const std::exception &e) {
      out << "error: " << e.what() << '\n';
      ++errors;
    }
  }
  return errors;
}
