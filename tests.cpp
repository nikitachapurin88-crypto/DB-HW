#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "database.h"

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_THROWS(Exc, ...)                                                 \
  do {                                                                         \
    bool thrown_ = false;                                                      \
    try {                                                                      \
      __VA_ARGS__;                                                             \
    } catch (const Exc &) {                                                    \
      thrown_ = true;                                                          \
    } catch (...) {                                                            \
    }                                                                          \
    if (!thrown_) {                                                            \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                      \
                << ": expected " #Exc "\n";                                    \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static Value I(std::int64_t v) { return v; }
static Value F(double v) { return v; }
static Value S(const std::string &v) { return v; }

static Schema datasets_schema() {
  return Schema{{{"id", ColumnType::Int},
                 {"name", ColumnType::String},
                 {"task", ColumnType::String},
                 {"size", ColumnType::Int}},
                0};
}
static Schema models_schema() {
  return Schema{{{"id", ColumnType::Int},
                 {"name", ColumnType::String},
                 {"version", ColumnType::String}},
                0};
}
static Schema experiments_schema() {
  return Schema{{{"id", ColumnType::Int},
                 {"dataset_id", ColumnType::Int},
                 {"model_id", ColumnType::Int},
                 {"accuracy", ColumnType::Float},
                 {"status", ColumnType::String}},
                0,
                {{"dataset_id", "datasets", "id"}, {"model_id", "models", "id"}}};
}

static Condition eq(const std::string &col, std::int64_t v) {
  return Condition{col, Op::Eq, I(v)};
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "minidb_test_data";
  const fs::path dir_backup = fs::temp_directory_path() / "minidb_test_backup";
  fs::remove_all(dir);
  fs::remove_all(dir_backup);

  {
    Database db(dir);

    // --- схема ---
    db.create_table("datasets", datasets_schema());
    db.create_table("models", models_schema());
    db.create_table("experiments", experiments_schema());
    CHECK(db.list_tables().size() == 3);
    CHECK(db.has_table("models"));
    CHECK_THROWS(TableExists, db.create_table("datasets", datasets_schema()));
    CHECK_THROWS(InvalidName, db.create_table("../x", datasets_schema()));
    CHECK_THROWS(DatabaseError,
                 db.create_table("bad_pk", Schema{{{"id", ColumnType::String}}, 0}));
    CHECK(!db.has_table("bad_pk"));
    CHECK_THROWS(TableNotFound, db.select("nope"));

    // --- внешние ключи: объявление ---
    const auto fks = db.schema("experiments").fks;
    CHECK(fks.size() == 2);
    CHECK(fks[0].column == "dataset_id" && fks[0].ref_table == "datasets" &&
          fks[0].ref_column == "id");
    CHECK(fks[1].column == "model_id" && fks[1].ref_table == "models" &&
          fks[1].ref_column == "id");
    CHECK(db.schema("datasets").fks.empty());
    // ссылка на несуществующую таблицу
    CHECK_THROWS(DatabaseError,
                 db.create_table("bad_fk1",
                                 Schema{{{"id", ColumnType::Int}, {"x", ColumnType::Int}},
                                        0, {{"x", "ghost", "id"}}}));
    // ссылка не на первичный ключ
    CHECK_THROWS(DatabaseError,
                 db.create_table("bad_fk2",
                                 Schema{{{"id", ColumnType::Int}, {"x", ColumnType::Int}},
                                        0, {{"x", "datasets", "size"}}}));
    // столбец внешнего ключа не int
    CHECK_THROWS(DatabaseError,
                 db.create_table("bad_fk3",
                                 Schema{{{"id", ColumnType::Int}, {"x", ColumnType::String}},
                                        0, {{"x", "datasets", "id"}}}));
    // столбца внешнего ключа нет в таблице
    CHECK_THROWS(DatabaseError,
                 db.create_table("bad_fk4",
                                 Schema{{{"id", ColumnType::Int}},
                                        0, {{"nope", "datasets", "id"}}}));
    CHECK(!db.has_table("bad_fk1") && !db.has_table("bad_fk2") &&
          !db.has_table("bad_fk3") && !db.has_table("bad_fk4"));
    CHECK(!fs::exists(dir / "bad_fk1.dat"));
    CHECK(db.list_tables().size() == 3);

    // --- INSERT / SELECT ---
    db.insert("datasets", Row{I(1), S("MNIST"), S("classification"), I(190)});
    db.insert("datasets", Row{I(2), S("CIFAR"), S("classification"), I(210)});
    db.insert("datasets", Row{I(3), S("IMDB"), S("sentiment"), I(200)});
    db.insert("datasets", Row{I(4), S("Iris"), S("classification"), I(220)});
    db.insert("datasets", Row{I(5), S("Wine"), S("classification"), I(180)});
    CHECK(db.select("datasets").rows.size() == 5);

    auto by_pk = db.select("datasets", eq("id", 3));
    CHECK(by_pk.rows.size() == 1);
    CHECK(std::get<std::string>(by_pk.rows[0][1]) == "IMDB");
    CHECK(db.select("datasets", eq("id", 99)).rows.empty());
    CHECK(db.select("datasets", Condition{"size", Op::Gt, I(200)}).rows.size() == 2);
    CHECK(db.select("datasets", Condition{"size", Op::Ge, I(200)}).rows.size() == 3);
    CHECK(db.select("datasets", Condition{"size", Op::Ne, I(200)}).rows.size() == 4);
    CHECK(db.select("datasets", Condition{"name", Op::Eq, S("Iris")}).rows.size() == 1);

    // --- ошибки ввода ---
    CHECK_THROWS(DuplicateKey,
                 db.insert("datasets", Row{I(1), S("X"), S("t"), I(1)}));
    CHECK_THROWS(TypeMismatch,
                 db.insert("datasets", Row{I(9), I(5), S("t"), I(1)}));
    CHECK_THROWS(DatabaseError, db.insert("datasets", Row{I(9), S("X")}));
    CHECK_THROWS(DatabaseError,
                 db.insert("datasets",
                           Row{I(9), S(std::string(65, 'x')), S("t"), I(1)}));
    CHECK_THROWS(TypeMismatch,
                 db.select("datasets", Condition{"name", Op::Eq, I(1)}));
    CHECK_THROWS(DatabaseError,
                 db.select("datasets", Condition{"nope", Op::Eq, I(1)}));
    CHECK(db.select("datasets").rows.size() == 5); // ничего не сломалось

    // --- UPDATE ---
    CHECK(db.update("datasets", eq("id", 2), Changes{{"size", I(250)}}) == 1);
    CHECK(std::get<std::int64_t>(db.select("datasets", eq("id", 2)).rows[0][3]) == 250);
    CHECK_THROWS(DatabaseError,
                 db.update("datasets", eq("id", 2), Changes{{"id", I(7)}}));
    CHECK_THROWS(TypeMismatch,
                 db.update("datasets", eq("id", 2), Changes{{"size", S("x")}}));
    CHECK(db.update("datasets", Condition{"size", Op::Lt, I(200)},
                    Changes{{"task", S("tiny")}}) == 2);

    // --- DELETE ---
    CHECK(db.remove("datasets", eq("id", 3)) == 1);
    CHECK(db.select("datasets", eq("id", 3)).rows.empty());
    CHECK(db.select("datasets").rows.size() == 4);
    CHECK(db.remove("datasets", eq("id", 3)) == 0);
    db.insert("datasets", Row{I(3), S("IMDB2"), S("sentiment"), I(230)}); // PK снова свободен
    CHECK(db.select("datasets").rows.size() == 5);

    // --- float принимает целое ---
    db.insert("models", Row{I(1), S("LogReg"), S("1.0")});
    db.insert("models", Row{I(2), S("RandomForest"), S("1.2")});
    db.insert("experiments", Row{I(1), I(1), I(1), F(0.91), S("completed")});
    db.insert("experiments", Row{I(2), I(1), I(2), I(1), S("completed")});
    db.insert("experiments", Row{I(3), I(2), I(1), F(0.76), S("running")});
    db.insert("experiments", Row{I(4), I(4), I(2), F(0.5), S("failed")});
    CHECK(std::get<double>(db.select("experiments", eq("id", 2)).rows[0][3]) == 1.0);
    CHECK(db.select("experiments",
                    Condition{"accuracy", Op::Gt, F(0.8)}).rows.size() == 2);
    CHECK(db.select("experiments",
                    Condition{"status", Op::Eq, S("running")}).rows.size() == 1);

    // --- выбор столбцов ---
    auto proj = db.select("experiments", eq("id", 1), {"status", "id"});
    CHECK(proj.columns.size() == 2 && proj.columns[0] == "status");
    CHECK(std::get<std::string>(proj.rows[0][0]) == "completed");
    CHECK(std::get<std::int64_t>(proj.rows[0][1]) == 1);
    CHECK_THROWS(DatabaseError, db.select("experiments", std::nullopt, {"nope"}));

    // --- внешние ключи ---
    CHECK_THROWS(ForeignKeyError,
                 db.insert("experiments", Row{I(50), I(99), I(1), F(0.5), S("failed")}));
    CHECK_THROWS(ForeignKeyError,
                 db.insert("experiments", Row{I(50), I(1), I(99), F(0.5), S("failed")}));
    CHECK(db.select("experiments", eq("id", 50)).rows.empty());
    CHECK_THROWS(ForeignKeyError,
                 db.update("experiments", eq("id", 1), Changes{{"dataset_id", I(99)}}));
    CHECK(db.update("experiments", eq("id", 1), Changes{{"dataset_id", I(2)}}) == 1);
    CHECK(db.update("experiments", eq("id", 1), Changes{{"dataset_id", I(1)}}) == 1);
    // родителя, на которого есть ссылки, удалять нельзя
    CHECK_THROWS(ForeignKeyError, db.remove("datasets", eq("id", 1)));
    CHECK_THROWS(ForeignKeyError, db.remove("models", std::nullopt));
    CHECK(db.select("datasets", eq("id", 1)).rows.size() == 1);
    CHECK(db.row_count("datasets") == 5);
  }

  // --- перезапуск: новый объект Database над той же директорией ---
  {
    Database db(dir);
    CHECK(db.list_tables().size() == 3);
    // внешние ключи пережили перезапуск (записаны в каталог и прочитаны обратно)
    CHECK(db.schema("experiments").fks.size() == 2);
    CHECK(db.schema("experiments").fks[1].ref_table == "models");
    CHECK(db.schema("datasets").fks.empty());
    {
      std::ifstream cat(dir / "catalog.txt");
      const std::string text((std::istreambuf_iterator<char>(cat)),
                             std::istreambuf_iterator<char>());
      CHECK(text.find("fk=dataset_id>datasets.id") != std::string::npos);
      CHECK(text.find("fk=model_id>models.id") != std::string::npos);
    }
    CHECK(db.select("datasets").rows.size() == 5);
    CHECK(std::get<std::string>(db.select("datasets", eq("id", 3)).rows[0][1]) == "IMDB2");
    CHECK(std::get<std::int64_t>(db.select("datasets", eq("id", 2)).rows[0][3]) == 250);
    CHECK(db.select("experiments").rows.size() == 4);
    CHECK_THROWS(DuplicateKey,
                 db.insert("datasets", Row{I(5), S("X"), S("t"), I(1)}));

    // --- JOIN ---
    auto j = db.join("experiments", "datasets", "dataset_id", "id");
    CHECK(j.rows.size() == 4);
    CHECK(j.columns.size() == 9);
    CHECK(j.columns[0] == "experiments.id");
    CHECK(j.columns[5] == "datasets.id");
    CHECK(std::get<std::string>(j.rows[0][7]) == "tiny"); // датасет 1 получил task = tiny
    CHECK_THROWS(DatabaseError,
                 db.join("experiments", "datasets", "nope", "id"));

    // --- backup ---
    db.backup(dir_backup);
    CHECK_THROWS(DatabaseError, db.backup(dir / "inside"));
    Database copy(dir_backup);
    CHECK(copy.select("datasets").rows.size() == 5);
    CHECK(copy.select("experiments").rows.size() == 4);
    db.insert("datasets", Row{I(6), S("Frank"), S("t"), I(30)});
    CHECK(copy.select("datasets").rows.size() == 5); // копия независима

    // --- compact ---
    // слотов: 5 + 1 (IMDB2) + 1 (Frank) = 7; запись = 1 + 8 + 64 + 64 + 8 = 145 байт
    CHECK(fs::file_size(dir / "datasets.dat") == 7 * 145);
    CHECK(db.remove("datasets", eq("id", 5)) == 1);
    CHECK(db.remove("datasets", eq("id", 6)) == 1);
    CHECK(fs::file_size(dir / "datasets.dat") == 7 * 145); // tombstone, файл не уменьшился
    db.compact("datasets");
    CHECK(fs::file_size(dir / "datasets.dat") == 4 * 145);
    CHECK(db.select("datasets").rows.size() == 4);
    CHECK(std::get<std::string>(db.select("datasets", eq("id", 4)).rows[0][1]) == "Iris");
    db.insert("datasets", Row{I(5), S("Wine"), S("classification"), I(180)});
    db.insert("datasets", Row{I(10), S("Zed"), S("t"), I(40)});
    CHECK(db.select("datasets", eq("id", 10)).rows.size() == 1);

    // --- drop: таблицу, на которую ссылаются, удалить нельзя ---
    CHECK_THROWS(DatabaseError, db.drop_table("models"));
    CHECK_THROWS(DatabaseError, db.drop_table("datasets"));
    CHECK(db.has_table("models"));
    CHECK(fs::exists(dir / "models.dat"));
    db.drop_table("experiments"); // сначала дочерняя, потом родительская
    db.drop_table("models");
    CHECK(!db.has_table("models"));
    CHECK(!fs::exists(dir / "models.dat"));
    CHECK_THROWS(TableNotFound, db.drop_table("models"));
  }

  {
    Database db(dir);
    CHECK(!db.has_table("models"));
    CHECK(!db.has_table("experiments"));
    CHECK(db.has_table("datasets"));
    CHECK(db.select("datasets").rows.size() == 6);
  }

  fs::remove_all(dir);
  fs::remove_all(dir_backup);

  if (g_failures == 0) {
    std::cout << "All tests passed.\n";
    return 0;
  }
  std::cerr << g_failures << " check(s) failed.\n";
  return 1;
}
