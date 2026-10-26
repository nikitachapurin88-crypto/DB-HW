#ifndef DATABASE_H
#define DATABASE_H

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "exceptions.h"
#include "table.h" // Table, Schema, Row, Condition, Changes, ResultSet

class Database {
public:
  explicit Database(const std::filesystem::path &dir);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  // схема
  void create_table(const std::string &name, const Schema &schema);
  void drop_table(const std::string &name);
  bool has_table(const std::string &name) const;
  std::vector<std::string> list_tables() const;
  const Schema &schema(const std::string &name) const;

  // CRUD (метод удаления назван remove: `delete` в C++ ключевое слово)
  void insert(const std::string &name, const Row &row);
  // columns пуст = все столбцы; иначе только перечисленные, в этом порядке
  ResultSet select(const std::string &name,
                   const std::optional<Condition> &cond = std::nullopt,
                   const std::vector<std::string> &columns = {}) const;
  std::size_t row_count(const std::string &name) const;
  std::size_t update(const std::string &name,
                     const std::optional<Condition> &cond,
                     const Changes &changes);
  std::size_t remove(const std::string &name,
                     const std::optional<Condition> &cond);

  // JOIN по равенству значений столбцов
  ResultSet join(const std::string &left, const std::string &right,
                 const std::string &left_col,
                 const std::string &right_col) const;

  // копия всей директории базы; восстановление = Database(dest)
  void backup(const std::filesystem::path &dest) const;

  // физически убрать удалённые записи из файла таблицы
  void compact(const std::string &name);

private:
  Table &get_table(const std::string &name);
  const Table &get_table(const std::string &name) const;
  std::filesystem::path table_file(const std::string &name) const;

  void save_catalog() const;
  void load_catalog();

  // каждый внешний ключ схемы должен ссылаться на PK существующей таблицы
  void validate_foreign_keys(const Schema &schema) const;

  // значение внешнего ключа должно существовать в родительской таблице
  void check_parent_exists(const ForeignKey &fk, const Value &value) const;
  // на строки родителя, которые удаляются, не должны ссылаться дочерние
  void check_not_referenced(const std::string &parent_name,
                            const std::optional<Condition> &cond) const;

  std::filesystem::path path_;
  std::filesystem::path catalog_path_;
  std::unordered_map<std::string, Table> tables_;
};

#endif // DATABASE_H
