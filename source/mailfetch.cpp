#include "mailfetch.h"

#include <cstdio>
#include <cstring>

#include "log.h"
#include "net.h"

namespace mailfetch {

namespace {

// Percent-encodes anything that is not safe in a form body. Credentials can
// contain arbitrary bytes, so this is not optional.
std::string UrlEncode(const std::string &in) {
    static const char *kHex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// The console's address as the server wants it: "w" then the id, 16 digits.
std::string MailId(const msgcfg::Config &cfg) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "w%016llu", static_cast<unsigned long long>(cfg.nwc24_id));
    return buf;
}

}  // namespace

std::string CgiResponse::Get(const std::string &key) const {
    for (const auto &kv : values) {
        if (kv.first == key) return kv.second;
    }
    return {};
}

CgiResponse ParseCgi(const std::string &body) {
    CgiResponse out;
    size_t      pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();

        std::string line = body.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        pos = eol + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key   = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        out.values.emplace_back(key, value);

        if (key == "cd") out.code = std::atoi(value.c_str());
        else if (key == "msg") out.message = value;
    }
    return out;
}

bool Check(const msgcfg::Config &cfg, CheckResult &out) {
    out = CheckResult{};

    const std::string url = cfg.urls[msgcfg::URL_CHECK];
    if (url.empty()) {
        LOG("mailcheck: no check URL in the config");
        return false;
    }
    if (cfg.mlchkid.empty()) {
        LOG("mailcheck: this console has no mlchkid -- is mail registered?");
        return false;
    }

    // The challenge is echoed back inside an HMAC the console uses to verify
    // the server. We do not verify it, so any fixed string does.
    const std::string form =
        "mlchkid=" + UrlEncode(cfg.mlchkid) + "&chlng=" + UrlEncode("wuc24");

    std::vector<uint8_t> body;
    int                  status = 0;
    if (!net::HttpPostForm(url, form, body, status)) return false;
    if (status != 200) {
        LOG("mailcheck: HTTP %d", status);
        return false;
    }

    const CgiResponse cgi = ParseCgi(std::string(body.begin(), body.end()));
    out.ok       = true;
    out.code     = cgi.code;
    out.flag     = cgi.Get("mail.flag");
    out.interval = std::atoi(cgi.Get("interval").c_str());

    // An all-zero flag means nothing is waiting; anything else is new mail.
    out.has_mail = !out.flag.empty() &&
                   out.flag.find_first_not_of('0') != std::string::npos;

    LOG("mailcheck: cd=%d (%s) interval=%d", cgi.code, cgi.message.c_str(), out.interval);
    LOG("mailcheck: %s", out.has_mail ? "MAIL IS WAITING" : "no mail waiting");
    return cgi.code == 100;
}

bool Receive(const msgcfg::Config &cfg, std::vector<std::string> &out_messages) {
    out_messages.clear();

    const std::string url = cfg.urls[msgcfg::URL_RECEIVE];
    if (url.empty()) {
        LOG("mailfetch: no receive URL in the config");
        return false;
    }
    if (cfg.password.empty()) {
        LOG("mailfetch: this console has no mail password");
        return false;
    }

    // maxsize caps what the server puts in one response; it stops adding
    // messages once the total would exceed it.
    constexpr int kMaxSize = 256 * 1024;

    char form[512];
    std::snprintf(form, sizeof(form), "mlid=%s&passwd=%s&maxsize=%d",
                  UrlEncode(MailId(cfg)).c_str(), UrlEncode(cfg.password).c_str(), kMaxSize);

    std::vector<uint8_t> body;
    int                  status = 0;
    if (!net::HttpPostForm(url, form, body, status)) return false;
    if (status != 200) {
        LOG("mailfetch: HTTP %d", status);
        return false;
    }

    const std::string text(body.begin(), body.end());

    // The reply is multipart. Its first line is the boundary delimiter, the
    // first part is filler plus the CGI status, and every part after that is
    // one message.
    size_t first_eol = text.find("\r\n");
    if (first_eol == std::string::npos || text.compare(0, 2, "--") != 0) {
        LOG("mailfetch: response is not the expected multipart form");
        return false;
    }
    const std::string delim = text.substr(0, first_eol);

    std::vector<std::string> parts;
    size_t                   pos = 0;
    while (pos < text.size()) {
        const size_t start = text.find(delim, pos);
        if (start == std::string::npos) break;
        const size_t after = start + delim.size();
        if (text.compare(after, 2, "--") == 0) break;  // closing delimiter

        const size_t next = text.find(delim, after);
        const size_t end  = (next == std::string::npos) ? text.size() : next;
        parts.push_back(text.substr(after, end - after));
        pos = end;
    }

    if (parts.empty()) {
        LOG("mailfetch: no parts in the response");
        return false;
    }

    // Part 0 carries the CGI status.
    const CgiResponse cgi = ParseCgi(parts[0]);
    LOG("mailfetch: cd=%d (%s) mailnum=%s mailsize=%s", cgi.code, cgi.message.c_str(),
        cgi.Get("mailnum").c_str(), cgi.Get("mailsize").c_str());
    if (cgi.code != 100) return false;

    // Each later part is one message, behind its own little header block.
    for (size_t i = 1; i < parts.size(); i++) {
        const size_t blank = parts[i].find("\r\n\r\n");
        if (blank == std::string::npos) continue;

        std::string message = parts[i].substr(blank + 4);
        // Trim the CRLF that belongs to the following delimiter.
        while (message.size() >= 2 && message.compare(message.size() - 2, 2, "\r\n") == 0 &&
               message.size() > 2 && message[message.size() - 3] == '\n') {
            message.resize(message.size() - 2);
        }
        if (message.empty()) continue;

        LOG("mailfetch: message %zu is %zu bytes", out_messages.size() + 1, message.size());
        out_messages.push_back(std::move(message));
    }

    LOG("mailfetch: %zu message(s) downloaded", out_messages.size());
    return true;
}

}  // namespace mailfetch
