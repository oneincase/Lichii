// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#ifdef _WIN32
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "video_core/vulkan_common/vulkan_overlay_filter.h"
#endif

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#ifdef _WIN32
#include "common/scope_exit.h"
#endif
#include "video_core/vulkan_common/vulkan_library.h"

#ifdef __APPLE__
#include <algorithm>
#include <cctype>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/sysctl.h>
#include <vector>

#include "common/settings.h"
#include <vulkan/vulkan_core.h>
#endif

#ifdef _WIN32
namespace {

void DisableOverwolfLayers() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> names;
        std::map<std::string, std::string> environment;
        for (const auto hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
                for (const auto subkey : {L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",
                                          L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers"}) {
                    HKEY key{};
                    if (RegOpenKeyExW(hive, subkey, 0, KEY_QUERY_VALUE | view, &key) !=
                        ERROR_SUCCESS) {
                        continue;
                    }
                    SCOPE_EXIT {
                        RegCloseKey(key);
                    };
                    for (DWORD index = 0;; ++index) {
                        std::wstring path(32768, L'\0');
                        DWORD length = static_cast<DWORD>(path.size());
                        DWORD type{}, enabled{}, size = sizeof(enabled);
                        const auto result =
                            RegEnumValueW(key, index, path.data(), &length, nullptr, &type,
                                          reinterpret_cast<BYTE*>(&enabled), &size);
                        if (result == ERROR_NO_MORE_ITEMS) {
                            break;
                        }
                        if (result != ERROR_SUCCESS || type != REG_DWORD ||
                            size != sizeof(enabled) || enabled != 0) {
                            continue;
                        }
                        path.resize(length);
                        try {
                            const std::filesystem::path manifest_path{path};
                            std::error_code error;
                            const auto bytes = std::filesystem::file_size(manifest_path, error);
                            if (error || bytes > 1024 * 1024) {
                                continue;
                            }
                            std::ifstream file{manifest_path};
                            if (!file) {
                                continue;
                            }
                            const auto manifest = nlohmann::json::parse(file, nullptr, false);
                            const auto utf8 = manifest_path.u8string();
                            Detail::CollectOverwolfLayerControls(
                                manifest, std::string{utf8.begin(), utf8.end()}, names,
                                environment);
                        } catch (const std::exception& exception) {
                            LOG_WARNING(Render_Vulkan,
                                        "Skipping unreadable Vulkan layer manifest: {}",
                                        exception.what());
                        }
                    }
                }
            }
        }
        if (!names.empty()) {
            std::string disabled;
            if (const auto* current = std::getenv("VK_LOADER_LAYERS_DISABLE")) {
                disabled = current;
            }
            for (const auto& name : names) {
                if (!disabled.empty()) {
                    disabled += ',';
                }
                disabled += name;
            }
            environment.insert_or_assign("VK_LOADER_LAYERS_DISABLE", std::move(disabled));
        }
        for (const auto& [name, value] : environment) {
            if (_putenv_s(name.c_str(), value.c_str()) != 0) {
                LOG_WARNING(Render_Vulkan, "Could not set Overwolf layer disable control {}", name);
            }
        }
        if (!names.empty()) {
            LOG_INFO(Render_Vulkan,
                     "Disabled {} installed Overwolf Vulkan layer(s) for this process",
                     names.size());
        }
    });
}
} // namespace
#endif

