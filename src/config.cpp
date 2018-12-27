//
// Testtool - helpers for reading json configuration
//
// Copyright (c) 2018 InnoGames GmbH
//
#include <nlohmann/json.hpp>

bool key_present(const nlohmann::json &j, const std::string &key) {
  return j.find(key) != j.end();
}

template <class T> T safe_get(const nlohmann::json &j, const char *key, T val) {
  auto test = j.find(key);

  if (test == j.end() || test->is_null()) {
    val = T{};
  } else {
    try {
      val = j.value(key, T{});
    } catch (nlohmann::detail::type_error) {
      return val;
    }
  }
  return val;
}

template bool safe_get(const nlohmann::json &, const char *, bool);
template int safe_get(const nlohmann::json &, const char *, int);
template std::string safe_get(const nlohmann::json &, const char *,
                              std::string);
template std::vector<std::string> safe_get(const nlohmann::json &, const char *,
                                           std::vector<std::string>);