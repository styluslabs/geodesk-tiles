#pragma once
#include "sqlite3.h"
#include <string>
#include <tuple>
#include <functional>
#include <utility>

#ifndef SQLITEPP_LOGE
#include <stdio.h>
#define SQLITEPP_LOG(msg, ...) do { fprintf(stderr, msg, ## __VA_ARGS__); } while (0)
#define SQLITEPP_LOGW SQLITEPP_LOG
#define SQLITEPP_LOGE SQLITEPP_LOG
#warning "SQLITEPP_LOGE not defined, using default."
#endif

template<class F, class Tuple, size_t... Is>
constexpr auto apply_impl(Tuple t, F f, std::index_sequence<Is...>)
{
  return f(std::get<Is>(t)...);
}

template<class F, class Tuple>
constexpr auto apply_tuple(F f, Tuple t)
{
  return apply_impl(t, f, std::make_index_sequence<std::tuple_size<Tuple> {}> {});
}

// from https://github.com/SqliteModernCpp/sqlite_modern_cpp - extract signature from all possible fn types
template<typename> struct func_traits;

template <typename F>
struct func_traits : public func_traits< decltype(&std::remove_reference<F>::type::operator()) > {};

template <typename Cls, typename Ret, typename... Args>
struct func_traits<Ret(Cls::*)(Args...) const> : func_traits<Ret(*)(Args...)> {};

template <typename Cls, typename Ret, typename... Args>
struct func_traits<Ret(Cls::*)(Args...)> : func_traits<Ret(*)(Args...)> {};  // non-const

template <typename Ret, typename... Args>
struct func_traits<Ret(*)(Args...)> {
  typedef Ret result_type;
  using argument_tuple = std::tuple<Args...>;
  template <std::size_t Index>
  using argument = typename std::tuple_element<Index,	argument_tuple>::type;
  static const std::size_t arity = sizeof...(Args);
};

// of course there are many sqlite C++ wrappers and ORMs, but many (most?) don't even provide variadic bind()
//  and only https://github.com/SqliteModernCpp/sqlite_modern_cpp seems to support extracting column types
//  from callback fn signature
class SQLiteStmt
{
public:
  sqlite3_stmt* stmt = NULL;
  SQLiteStmt(sqlite3_stmt* _stmt) : stmt(_stmt) {}
  SQLiteStmt(const SQLiteStmt&) = delete;
  SQLiteStmt(SQLiteStmt&& other) : stmt(std::exchange(other.stmt, nullptr)) {}
  SQLiteStmt& operator=(SQLiteStmt&& other) { std::swap(stmt, other.stmt); return *this; }
  ~SQLiteStmt() { if(stmt) sqlite3_finalize(stmt); }

  SQLiteStmt(sqlite3* db, const char* sql) {
    // sqlite3_prepare_v2 only compiles a single statement!
    const char* leftover = NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &stmt, &leftover) != SQLITE_OK)
      SQLITEPP_LOGE("sqlite3_prepare_v2 error: %s in %s", sqlite3_errmsg(db), sql);
    if(leftover && leftover[0])
      SQLITEPP_LOGW("Remainder of SQL will be ignored: %s", leftover);
  }

  SQLiteStmt(sqlite3* db, const std::string& sql) : SQLiteStmt(db, sql.c_str()) {}

  void bind_at(int loc) {}
  template<class... Args> void bind_at(int loc, bool arg0, Args... args)
  { sqlite3_bind_int(stmt, loc, arg0 ? 1 : 0); bind_at(loc+1, args...); }
  template<class... Args> void bind_at(int loc, int arg0, Args... args)
  { sqlite3_bind_int(stmt, loc, arg0); bind_at(loc+1, args...); }
  template<class... Args> void bind_at(int loc, int64_t arg0, Args... args)
  { sqlite3_bind_int64(stmt, loc, arg0); bind_at(loc+1, args...); }
  template<class... Args> void bind_at(int loc, double arg0, Args... args)
  { sqlite3_bind_double(stmt, loc, arg0); bind_at(loc+1, args...); }
  template<class... Args> void bind_at(int loc, const char* arg0, Args... args)
  { sqlite3_bind_text(stmt, loc, arg0, -1, SQLITE_TRANSIENT); bind_at(loc+1, args...); }  //SQLITE_STATIC
  template<class... Args> void bind_at(int loc, const std::string& arg0, Args... args)
  { sqlite3_bind_text(stmt, loc, arg0.c_str(), int(arg0.size()), SQLITE_TRANSIENT); bind_at(loc+1, args...); }

  template<class... Args> SQLiteStmt& bind(Args... args) { if(stmt) bind_at(1, args...); return *this; }

  template<class T> T get_col(int idx);

  template<class T> std::tuple<T> columns(int idx) { return std::make_tuple(get_col<T>(idx)); }

  template<class T1, class T2, class... Args>
  std::tuple<T1, T2, Args...> columns(int idx) {
    return std::tuple_cat(columns<T1>(idx), columns<T2, Args...>(idx+1));  //sizeof...(Args)
  }

  // have to use struct to extract parameter pack
  template <typename> struct _columns;
  template <typename... Args>
  struct _columns< std::tuple<Args...> > {
    static std::tuple<Args...> get(SQLiteStmt& inst) { return inst.columns<Args...>(0); }
  };

  template<class F>
  bool exec(F&& cb, bool single_step = false, bool* abort = nullptr) {
    if(!stmt) { SQLITEPP_LOGE("Attempting to exec null statement!"); return false; }
#ifdef SQLITEPP_LOGTIME
    auto t0 = std::chrono::high_resolution_clock::now();
#endif
    int res;
    if(single_step)
      res = sqlite3_step(stmt);
    else {
      while((res = sqlite3_step(stmt)) == SQLITE_ROW) {
        apply_tuple(cb, _columns<typename func_traits<F>::argument_tuple>::get(*this));
        if (abort && *abort) { res = SQLITE_OK; break; }
      }
    }
    bool ok = res == SQLITE_DONE || res == SQLITE_OK;
    if(!ok)
      SQLITEPP_LOGE("sqlite3_step error for %s: %s", sqlite3_sql(stmt), sqlite3_errmsg(sqlite3_db_handle(stmt)));
#ifdef SQLITEPP_LOGTIME
    auto t1 = std::chrono::high_resolution_clock::now();
    LOG("Query time: %.6f s for %s\n", std::chrono::duration<float>(t1 - t0).count(), sqlite3_sql(stmt));
#endif
    sqlite3_reset(stmt);
    return ok;
  }

  bool exec() { return exec([](sqlite3_stmt*){}, true); }

  template<class... Args>
  bool onerow(Args&... args) {
    if(!stmt) { SQLITEPP_LOGE("Attempting to exec null statement!"); return false; }
    int res = sqlite3_step(stmt);
    // no rows - not an error, but return false to inform caller no data was written
    if(res == SQLITE_DONE || res == SQLITE_OK) {
      sqlite3_reset(stmt);
      return false;
    }
    if(res == SQLITE_ROW) {
      std::tie(args...) = columns<Args...>(0);
      res = sqlite3_step(stmt);
      if(res == SQLITE_ROW)
        SQLITEPP_LOGW("sqlite3_step returned multiple rows for %s", sqlite3_sql(stmt));
    }
    bool ok = res == SQLITE_ROW || res == SQLITE_DONE || res == SQLITE_OK;
    if(!ok)
      SQLITEPP_LOGE("sqlite3_step error for %s: %s", sqlite3_sql(stmt), sqlite3_errmsg(sqlite3_db_handle(stmt)));
    sqlite3_reset(stmt);
    return ok;
  }
};

