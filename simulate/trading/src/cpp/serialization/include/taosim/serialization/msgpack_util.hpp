/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <fmt/format.h>
#include <msgpack.hpp>

#include <source_location>

//-------------------------------------------------------------------------

namespace taosim::serialization
{

//-------------------------------------------------------------------------

class HumanReadableStream
{
    msgpack::sbuffer m_underlying;

public:
    explicit HumanReadableStream(size_t initByteSize = MSGPACK_SBUFFER_INIT_SIZE)
        : m_underlying{initByteSize}
    {}

    [[nodiscard]] const char* data() const noexcept { return m_underlying.data(); }
    [[nodiscard]] size_t size() const noexcept { return m_underlying.size(); }

    void write(const char* buf, size_t len) { m_underlying.write(buf, len); }
};

class BinaryStream
{
    msgpack::sbuffer m_underlying;

public:
    explicit BinaryStream(size_t initByteSize = MSGPACK_SBUFFER_INIT_SIZE)
        : m_underlying{initByteSize}
    {}

    [[nodiscard]] const char* data() const noexcept { return m_underlying.data(); }
    [[nodiscard]] size_t size() const noexcept { return m_underlying.size(); }

    void write(const char* buf, size_t len) { m_underlying.write(buf, len); }
};

//-------------------------------------------------------------------------

struct MsgPackError : msgpack::type_error
{
    std::string message;

    explicit MsgPackError(std::source_location sl = std::source_location::current()) noexcept
    {
        message = fmt::format("{}#L{}: {}", sl.file_name(), sl.line(), msgpack::type_error::what());
    }

    const char* what() const noexcept override { return message.c_str(); }
};

//-------------------------------------------------------------------------

[[nodiscard]] inline const msgpack::object* msgpackFind(
    const msgpack::object& o, std::string_view key)
{
    for (size_t i = 0; i < o.via.map.size; ++i) {
        const auto& k = o.via.map.ptr[i].key;
        if (k.type == msgpack::type::STR) {
            std::string_view ks{k.via.str.ptr, k.via.str.size};
            if (ks == key) {
                return &o.via.map.ptr[i].val;
            }
        }
    }
    return nullptr;
}

//-------------------------------------------------------------------------

}  // namespace taosim::serialization

//-------------------------------------------------------------------------