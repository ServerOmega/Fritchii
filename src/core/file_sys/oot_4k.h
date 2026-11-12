// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

/// When EDEN_NX2_OOT_4K=1, grow DynamicTextureAllocatorMem and Graphics in any
/// System/HeapSize/*HeapDef.byml.zs found in this program RomFS (LayeredFS-style, in memory).
/// `romfs` is the packed RomFS image and is replaced on success.
void ApplyOot4kHeapDef(VirtualFile& romfs);

} // namespace FileSys
