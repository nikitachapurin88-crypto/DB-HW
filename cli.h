#ifndef CLI_H
#define CLI_H

#include <iosfwd>
#include <string>

#include "database.h"

// Ошибка разбора команды (не ошибка движка).
class CliError : public DatabaseError {
public:
  using DatabaseError::DatabaseError;
};

// Выполняет одну команду и печатает результат в `out`.
// Бросает исключение при ошибке разбора или ошибке движка.
// Возвращает false, если команда просит выйти (exit / quit).
bool execute_command(Database &db, const std::string &line, std::ostream &out);

// Читает команды из `in` построчно, пока не кончатся или не встретится exit.
// Ошибки печатает в `out` и продолжает со следующей строки.
//   prompt: печатать приглашение «minidb> » перед чтением строки (интерактив)
//   echo:   печатать каждую команду перед результатом (для скриптов)
// Пустые строки и строки, начинающиеся с # или --, пропускаются.
// Возвращает число команд, завершившихся ошибкой.
int run_cli(Database &db, std::istream &in, std::ostream &out, bool prompt,
            bool echo);

#endif // CLI_H
