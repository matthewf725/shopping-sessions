#pragma once
#include "sessions.hpp"

namespace shop {
void write_users(const fs::path& path, const std::vector<UserDay>& rows, const Keys& users, const Dimensions& d);
void write_campaigns(const fs::path& path, const CampaignDays& totals, const Dimensions& d);
}
