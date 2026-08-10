// mailmsg.cpp — building the stored form of a WC24 message.
//
// Kept free of NAND and VFF dependencies so it can be exercised on a host
// against the field offsets a real console produced (tools/mail_test.cpp).
#include "mail.h"

#include <cstdio>
#include <cstring>

namespace mail {

namespace {

// Appends `text` to `out` and reports where it landed.
void Append(std::string &out, const std::string &text, uint32_t *offset = nullptr,
            uint32_t *length = nullptr) {
    if (offset) *offset = static_cast<uint32_t>(out.size());
    out += text;
    if (length) *length = static_cast<uint32_t>(text.size());
}

// Minimal base64, for X-Wii-AltName (a UTF-16BE display name).
std::string Base64(const std::string &in) {
    static const char *kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t      i = 0;
    while (i + 2 < in.size()) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace

// Renders "minutes since 1900" -- the unit the index entry stores -- as the
// RFC822 date the message board shows. Plain civil-calendar arithmetic, since
// there is no timezone handling to get wrong: everything is UTC.
std::string FormatDate(uint32_t minutes_since_1900) {
    static const char *kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    uint32_t days    = minutes_since_1900 / (24 * 60);
    uint32_t minutes = minutes_since_1900 % (24 * 60);

    int year = 1900;
    for (;;) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        const uint32_t in_year = leap ? 366 : 365;
        if (days < in_year) break;
        days -= in_year;
        year++;
    }

    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int month_days[12] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 0;
    while (month < 12 && days >= static_cast<uint32_t>(month_days[month])) {
        days -= month_days[month];
        month++;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02u %s %d %02u:%02u:00 -0000", days + 1, kMonth[month],
                  year, minutes / 60, minutes % 60);
    return buf;
}

namespace {

// ASCII to UTF-16BE, which is how the Wii carries display names.
std::string ToUtf16Be(const std::string &in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (char c : in) {
        out += '\0';
        out += c;
    }
    return out;
}

}  // namespace

std::string BuildMessage(const Message &msg, uint32_t id, uint64_t friend_code,
                         uint32_t minutes_since_1900, FieldSpans &spans) {
    (void)id;
    (void)friend_code;
    (void)minutes_since_1900;

    std::string out;
    spans = FieldSpans{};

    // Modelled byte-for-byte on a message the console actually received and
    // accepted. Three things about that were not guessable:
    //
    //   * there is NO Date: header -- the entry's minutes_since_1900 is 0 too
    //   * the body is not a flat text/plain part; it is multipart/mixed
    //   * the text part carries "Content-Description: wiimail", which is what
    //     marks it as the message-board body
    //
    // A boundary only has to be unique within the message.
    char boundary[64];
    std::snprintf(boundary, sizeof(boundary), "wuc24%08X/%u", minutes_since_1900, id);

    Append(out, "From: ");
    Append(out, msg.from, &spans.from_offset, &spans.from_length);
    Append(out, "\r\n");

    Append(out, "To: ");
    Append(out, msg.to, &spans.to_offset, &spans.to_length);
    Append(out, "\r\n");

    Append(out, "Subject: ");
    Append(out, msg.subject, &spans.subject_offset, &spans.subject_length);
    Append(out, "\r\n");

    Append(out, "MIME-Version: 1.0\r\n");

    if (!msg.alt_name.empty()) {
        Append(out, "X-Wii-AltName: " + Base64(ToUtf16Be(msg.alt_name)) + "\r\n");
    }

    Append(out, std::string("Content-Type: multipart/mixed; boundary=\"") + boundary + "\"\r\n");
    Append(out, "\r\n");

    // header_len points at the first boundary line, i.e. where the outer
    // headers stop and the MIME body starts.
    spans.mime_offset = static_cast<uint32_t>(out.size());
    Append(out, std::string("--") + boundary + "\r\n");

    // The charset span is the value alone -- no trailing CR, unlike the
    // outbound form -- and there is no Content-Transfer-Encoding at all.
    Append(out, "Content-Type: text/plain; charset=");
    Append(out, "utf-8", &spans.charset_offset, &spans.charset_length);
    Append(out, "\r\n");
    Append(out, "Content-Description: wiimail\r\n");
    Append(out, "\r\n");

    // The body span runs from here to just before the closing boundary, so it
    // takes in the blank lines that follow the text.
    spans.body_offset = static_cast<uint32_t>(out.size());

    std::string text = msg.body;
    // Normalise to CRLF, which is what the console is given.
    std::string normalised;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) normalised += '\r';
        normalised += text[i];
    }
    if (normalised.size() < 2 || normalised.compare(normalised.size() - 2, 2, "\r\n") != 0) {
        normalised += "\r\n";
    }
    normalised += "\r\n\r\n\r\n";

    Append(out, normalised);
    spans.body_length = static_cast<uint32_t>(out.size()) - spans.body_offset;

    Append(out, std::string("--") + boundary + "--");
    return out;
}

}  // namespace mail
