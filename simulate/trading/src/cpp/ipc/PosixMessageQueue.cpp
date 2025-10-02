/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "taosim/ipc/PosixMessageQueue.hpp"

#include "taosim/ipc/util.hpp"

#include <fmt/format.h>

#include <source_location>
#include <stdexcept>
#include <vector>

//-------------------------------------------------------------------------

namespace taosim::ipc
{

//-------------------------------------------------------------------------

PosixMessageQueue::PosixMessageQueue(const PosixMessageQueueDesc& desc)
    : m_desc{desc}
{
    m_desc.name = desc.name.starts_with("/") ? desc.name : "/" + desc.name;

    m_handle = mq_open(
        m_desc.name.c_str(),
        m_desc.oflag,
        m_desc.mode,
        &m_desc.attr);

    if (m_handle == static_cast<mqd_t>(-1)) {
        throw std::runtime_error{fmt::format(
            "{}: Failed to create POSIX mqueue with name '{}': {} ({})",
            std::source_location::current().function_name(),
            m_desc.name, errno, std::strerror(errno)
        )};
    }
}

//-------------------------------------------------------------------------

PosixMessageQueue::~PosixMessageQueue() noexcept
{
    mq_close(m_handle);
    mq_unlink(m_desc.name.c_str());
}

//-------------------------------------------------------------------------

bool PosixMessageQueue::send(std::span<const char> msg, uint32_t priority) noexcept
{
    const timespec ts = makeTimespec(m_desc.timeout.value_or(0));
    return mq_timedsend(m_handle, msg.data(), msg.size(), priority, &ts) == 0;
}

//-------------------------------------------------------------------------

ssize_t PosixMessageQueue::receive(std::span<char> msg, uint32_t* priority) noexcept
{
    const timespec ts = makeTimespec(m_desc.timeout.value_or(0));
    return mq_timedreceive(m_handle, msg.data(), msg.size(), priority, &ts);
}

//-------------------------------------------------------------------------

void PosixMessageQueue::flush() noexcept
{
    std::vector<char> sink(m_desc.attr.mq_maxmsg * m_desc.attr.mq_msgsize);
    while (mq_receive(m_handle, sink.data(), sink.size(), nullptr) != -1);
}

//-------------------------------------------------------------------------

}  // namespace taosim::ipc

//-------------------------------------------------------------------------