namespace Vulkan {

namespace {

#ifdef __APPLE__
namespace fs = std::filesystem;

std::string ToLower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

/// Reads the quoted value of a top-level JSON key. ICD manifests are tiny and written by the
/// loader's own rules, so a full JSON parser is not needed here (and this file is linked into
/// targets that do not link one).
std::optional<std::string> ReadJsonString(const std::string& text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t key_pos = text.find(needle);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    const size_t colon = text.find(':', key_pos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const size_t first_quote = text.find('"', colon + 1);
    if (first_quote == std::string::npos) {
        return std::nullopt;
    }
    const size_t second_quote = text.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
        return std::nullopt;
    }
    return text.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

/// Parses one ICD manifest. Relative library paths are resolved against the manifest's directory,
/// as the loader does.
std::optional<IcdInfo> LoadIcd(const fs::path& json_path, std::string name) {
    const std::string text = ReadFile(json_path);
    if (text.empty()) {
        return std::nullopt;
    }
    const auto library = ReadJsonString(text, "library_path");
    if (!library || library->empty()) {
        return std::nullopt;
    }
    fs::path library_path{*library};
    if (library_path.is_relative()) {
        library_path = json_path.parent_path() / library_path;
    }
    std::error_code error;
    if (!fs::exists(library_path, error) || error) {
        LOG_WARNING(Render_Vulkan, "Vulkan ICD {} points at missing library {}", json_path.string(),
                    library_path.string());
        return std::nullopt;
    }
    error.clear();
    library_path = fs::weakly_canonical(library_path, error);
    if (error) {
        library_path = (json_path.parent_path() / *library).lexically_normal();
    }
    IcdInfo info{};
    info.name = std::move(name);
    info.json_path = json_path.string();
    info.library_path = library_path.string();
    info.api_version = ReadJsonString(text, "api_version").value_or("");
    return info;
}

/// KosmicKrisp needs Metal 4, which macOS 26 and Apple silicon only provide. The bundled
/// driver is not offered to the loader elsewhere: the loader would keep a driver that cannot
/// create a device in its list, and older systems are the ones most sensitive to that.
bool MeetsKosmickrispOsRequirement() {
#if defined(__aarch64__)
    char version[64]{};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0) {
        return false;
    }
    return std::atoi(version) >= 26;
#else
    return false;
#endif
}

/// An XDG base directory, falling back to the usual place under the home directory.
fs::path XdgDirectory(const char* env_name, const char* fallback) {
    if (const char* value = std::getenv(env_name); value != nullptr && value[0] != '\0') {
        return fs::path{value};
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path{home} / fallback;
    }
    return {};
}

/// Every directory a Vulkan SDK, Homebrew or a Mesa build installs ICD manifests into.
std::vector<fs::path> IcdDirectories() {
    std::vector<fs::path> dirs;
    // Lichii's own directory first: an implementation placed there (a locally built KosmicKrisp,
    // for instance) is what the app runs, ahead of anything a package manager installed.
    if (const fs::path data = XdgDirectory("XDG_DATA_HOME", ".local/share"); !data.empty()) {
        dirs.emplace_back(data / "lichii/vulkan/icd.d");
    }
    if (const fs::path config = XdgDirectory("XDG_CONFIG_HOME", ".config"); !config.empty()) {
        dirs.emplace_back(config / "lichii/vulkan/icd.d");
    }
    // The KosmicKrisp a release ships, used when the host has no newer one of its own.
    if (MeetsKosmickrispOsRequirement()) {
        dirs.emplace_back(Common::FS::GetBundleDirectory() / "Contents/Resources/vulkan/icd.d");
    }
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk != nullptr && sdk[0] != '\0') {
        dirs.emplace_back(fs::path(sdk) / "share/vulkan/icd.d");
    }
    dirs.emplace_back("/opt/homebrew/share/vulkan/icd.d");
    dirs.emplace_back("/opt/homebrew/etc/vulkan/icd.d");
    dirs.emplace_back("/usr/local/share/vulkan/icd.d");
    dirs.emplace_back("/usr/local/etc/vulkan/icd.d");
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        dirs.emplace_back(fs::path(home) / ".local/share/vulkan/icd.d");
    }
    dirs.emplace_back("/etc/vulkan/icd.d");
    // Homebrew keeps every installed Mesa under its Cellar; the share symlinks can be stale after
    // an upgrade, so the Cellar directories are searched as well.
    std::error_code error;
    const fs::path cellar{"/opt/homebrew/Cellar/mesa"};
    if (fs::is_directory(cellar, error) && !error) {
        std::vector<fs::path> versions;
        for (const auto& entry : fs::directory_iterator(cellar, error)) {
            if (error) {
                break;
            }
            if (entry.is_directory(error) && !error) {
                versions.push_back(entry.path() / "share/vulkan/icd.d");
            }
        }
        // Newest version first.
        std::ranges::sort(versions, std::greater<>{});
        dirs.insert(dirs.end(), versions.begin(), versions.end());
    }
    return dirs;
}

