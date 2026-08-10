// mail_test.cpp — checks that a built message and its index fields agree.
//
// The console locates each header value with (length << 20) | offset into the
// stored message. This verifies that every span we record actually lands on
// the bytes it claims to -- the same check that decoded the format from a real
// console entry in the first place.
//
//   g++ -std=gnu++20 -fsanitize=address,undefined -I source -o /tmp/mail_test \
//       tools/mail_test.cpp source/mailmsg.cpp && /tmp/mail_test
#include <cstdio>
#include <string>

#include "mail.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &what) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", what.c_str());
    if (!condition) g_failures++;
}

// Mirrors wc24::PackField without pulling in the console headers.
uint32_t Pack(uint32_t offset, uint32_t length) {
    return (length << 20) | (offset & 0x000FFFFF);
}

// Resolves a packed field back to the bytes it points at.
std::string Resolve(const std::string &text, uint32_t packed) {
    const uint32_t offset = packed & 0x000FFFFF;
    const uint32_t length = packed >> 20;
    if (offset + length > text.size()) return "<out of range>";
    return text.substr(offset, length);
}

}  // namespace

int main() {
    mail::Message msg;
    msg.from     = "w0000000000000000@rc24.xyz";
    msg.to       = "w1234567890123456@rc24.xyz";
    msg.subject  = "Wii Message";
    msg.alt_name = "wuc24";
    msg.body     = "Hello from wuc24.\nSecond line.\n";

    mail::FieldSpans  spans;
    const std::string text = mail::BuildMessage(msg, 17, 0x0018FDEF28C70E57ULL, 66587943, spans);

    std::printf("built %zu bytes\n\n--- message ---\n%s\n--- end ---\n\n", text.size(),
                text.c_str());

    std::printf("1. every packed field resolves to exactly its value\n");
    Check(Resolve(text, Pack(spans.from_offset, spans.from_length)) == msg.from,
          "packed_from -> the From address");
    Check(Resolve(text, Pack(spans.to_offset, spans.to_length)) == msg.to,
          "packed_to -> the To address");
    Check(Resolve(text, Pack(spans.subject_offset, spans.subject_length)) == msg.subject,
          "packed_subject -> the Subject");
    Check(Resolve(text, Pack(spans.charset_offset, spans.charset_length)) == "utf-8",
          "packed_charset -> the charset, value only (received form)");
    // The body span deliberately covers the text AND the blank lines that
    // follow it, up to the closing boundary -- that is what the real entry
    // does (message_length 46 for a 38-character line).
    Check(text.substr(spans.body_offset, spans.body_length).rfind("Hello from wuc24.", 0) == 0,
          "message_offset -> the start of the body text");
    Check(text.substr(spans.body_offset, spans.body_length).find("Second line.") !=
              std::string::npos,
          "message_length covers the whole body");

    std::printf("\n2. structure the message board relies on\n");
    Check(text.rfind("From: ", 0) == 0, "starts at From: -- received mail has no Date: header");
    Check(text.find("Date:") == std::string::npos, "no Date: header at all");
    Check(text.find("multipart/mixed; boundary=") != std::string::npos, "is multipart/mixed");
    Check(text.find("Content-Description: wiimail") != std::string::npos,
          "text part is marked as the wiimail body");
    Check(text.find("Content-Transfer-Encoding") == std::string::npos,
          "no Content-Transfer-Encoding, matching received mail");
    Check(text.find("MIME-Version: 1.0") != std::string::npos, "declares MIME");
    Check(text.compare(spans.mime_offset, 2, "--") == 0,
          "header_len points at the first boundary line");
    Check(text.substr(text.size() - 2) == "--", "ends with the closing boundary");
    Check(text.find("X-Wii-AltName: ") != std::string::npos, "carries the display name");
    // "wuc24" as UTF-16BE base64.
    Check(text.find("X-Wii-AltName: AHcAdQBjADIANA==") != std::string::npos,
          "display name is base64 of UTF-16BE");

    std::printf("\n3. offsets stay in range and in header order\n");
    Check(spans.from_offset < spans.to_offset, "From precedes To");
    Check(spans.to_offset < spans.subject_offset, "To precedes Subject");
    Check(spans.subject_offset < spans.mime_offset, "Subject precedes the MIME body");
    Check(spans.mime_offset < spans.charset_offset, "boundary precedes the part charset");
    Check(spans.charset_offset < spans.body_offset, "charset precedes the body");
    Check(spans.body_offset + spans.body_length < text.size(),
          "body stops before the closing boundary");
    Check(Pack(spans.body_offset, spans.body_length) >> 20 == spans.body_length,
          "lengths survive packing");

    std::printf("\n4. awkward inputs\n");
    {
        mail::Message empty;
        empty.from = "a@b";
        empty.to   = "c@d";
        mail::FieldSpans s2;
        const std::string t2 = mail::BuildMessage(empty, 0, 0, 0, s2);
        Check(s2.subject_length == 0, "empty subject gives a zero-length span");
        Check(t2.find("X-Wii-AltName") == std::string::npos,
              "no display name means no AltName header");
        Check(s2.body_offset <= t2.size(), "empty body offset is still in range");
    }
    {
        mail::Message long_msg;
        long_msg.from    = "a@b";
        long_msg.to      = "c@d";
        long_msg.subject = std::string(200, 'S');
        long_msg.body    = std::string(5000, 'B');
        mail::FieldSpans s3;
        const std::string t3 = mail::BuildMessage(long_msg, 1, 0, 0, s3);
        Check(t3.substr(s3.subject_offset, s3.subject_length) == long_msg.subject,
              "a 200-char subject still resolves");
        Check(t3.substr(s3.body_offset, s3.body_length).rfind(long_msg.body, 0) == 0,
              "a 5000-byte body still resolves");
        Check(s3.body_offset < (1u << 20), "offsets fit the 20-bit field");
    }

    std::printf("\n5. date rendering (minutes since 1900)\n");
    {
        struct Case { uint32_t minutes; const char *expect; };
        // Cross-checked against Python: (datetime - 1900-01-01) in minutes.
        const Case cases[] = {
            {0,        "01 Jan 1900 00:00:00 -0000"},
            {525600,   "01 Jan 1901 00:00:00 -0000"},   // 365 days
            {52596000, "02 Jan 2000 00:00:00 -0000"},   // 1900 was not a leap year
            {66587943, "09 Aug 2026 15:03:00 -0000"},   // the captured entry's timestamp
        };
        for (const auto &c : cases) {
            const std::string got = mail::FormatDate(c.minutes);
            Check(got == c.expect, std::string(c.expect) + "  (got " + got + ")");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
