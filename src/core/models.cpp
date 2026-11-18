#include "core/models.h"

#include <utility>

namespace usb {

bool Device::has_letter() const {
    for (const auto& part : partitions) {
        if (part.letter != 0) return true;
    }
    return false;
}

Occupant::~Occupant() {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
}

Occupant::Occupant(Occupant&& other) noexcept
    : pid(other.pid),
      name(std::move(other.name)),
      path(std::move(other.path)),
      icon(other.icon) {
    other.pid = 0;
    other.icon = nullptr;
}

Occupant& Occupant::operator=(Occupant&& other) noexcept {
    if (this == &other) return *this;
    if (icon) DestroyIcon(icon);
    pid = other.pid;
    name = std::move(other.name);
    path = std::move(other.path);
    icon = other.icon;
    other.pid = 0;
    other.icon = nullptr;
    return *this;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

}  // namespace usb