/// Finds the newest KosmicKrisp manifest under the ICD directories.
std::optional<IcdInfo> FindKosmickrispIcdIn(const std::vector<fs::path>& dirs) {
    std::error_code error;
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, error) || error) {
            error.clear();
            continue;
        }
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(dir, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file(error) || error) {
                error.clear();
                continue;
            }
            const std::string name = ToLower(entry.path().filename().string());
            if (name.find("kosmickrisp") != std::string::npos && entry.path().extension() == ".json") {
                candidates.push_back(entry.path());
            }
        }
        std::ranges::sort(candidates);
        for (const auto& candidate : candidates) {
            if (auto info = LoadIcd(candidate, "KosmicKrisp"); info) {
                return info;
            }
        }
    }
    return std::nullopt;
}

/// The MoltenVK manifest shipped in the app bundle, else one installed on the host.
std::optional<IcdInfo> FindMoltenVkIcd() {
    const auto bundle_icd =
        Common::FS::GetBundleDirectory() / "Contents/Resources/MoltenVK_icd.json";
    if (auto info = LoadIcd(bundle_icd, "MoltenVK"); info) {
        return info;
    }
    std::vector<fs::path> dirs = IcdDirectories();
    std::error_code error;
    const fs::path moltenvk_cellar{"/opt/homebrew/Cellar/molten-vk"};
    if (fs::is_directory(moltenvk_cellar, error) && !error) {
        std::vector<fs::path> versions;
        for (const auto& entry : fs::directory_iterator(moltenvk_cellar, error)) {
            if (error) {
                break;
            }
            if (entry.is_directory(error) && !error) {
                versions.push_back(entry.path() / "etc/vulkan/icd.d");
            }
        }
        std::ranges::sort(versions, std::greater<>{});
        dirs.insert(dirs.end(), versions.begin(), versions.end());
    }
    for (const auto& dir : dirs) {
        if (auto info = LoadIcd(dir / "MoltenVK_icd.json", "MoltenVK"); info) {
            return info;
        }
    }
    return std::nullopt;
}

/// Joins the manifests for the loader's VK_DRIVER_FILES (a colon separated list on macOS).
std::string JoinIcdPaths(const std::vector<IcdInfo>& icds) {
    std::string joined;
    for (const auto& icd : icds) {
        if (!joined.empty()) {
            joined += ':';
        }
        joined += icd.json_path;
    }
    return joined;
}
#endif // __APPLE__

std::once_flag driver_environment_once;
std::vector<IcdInfo> resolved_icds;
bool driver_environment_set{};

} // Anonymous namespace

bool PrepareDriverEnvironment() {
    std::call_once(driver_environment_once, [] {
#ifdef __APPLE__
        // A list the user set themselves wins: it may be how they run a driver this build cannot
        // discover, and silently replacing it would be surprising.
        for (const char* name : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES"}) {
            if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
                LOG_INFO(Render_Vulkan, "Vulkan driver environment: {} is set by the environment: {}",
                         name, value);
                resolved_icds.clear();
                return;
            }
        }

        std::vector<IcdInfo> icds;
        if (auto moltenvk = FindMoltenVkIcd(); moltenvk) {
            icds.push_back(std::move(*moltenvk));
        } else {
            LOG_WARNING(Render_Vulkan,
                        "No MoltenVK ICD manifest found; the Vulkan loader will not see MoltenVK");
        }
        if (auto kosmickrisp = FindKosmickrispIcd(); kosmickrisp) {
            icds.push_back(std::move(*kosmickrisp));
        } else {
            LOG_INFO(Render_Vulkan, "KosmicKrisp is not installed; the API list stays on MoltenVK");
        }
        if (icds.empty()) {
            return;
        }

        const std::string joined = JoinIcdPaths(icds);
        // Overwrites, since the variable may be inherited but empty.
        if (setenv("VK_DRIVER_FILES", joined.c_str(), 1) != 0) {
            LOG_WARNING(Render_Vulkan, "Could not set VK_DRIVER_FILES");
            return;
        }
        resolved_icds = std::move(icds);
        driver_environment_set = true;
        for (const auto& icd : resolved_icds) {
            LOG_INFO(Render_Vulkan, "Vulkan ICD: {} (Vulkan {}) at {}", icd.name,
                     icd.api_version.empty() ? "?" : icd.api_version, icd.library_path);
        }
        LOG_INFO(Render_Vulkan, "VK_DRIVER_FILES={}", joined);
#else
        // Other hosts keep the driver setup they had: the loader finds the installed drivers.
#endif
    });
    return driver_environment_set;
}

