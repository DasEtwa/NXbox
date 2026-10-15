// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/accounts.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/fs/fs.h"
#include "common/fs/fs_uwp.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/file_sys/savedata_factory.h"
#include "core/hle/service/acc/profile_manager.h"

namespace EdenXbox::Accounts {
namespace {
bool ValidUuid(const std::string& value) {
    return value.size() == 32 &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) {
                           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                  (c >= 'A' && c <= 'F');
                       }) &&
           value != std::string(32, '0');
}
} // namespace

State Load(const std::filesystem::path& local_state) {
    State state;
    try {
        std::ifstream input(local_state / "accounts.json");
        if (!input)
            return state;
        const auto json = nlohmann::json::parse(input);
        state.initialized = json.value("initialized", false);
        state.multi = json.value("multi", false);
        state.selected_uuid = json.value("selected_uuid", std::string{});
        if (!ValidUuid(state.selected_uuid))
            state.selected_uuid.clear();
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return state;
}

bool Save(const std::filesystem::path& local_state, const State& state) {
    const auto temporary = local_state / "accounts.json.tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << nlohmann::json{{"initialized", state.initialized},
                                 {"multi", state.multi},
                                 {"selected_uuid", state.selected_uuid}}
                      .dump(2);
        output.close();
        if (!output)
            return false;
    }
    const auto destination = local_state / "accounts.json";
    if (Common::FS::Exists(destination)) {
        return ReplaceFileFromAppW(destination.c_str(), temporary.c_str(), nullptr, 0, nullptr,
                                   nullptr) != 0;
    }
    return MoveFileFromAppW(temporary.c_str(), destination.c_str()) != 0;
}

void Apply(const std::filesystem::path& local_state) {
    const auto state = Load(local_state);
    Service::Account::ProfileManager::SetMultiAccountEnabled(state.multi);
    Service::Account::ProfileManager profiles;
    std::size_t index = 0;
    if (state.multi && !state.selected_uuid.empty()) {
        index = profiles.GetUserIndex(Common::UUID{state.selected_uuid}).value_or(0);
    }
    Settings::values.current_user = static_cast<s32>(index);
    for (const auto uuid : profiles.GetAllUsers())
        profiles.CloseUser(uuid);
    profiles.OpenUser(*profiles.GetUser(index));
}

bool DeleteSaves(const std::string& value) {
    if (!ValidUuid(value))
        return false;
    const Common::UUID uuid{value};
    const auto nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    for (const bool future : {false, true}) {
        const auto relative =
            FileSys::SaveDataFactory::GetUserGameSaveDataRoot(uuid.AsU128(), future);
        if (!Common::FS::RemoveDirRecursively(nand /
                                              std::filesystem::path(relative).relative_path()))
            return false;
    }
    return true;
}
} // namespace EdenXbox::Accounts