#ifndef NDEBUG
#define CHK_COL(idx, type) \
  do { \
    int ctype = sqlite3_column_type(stmt, idx); \
    if(ctype != type && ctype != SQLITE_NULL) \
      SQLITEPP_LOGW("Requested data type does not match type of column %d in %s", idx, sqlite3_sql(stmt)); \
  } while(0)
#else
#define CHK_COL(idx, type) do { } while(0)
#endif

// can't be inside class due to GCC bug
template<> inline int SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_INTEGER); return sqlite3_column_int(stmt, idx); }
template<> inline int64_t SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_INTEGER); return sqlite3_column_int64(stmt, idx); }
template<> inline float SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_FLOAT); return float(sqlite3_column_double(stmt, idx)); }
template<> inline double SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_FLOAT); return sqlite3_column_double(stmt, idx); }
template<> inline const char* SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_TEXT); return (const char*)sqlite3_column_text(stmt, idx); }
template<> inline std::string SQLiteStmt::get_col(int idx)
  { CHK_COL(idx, SQLITE_TEXT); const char* res = (const char*)sqlite3_column_text(stmt, idx);  return res ? res : ""; }
template<> inline sqlite3_stmt* SQLiteStmt::get_col(int idx) { return stmt; }

#undef CHK_COL

class SQLiteDB
{
public:
  sqlite3* db = NULL;

  SQLiteDB(const std::string& file, int mode, const char* vfs = NULL) {
    if(open(file, mode, vfs) != SQLITE_OK)
      SQLITEPP_LOGW("sqlite3_open_v2 failed for %s", file.c_str());
  }
  SQLiteDB(sqlite3* _db = NULL) : db(_db) {}
  SQLiteDB(const SQLiteDB&) = delete;
  SQLiteDB(SQLiteDB&& other) : db(std::exchange(other.db, nullptr)) {}
  ~SQLiteDB() {
    if(!db) return;
#ifndef NDEBUG  // sqlite FTS, e.g., leaves around some unfinalized statements that we can't do anything about
    sqlite3_stmt* stmt = NULL;
    while((stmt = sqlite3_next_stmt(db, stmt)))
      SQLITEPP_LOGW("SQLite statement was not finalized: %s", sqlite3_sql(stmt));
#endif
    sqlite3_close(db);
  }

  int open(const std::string& file, int mode, const char* vfs = NULL)
    { return sqlite3_open_v2(file.c_str(), &db, mode, vfs); }
  sqlite3* release() { return std::exchange(db, nullptr); }

  const char* errMsg() { return sqlite3_errmsg(db); }
  int totalChanges() { return sqlite3_total_changes(db); }
  int64_t lastInsertRowId() { return sqlite3_last_insert_rowid(db); }
  bool exec(const char* sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK; }
  bool exec(const std::string& sql) { return exec(sql.c_str()); }
  SQLiteStmt stmt(const char* sql) { return SQLiteStmt(db, sql); }
  SQLiteStmt stmt(const std::string& sql) { return stmt(sql.c_str()); }

  //std::unordered_map<std::string, sqlite3_stmt*> stmtCache;
};
