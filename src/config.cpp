#include <sstream>

#include "config.h"
#include "msg.h"

using namespace std;

ConfigLine::ConfigLine(istringstream& line) {
    /* Read all things from the definition line.
       Configuration is specified by 'var=val' strings.
       Each pair is separated by space. There must be no spaces around = character. */
    string confword;

    while (line >> confword) {
        int split = confword.find('=');
        if (split == -1) {
            log_txt(MSG_TYPE_CRITICAL, "Malformed configuration entry: %s", confword.c_str());
            continue;
        }
        string var = confword.substr(0, split);
        this->entries[var] = confword.substr(split+1);
    }
}
