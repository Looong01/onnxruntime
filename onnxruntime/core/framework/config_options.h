// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <unordered_map>
#include "core/common/status.h"
#include "core/common/optional.h"

namespace onnxruntime {

/**
 * Configuration options that can be used by any struct by inheriting this class.
 * Provides infrastructure to add/get config entries
 */
struct ConfigOptions {
  // Maximum key/value string lengths specified in
  // core/session/onnxruntime_session_options_config_keys.h
  static constexpr size_t kMaxKeyLength = 1024;
  static constexpr size_t kMaxValueLength = 4096;

  std::unordered_map<std::string, std::string> configurations;

  // Gets the config string associated with the given config_key.
  // If not found, an empty optional is returned.
  std::optional<std::string> GetConfigEntry(const std::string& config_key) const noexcept;

  // Check if this instance of ConfigOptions has a config using the given config_key.
  // Returns true if found and copies the value into config_value.
  // Returns false if not found and clears config_value.
  bool TryGetConfigEntry(const std::string& config_key, std::string& config_value) const noexcept;

  // Get the config string in this instance of ConfigOptions using the given config_key
  // If there is no such config, the given default string will be returned
  std::string GetConfigOrDefault(const std::string& config_key, const std::string& default_value) const noexcept;

  // Add a config pair (config_key, config_value) to this instance of ConfigOptions
  Status AddConfigEntry(const char* config_key, const char* config_value) noexcept;

  // Gets a constant reference the map of all configurations.
  const std::unordered_map<std::string, std::string>& GetConfigOptionsMap() const noexcept;

  friend std::ostream& operator<<(std::ostream& os, const ConfigOptions& config_options);
};

}  // namespace onnxruntime
