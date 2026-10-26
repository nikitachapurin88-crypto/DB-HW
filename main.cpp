// Демо MiniDB: три таблицы, CRUD, JOIN, backup.
// Первый запуск создаёт и наполняет базу, второй и далее показывают,
// что данные сохранились после перезапуска. `--reset` удаляет базу.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

#include "database.h"

namespace fs = std::filesystem;

namespace {

Value I(std::int64_t v) { return v; }
Value F(double v) { return v; }
Value S(const std::string &v) { return v; }

void print_value(const Value &v) {
  std::visit([](const auto &x) { std::cout << x; }, v);
}

void print(const std::string &title, const ResultSet &rs) {
  std::cout << "\n== " << title << " (" << rs.rows.size() << ") ==\n";
  for (const auto &c : rs.columns) {
    std::cout << c << '\t';
  }
  std::cout << '\n';
  for (const auto &row : rs.rows) {
    for (const auto &v : row) {
      print_value(v);
      std::cout << '\t';
    }
    std::cout << '\n';
  }
}

void seed(Database &db) {
  db.create_table("students",
                  Schema{{{"id", ColumnType::Int},
                          {"name", ColumnType::Text},
                          {"age", ColumnType::Int}},
                         0});
  db.create_table("courses",
                  Schema{{{"id", ColumnType::Int},
                          {"title", ColumnType::Text},
                          {"credits", ColumnType::Int}},
                         0});
  db.create_table("enrollments",
                  Schema{{{"id", ColumnType::Int},
                          {"student_id", ColumnType::Int},
                          {"course_id", ColumnType::Int},
                          {"grade", ColumnType::Float}},
                         0});

  db.insert("students", Row{I(1), S("Alice"), I(19)});
  db.insert("students", Row{I(2), S("Bob"), I(21)});
  db.insert("students", Row{I(3), S("Carol"), I(20)});
  db.insert("students", Row{I(4), S("Dave"), I(22)});

  db.insert("courses", Row{I(1), S("Math"), I(5)});
  db.insert("courses", Row{I(2), S("Physics"), I(4)});
  db.insert("courses", Row{I(3), S("Databases"), I(6)});

  db.insert("enrollments", Row{I(1), I(1), I(1), F(8.5)});
  db.insert("enrollments", Row{I(2), I(1), I(3), F(9.0)});
  db.insert("enrollments", Row{I(3), I(2), I(1), F(7.0)});
  db.insert("enrollments", Row{I(4), I(3), I(2), F(6.5)});
  db.insert("enrollments", Row{I(5), I(4), I(3), F(10.0)});

  print("students after INSERT", db.select("students"));

  // SELECT с условием: по PK идёт через индекс, по остальным полный проход
  print("students WHERE id = 3",
        db.select("students", Condition{"id", Op::Eq, I(3)}));
  print("students WHERE age > 20",
        db.select("students", Condition{"age", Op::Gt, I(20)}));

  // UPDATE
  const auto updated = db.update("students", Condition{"id", Op::Eq, I(2)},
                                 Changes{{"age", I(25)}, {"name", S("Robert")}});
  std::cout << "\nUPDATE students SET age=25, name='Robert' WHERE id=2 -> "
            << updated << " row(s)\n";

  // DELETE
  const auto removed = db.remove("enrollments", Condition{"id", Op::Eq, I(4)});
  std::cout << "DELETE FROM enrollments WHERE id=4 -> " << removed
            << " row(s)\n";

  db.backup("minidb_backup");
  std::cout << "Backup saved to ./minidb_backup\n";
}

} // namespace

int main(int argc, char **argv) {
  const fs::path dir = "minidb_data";

  if (argc > 1 && std::string(argv[1]) == "--reset") {
    fs::remove_all(dir);
    fs::remove_all("minidb_backup");
    std::cout << "Database removed.\n";
    return 0;
  }

  try {
    Database db(dir);

    if (!db.has_table("students")) {
      std::cout << "First run: creating tables and data.\n";
      seed(db);
    } else {
      std::cout << "Database reopened: data survived the restart.\n";
    }

    for (const auto &name : db.list_tables()) {
      print(name, db.select(name));
    }
    print("JOIN enrollments.student_id = students.id",
          db.join("enrollments", "students", "student_id", "id"));
    print("JOIN enrollments.course_id = courses.id",
          db.join("enrollments", "courses", "course_id", "id"));
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
