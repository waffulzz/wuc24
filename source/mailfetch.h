// mailfetch.h — talking to the WiiConnect24 mail server.
//
// Mail does not travel through the nwc24dl.bin download list. It has its own
// protocol: HTTP POSTs to CGI endpoints whose URLs live in nwc24msg.cfg, using
// the console's own credentials. Responses are "key=value" lines, one per
// line, with cd=100 meaning success.
//
//   check.cgi    is there mail waiting?           harmless, repeatable
//   receive.cgi  give me the mail                 CONSUMES it server-side
//
// receive.cgi marks messages delivered *before* it sends them, so anything we
// fetch will never reach the console by its own route. Never call it while a
// console-side capture is pending.
#pragma once

#include <string>
#include <vector>

#include "msgcfg.h"

namespace mailfetch {

// A parsed CGI response.
struct CgiResponse {
    int         code = 0;   // cd=  (100 is success)
    std::string message;    // msg=
    std::string Get(const std::string &key) const;

    std::vector<std::pair<std::string, std::string>> values;
};

// Splits a "key=value" per line body.
CgiResponse ParseCgi(const std::string &body);

// Asks whether mail is waiting. Does not consume anything.
struct CheckResult {
    bool        ok       = false;
    int         code     = 0;
    bool        has_mail = false;
    std::string flag;      // mail.flag; all zeroes means nothing waiting
    int         interval = 0;
};
bool Check(const msgcfg::Config &cfg, CheckResult &out);

// Downloads waiting mail. Each element is one complete message in the form the
// console stores. THIS CONSUMES THE MAIL on the server.
bool Receive(const msgcfg::Config &cfg, std::vector<std::string> &out_messages);

}  // namespace mailfetch
