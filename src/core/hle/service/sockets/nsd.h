// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Sockets {

// The name nsd hands back for a server FQDN: '%' becomes the environment identifier (lp1).
// sfdnsres runs names through it when the caller sets use_nsd_resolve.
std::string ResolveFqdn(const std::string& fqdn_in);

class NSD final : public ServiceFramework<NSD> {
public:
    explicit NSD(Core::System& system_, const char* name);
    ~NSD() override;

private:
    void SetChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx);
    void Resolve(HLERequestContext& ctx);
    void ResolveEx(HLERequestContext& ctx);
    void GetEnvironmentIdentifier(HLERequestContext& ctx);
    void GetApplicationServerEnvironmentType(HLERequestContext& ctx);
};

} // namespace Service::Sockets