std::vector<IcdInfo> ResolvedIcds() {
    return resolved_icds;
}

std::string DriverEnvironmentSummary() {
    if (resolved_icds.empty()) {
        return "environment-provided";
    }
    std::string summary;
    for (const auto& icd : resolved_icds) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += icd.name;
        if (!icd.api_version.empty()) {
            summary += " (Vulkan " + icd.api_version + ")";
        }
    }
    return summary;
}

std::optional<IcdInfo> FindKosmickrispIcd() {
#ifdef __APPLE__
    // The UI asks for this on every API label refresh; the directories do not change while the
    // app runs.
    static const std::optional<IcdInfo> discovered = []() -> std::optional<IcdInfo> {
        if (const char* override_path = std::getenv("EDEN_NX2_KK_ICD");
            override_path != nullptr && override_path[0] != '\0') {
            if (auto info = LoadIcd(fs::path(override_path), "KosmicKrisp"); info) {
                return info;
            }
            LOG_WARNING(Render_Vulkan, "EDEN_NX2_KK_ICD={} is not a usable ICD manifest",
                        override_path);
        }
        return FindKosmickrispIcdIn(IcdDirectories());
    }();
    return discovered;
#else
    return std::nullopt;
#endif
}

u32 SelectedDriverId() {
#ifdef __APPLE__
    // EDEN_NX2_VULKAN_DRIVER=mvk|kk picks the implementation for A/B runs and for headless
    // tools that never load the UI settings.
    if (const char* value = std::getenv("EDEN_NX2_VULKAN_DRIVER");
        value != nullptr && value[0] != '\0') {
        const std::string name = ToLower(value);
        if (name == "kk" || name == "kosmickrisp") {
            return VK_DRIVER_ID_MESA_KOSMICKRISP;
        }
        if (name == "mvk" || name == "moltenvk") {
            return VK_DRIVER_ID_MOLTENVK;
        }
        LOG_WARNING(Render_Vulkan, "Unknown EDEN_NX2_VULKAN_DRIVER={}", value);
    }
    switch (Settings::values.vulkan_driver.GetValue()) {
    case Settings::VulkanDriver::KosmicKrisp:
        return VK_DRIVER_ID_MESA_KOSMICKRISP;
    case Settings::VulkanDriver::Auto:
    case Settings::VulkanDriver::MoltenVK:
    default:
        return VK_DRIVER_ID_MOLTENVK;
    }
#else
    return 0;
#endif
}

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context) {
#ifdef _WIN32
    DisableOverwolfLayers();
#endif
    PrepareDriverEnvironment();
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Android manages its Vulkan driver from the frontend.
    return context->GetDriverLibrary();
#else
    auto library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    const auto bundle_directory = Common::FS::GetBundleDirectory();
    // KosmicKrisp is a Vulkan ICD and needs the Khronos loader; loading MoltenVK directly would
    // hide it. The loader is preferred, and MoltenVK alone is the last resort.
    const std::string bundle_loader =
        (bundle_directory / "Contents/Frameworks/libvulkan.1.dylib").string();
    const std::string bundle_moltenvk =
        (bundle_directory / "Contents/Frameworks/libMoltenVK.dylib").string();
    std::string sdk_loader;
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk != nullptr && sdk[0] != '\0') {
        sdk_loader = (fs::path(sdk) / "lib/libvulkan.1.dylib").string();
    }
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"),
                                   bundle_loader.empty() ? nullptr : bundle_loader.c_str(),
                                   sdk_loader.empty() ? nullptr : sdk_loader.c_str(),
                                   "/opt/homebrew/lib/libvulkan.1.dylib",
                                   "/usr/local/lib/libvulkan.1.dylib",
                                   bundle_moltenvk.empty() ? nullptr : bundle_moltenvk.c_str(),
                                   nullptr};
    for (const char* library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            LOG_INFO(Render_Vulkan, "Using Vulkan library {}", library_path);
            break;
        }
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan
