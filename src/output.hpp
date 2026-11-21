#pragma once
#include "sessions.hpp"

namespace shop {
void write_users(const fs::path& path, const UserDays& rows, const Keys& users, const Dimensions& d, std::span<uint8_t> spare);
void write_campaigns(const fs::path& path, const CampaignDays& totals, const Dimensions& d);
}
