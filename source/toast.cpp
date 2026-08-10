#include "toast.h"

#include <notifications/notifications.h>

#include "log.h"

namespace toast {

namespace {

bool s_available = false;

// Muted colours so a background job does not look like an error.
constexpr NMColor kText       = {255, 255, 255, 255};
constexpr NMColor kBackground = {40, 70, 120, 255};
constexpr NMColor kDone       = {40, 110, 60, 255};
constexpr NMColor kFailed     = {150, 45, 45, 255};

}  // namespace

bool Init() {
    if (s_available) return true;

    const NotificationModuleStatus status = NotificationModule_InitLibrary();
    if (status != NOTIFICATION_MODULE_RESULT_SUCCESS) {
        LOG("toast: notifications unavailable (%s) -- carrying on with the log only",
            NotificationModule_GetStatusStr(status));
        return false;
    }
    s_available = true;
    return true;
}

void Shutdown() {
    if (!s_available) return;
    NotificationModule_DeInitLibrary();
    s_available = false;
}

bool Available() {
    return s_available;
}

Progress::Progress(const std::string &text) {
    if (!s_available) {
        LOG("%s", text.c_str());
        return;
    }
    // keepUntilShown: the overlay may not be up yet this early in a boot, and
    // dropping the first message would lose the only sign the job started.
    const NotificationModuleStatus status = NotificationModule_AddDynamicNotificationEx(
        text.c_str(), &m_handle, kText, kBackground, nullptr, nullptr, true);
    if (status == NOTIFICATION_MODULE_RESULT_SUCCESS) {
        m_active = true;
    } else {
        LOG("toast: could not show a notification (%s)",
            NotificationModule_GetStatusStr(status));
    }
    LOG("%s", text.c_str());
}

Progress::~Progress() {
    // Never leave a toast pinned on screen if the job returned early.
    if (m_active && !m_finished) {
        NotificationModule_FinishDynamicNotification(m_handle, 2.0f);
    }
}

void Progress::Update(const std::string &text) {
    LOG("%s", text.c_str());
    if (!m_active || m_finished) return;
    NotificationModule_UpdateDynamicNotificationText(m_handle, text.c_str());
}

void Progress::Finish(const std::string &text, bool error) {
    LOG("%s", text.c_str());
    if (!m_active || m_finished) return;

    NotificationModule_UpdateDynamicNotificationText(m_handle, text.c_str());
    NotificationModule_UpdateDynamicNotificationBackgroundColor(m_handle,
                                                               error ? kFailed : kDone);
    if (error) {
        NotificationModule_FinishDynamicNotificationWithShake(m_handle, 1.0f, 6.0f);
    } else {
        NotificationModule_FinishDynamicNotification(m_handle, 4.0f);
    }
    m_finished = true;
}

void Info(const std::string &text) {
    LOG("%s", text.c_str());
    if (s_available) NotificationModule_AddInfoNotification(text.c_str());
}

void Error(const std::string &text) {
    LOG("%s", text.c_str());
    if (s_available) NotificationModule_AddErrorNotification(text.c_str());
}

}  // namespace toast
