//
// Testtool - helpers for reading json configuration
//
// Copyright (c) 2018 InnoGames GmbH
//

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <nlohmann/json.hpp>

bool key_present(const nlohmann::json &j, const std::string &key);
template <class T>
T safe_get(const nlohmann::json &j, const char *key, T defval);

#endif
