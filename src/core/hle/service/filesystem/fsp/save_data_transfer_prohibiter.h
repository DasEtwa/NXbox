// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/am/polling_service.h"

namespace Service::FileSystem {

class ISaveDataTransferProhibiter : public PollingServiceFramework<ISaveDataTransferProhibiter> {
public:
    explicit ISaveDataTransferProhibiter(Core::System& system_);
    ~ISaveDataTransferProhibiter() override;
};

} // namespace Service::FileSystem
