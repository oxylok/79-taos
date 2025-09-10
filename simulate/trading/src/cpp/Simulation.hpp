/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "Agent.hpp"
#include "taosim/simulation/SimulationConfig.hpp"
#include "IConfigurable.hpp"
#include "IMessageable.hpp"
#include "LocalAgentManager.hpp"
#include "Message.hpp"
#include "MessageQueue.hpp"
#include "Recoverable.hpp"
#include "taosim/simulation/SimulationSignals.hpp"
#include "taosim/simulation/SimulationState.hpp"
#include "common.hpp"
#include "taosim/simulation/ISimulation.hpp"

#include <fmt/core.h>

#include <barrier>

//-------------------------------------------------------------------------

class MultiBookExchangeAgent;

//-------------------------------------------------------------------------

class Simulation
    : public taosim::simulation::ISimulation,
      public IMessageable,
      public IConfigurable
{
public:
    Simulation() noexcept;
    Simulation(uint32_t blockIdx, const fs::path& logDir);

    void dispatchMessage(
        Timestamp occurrence,
        Timestamp delay,
        const std::string& source,
        const std::string& target,
        const std::string& type,
        MessagePayload::Ptr payload = MessagePayload::create<EmptyPayload>()) const;

    template<typename... PrioArgs>
    requires std::constructible_from<PrioritizedMessage, Message::Ptr, PrioArgs...>
    void dispatchMessageWithPriority(
        Timestamp occurrence,
        Timestamp delay,
        const std::string& source,
        const std::string& target,
        const std::string& type,
        MessagePayload::Ptr payload,
        PrioArgs&&... prioArgs) const
    {
        queueMessageWithPriority(
            Message::create(occurrence, occurrence + delay, source, target, type, payload),
            std::forward<PrioArgs>(prioArgs)...);
    }

    void dispatchGenericMessage(
        Timestamp occurrence,
        Timestamp delay,
        const std::string& source,
        const std::string& target,
        const std::string& type,
        std::map<std::string, std::string> payload) const;

    void queueMessage(Message::Ptr msg) const;

    template<typename... Args>
    requires std::constructible_from<PrioritizedMessage, Args...>
    void queueMessageWithPriority(Args&&... args) const
    {
        m_messageQueue.push(PrioritizedMessage(std::forward<Args>(args)...));
    }

    template<typename Fn>
    void simulate(std::barrier<Fn>& barrier)
    {
        if (m_state == taosim::simulation::SimulationState::STOPPED) return;
        else if (m_state == taosim::simulation::SimulationState::INACTIVE) start();

        while (m_time.current < m_time.start + m_time.duration) {
            step();
            barrier.arrive_and_wait();
        }

        stop();
        fmt::println("end");
    }

    void simulate();

    [[nodiscard]] taosim::accounting::Account& account(const LocalAgentId& id) const noexcept;
    [[nodiscard]] std::span<const std::unique_ptr<Agent>> agents() const noexcept;
    [[nodiscard]] Timestamp currentTimestamp() const noexcept;
    [[nodiscard]] Timestamp duration() const noexcept;
    [[nodiscard]] MultiBookExchangeAgent* exchange() const noexcept { return m_exchange; }
    [[nodiscard]] DistributedProxyAgent* proxy() const noexcept { return m_proxy; }
    [[nodiscard]] taosim::simulation::SimulationSignals& signals() const noexcept;
    [[nodiscard]] std::mt19937& rng() const noexcept;
    [[nodiscard]] const taosim::simulation::SimulationConfig& config() const noexcept { return m_config2; }
    [[nodiscard]] const std::unique_ptr<LocalAgentManager>& localAgentManager() const noexcept { return m_localAgentManager; }
    [[nodiscard]] auto&& time(this auto&& self) noexcept { return self.m_time; }
    [[nodiscard]] uint32_t blockIdx() const noexcept { return m_blockIdx; }
    [[nodiscard]] auto&& logWindow(this auto&& self) noexcept { return self.m_logWindow; }
    [[nodiscard]] auto&& messageQueue(this auto&& self) noexcept { return self.m_messageQueue; }

    [[nodiscard]] BookId bookIdCanon(BookId bookId) const noexcept
    {
        return m_blockIdx * m_exchange->books().size() + bookId;
    }
    
    virtual const fs::path& logDir() const noexcept override { return m_logDir; }
    virtual void receiveMessage(Message::Ptr msg) override;
    virtual void configure(const pugi::xml_node& node) override;

    template<typename... Args>
    void logDebug(fmt::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        if (m_debug) {
            fmt::println(fmt, std::forward<Args>(args)...);
        }
    }
    
    void setDebug(bool flag) noexcept { m_debug = flag; }
    [[nodiscard]] bool debug() const noexcept { return m_debug; }

    void saveCheckpoint();
    void step();

    [[nodiscard]] static std::unique_ptr<Simulation> fromXML(pugi::xml_node node);
    [[nodiscard]] static std::unique_ptr<Simulation> fromCheckpoint(const fs::path& path);

private:
    void configureAgents(pugi::xml_node node);
    void configureLogging(pugi::xml_node node);
    void deliverMessage(Message::Ptr msg);
    void start();
    void stop();

    void updateTime(Timestamp newTime)
    {
        if (newTime == m_time.current) [[unlikely]] return;
        Timestamp oldTime = std::exchange(m_time.current, newTime);
        m_signals.time({.begin = oldTime + 1, .end = newTime});
    }

    mutable MessageQueue m_messageQueue;
    taosim::simulation::SimulationState m_state{taosim::simulation::SimulationState::INACTIVE};
    struct { Timestamp start, duration, step, current; } m_time;
    mutable taosim::simulation::SimulationSignals m_signals;
    std::unique_ptr<LocalAgentManager> m_localAgentManager;
    MultiBookExchangeAgent* m_exchange{};
    DistributedProxyAgent* m_proxy;
    mutable std::mt19937 m_rng;
    std::string m_id;
    std::string m_config;
    bool m_debug = false;
    fs::path m_logDir;
    taosim::simulation::SimulationConfig m_config2;
    uint32_t m_blockIdx{};
    fs::path m_baseLogDir;
    Timestamp m_logWindow{};

    friend class LocalAgentManager;
    friend class MultiBookExchangeAgent;
};

//-------------------------------------------------------------------------
