#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <cstdint>
#include <stdexcept>
#include <string>

class DatabaseError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class TableExists : public DatabaseError {
public:
  explicit TableExists(const std::string &name)
      : DatabaseError("table already exists: " + name) {}
};

class TableNotFound : public DatabaseError {
public:
  explicit TableNotFound(const std::string &name)
      : DatabaseError("table not found: " + name) {}
};

class InvalidName : public DatabaseError {
public:
  explicit InvalidName(const std::string &name)
      : DatabaseError("invalid name (allowed: letters, digits, '_'): '" +
                      name + "'") {}
};

class CatalogError : public DatabaseError {
public:
  using DatabaseError::DatabaseError;
};

class DuplicateKey : public DatabaseError {
public:
  explicit DuplicateKey(std::int64_t key)
      : DatabaseError("duplicate primary key: " + std::to_string(key)) {}
};

class TypeMismatch : public DatabaseError {
public:
  explicit TypeMismatch(const std::string &what)
      : DatabaseError("type mismatch: " + what) {}
};

// Нарушение ссылочной целостности (внешнего ключа).
class ForeignKeyError : public DatabaseError {
public:
  using DatabaseError::DatabaseError;
};

#endif // EXCEPTIONS_H
