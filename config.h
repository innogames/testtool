#ifndef _CONFIG_H
#define _CONFIG_H

#include <iostream>
#include <map>
#include <string>
#include <sstream>

/*
   Reads a config line consisting of key=value pairs ("a=b foo=bar")
*/
class ConfigLine {
    public:
        ConfigLine(std::istringstream& line);

        template<typename T, typename DT>
        bool load(std::string key, T& dst, DT default_value) {
            auto it = entries.find(key);
            if (it != entries.end()) {
                //dst = it->second;
                std::stringstream(it->second) >> dst;
                return true;
            } else {
                dst = T(default_value);
                return false;
            }
        }
    private:
        std::map<std::string, std::string> entries;
};

#endif

