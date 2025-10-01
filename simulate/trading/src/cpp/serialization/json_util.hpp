/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "taosim/decimal/decimal.hpp"

#include <rapidjson/document.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <optional>

//-------------------------------------------------------------------------

namespace taosim::json
{

//-------------------------------------------------------------------------

inline constexpr uint32_t kMaxDecimalPlaces = 8;

//-------------------------------------------------------------------------

struct IndentOptions
{
    char indentChar = ' ';
    uint8_t indentCharCount = 4;
};

struct FormatOptions
{
    std::optional<IndentOptions> indent = {};
    uint32_t decimals = kMaxDecimalPlaces;
};

[[nodiscard]] std::string json2str(
    const rapidjson::Value& json, const FormatOptions& formatOptions = {});

[[nodiscard]] rapidjson::Document str2json(const std::string& str);

void dumpJson(
    const rapidjson::Value& json,
    std::ofstream& ofs,
    const FormatOptions& formatOptions = {});

[[nodiscard]] rapidjson::Document loadJson(const std::filesystem::path& path);

[[nodiscard]] decimal_t getDecimal(const rapidjson::Value& json);

void serializeHelper(
    rapidjson::Document& json,
    const std::string& key,
    std::function<void(rapidjson::Document&)> serializer);

template<typename T>
void setOptionalMember(rapidjson::Document& json, const std::string& key, std::optional<T> opt)
{
    auto& allocator = json.GetAllocator();
    json.AddMember(
        rapidjson::Value{key.c_str(), allocator},
        [&] {
            if (!opt.has_value()) {
                return std::move(rapidjson::Value{}.SetNull());
            }
            if constexpr (std::constructible_from<rapidjson::Value, T>) {
                return std::move(rapidjson::Value{opt.value()});
            } else if constexpr (
                std::constructible_from<rapidjson::Value, const char*, decltype(allocator)>
                && requires (T t) {{ t.c_str() } -> std::convertible_to<const char*>; }) {
                return std::move(rapidjson::Value{opt.value().c_str(), allocator});
            } else {
                static_assert(false, "No conversion from T to rapidjson::Value exists");
            }
        }(),
        allocator);
}

//-------------------------------------------------------------------------

}  // namespace taosim::json

//-------------------------------------------------------------------------