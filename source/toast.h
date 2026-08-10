// toast.h — on-screen notifications.
//
// Wraps NotificationModule, which lives in a separate Aroma module the user may
// not have installed. Everything here degrades to a log line when it is absent,
// so a missing module never costs more than the notifications themselves.
#pragma once

#include <string>

namespace toast {

// Safe to call more than once. Returns false if the module is unavailable.
bool Init();
void Shutdown();

// True when notifications will actually appear on screen.
bool Available();

// A toast that stays up and can be re-worded while work proceeds.
class Progress {
public:
    explicit Progress(const std::string &text);
    ~Progress();

    Progress(const Progress &)            = delete;
    Progress &operator=(const Progress &) = delete;

    void Update(const std::string &text);

    // Fades out. `error` colours it red and shakes it.
    void Finish(const std::string &text, bool error = false);

private:
    uint32_t m_handle   = 0;
    bool     m_active   = false;
    bool     m_finished = false;
};

void Info(const std::string &text);
void Error(const std::string &text);

}  // namespace toast
