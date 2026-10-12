// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/system_archive/data/mii_resources.h"
#include "core/file_sys/system_archive/mii_model.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::SystemArchive {

VirtualDir MiiModel() {
    auto out = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                    std::vector<VirtualDir>{}, "data");

    out->AddFile(MakeArrayFile(MiiModelData::TEXTURES, "NXTextureLowLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURES, "NXTextureLowSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURES, "NXTextureMidLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURES, "NXTextureMidSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPES, "ShapeHigh.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPES, "ShapeMid.dat"));

    return out;
}

} // namespace FileSys::SystemArchive
