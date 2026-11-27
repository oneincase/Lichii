// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "core/frontend/graphics_context.h"

namespace Vulkan {

/// One Vulkan implementation (ICD) the loader can start.
struct IcdInfo {
    std::string name;         ///< Driver family, e.g. "MoltenVK" or "KosmicKrisp"
    std::string json_path;    ///< The ICD manifest passed to the loader
    std::string library_path; ///< The driver library the manifest points at
    std::string api_version;  ///< The manifest's declared Vulkan version

    [[nodiscard]] bool Valid() const {
        return !json_path.empty() && !library_path.empty();
    }
};

/// Points VK_DRIVER_FILES at the Vulkan implementations this process should use. Must run before
/// the loader is first used; idempotent. On macOS this lists the bundled MoltenVK and, when it is
/// installed and new enough, KosmicKrisp. Elsewhere it does nothing.
/// @returns True when this call set the environment variable.
bool PrepareDriverEnvironment();

/// The ICDs PrepareDriverEnvironment() resolved, in the order they were passed to the loader.
[[nodiscard]] std::vector<IcdInfo> ResolvedIcds();

/// Human readable one-line summary of the driver environment, for the boot log.
[[nodiscard]] std::string DriverEnvironmentSummary();

/// Finds the KosmicKrisp ICD manifest installed on this host, if any. The newest Mesa wins when
/// several are installed. Returns nullopt when KosmicKrisp is not installed.
[[nodiscard]] std::optional<IcdInfo> FindKosmickrispIcd();

/// The driver the Vulkan backend should create its device on, given the user's choice.
/// Auto follows the shipped default (MoltenVK). Only meaningful on Apple.
[[nodiscard]] u32 SelectedDriverId();

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context = nullptr);

} // namespace Vulkan
