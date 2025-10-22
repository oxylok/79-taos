/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "taosim/serialization/msgpack_util.hpp"
#include "taosim/simulation/SimulationManager.hpp"
#include "taosim/simulation/serialization/ValidatorRequest.hpp"
#include "taosim/simulation/serialization/ValidatorResponse.hpp"
#include "taosim/simulation/replay_helpers.hpp"
#include "taosim/simulation/util.hpp"
#include "taosim/message/MultiBookMessagePayloads.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>
#include <msgpack.hpp>

#include <barrier>
#include <latch>
#include <ranges>
#include <source_location>
#include <thread>

//-------------------------------------------------------------------------

namespace taosim::simulation
{

//-------------------------------------------------------------------------

void SimulationManager::runSimulations()
{
    std::barrier barrier{
        m_blockInfo.count,
        [&] {
            if (m_useMessagePack) {
                publishStateMessagePack();
            } else {
                publishState();
            }
            m_stepSignal();
        }};
    std::latch latch{m_blockInfo.count};

    publishStartInfo();

    for (auto& simulation : m_simulations) {
        boost::asio::post(
            *m_threadPool,
            [&] {
                simulation->simulate(barrier);
                latch.count_down();
            });
    }
    latch.wait();

    publishEndInfo();
}

//-------------------------------------------------------------------------

void SimulationManager::runReplay(
    const fs::path& replayDir, BookId bookId, std::span<const std::string> replacedAgents)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    for (const auto& simulation : m_simulations) {
        simulation->replacedAgents() = {replacedAgents.begin(), replacedAgents.end()};
    }

    auto it = ranges::find_if(
        m_simulations,
        [&](const auto& sim) { return sim->blockIdx() == bookId / m_blockInfo.dimension; });
    if (it == m_simulations.end()) {
        throw std::runtime_error{fmt::format(
            "{}: Could not find simulation matching bookId {} within {}; "
            "blockInfo was {{.count = {}, .dimension = {}}}",
            ctx, bookId, replayDir.c_str(), m_blockInfo.count, m_blockInfo.dimension)};
    }
    const auto& simulation = *it;

    auto bookIdToReplayLogPaths = [&] {
        std::vector<fs::path> replayLogPaths;
        const std::regex pat{R"(^Replay-\d+\.\d{8}-\d{8}\.log$)"};
        for (const auto& entry : fs::directory_iterator(replayDir)) {
            const auto path = entry.path();
            if (entry.is_regular_file() && std::regex_match(path.filename().string(), pat)) {
                replayLogPaths.push_back(path);
            }
        }
        auto parseBookId = [](auto&& path) {
            const auto filenameStr = path.filename().string();
            const std::regex pat{R"(^Replay-(\d+).*)"};
            if (std::smatch match; std::regex_match(filenameStr, match, pat)) {
                return std::stoul(match[1]);
            }
            return std::numeric_limits<uint64_t>::max();
        };
        std::vector<std::vector<fs::path>> res;
        res.resize(m_blockInfo.count * m_blockInfo.dimension);
        for (auto&& path : replayLogPaths) {
            res.at(parseBookId(path)).push_back(path);
        }
        for (auto&& paths : res) {
            ranges::sort(
                paths,
                [&](auto&& lhs, auto&& rhs) {
                    const auto lhsStr = lhs.filename().string();
                    const auto rhsStr = rhs.filename().string();
                    const std::regex pat{R"(^Replay-(\d+)\.(\d{8})-(\d{8})\.log$)"};
                    std::smatch matchLhs;
                    std::regex_search(lhsStr, matchLhs, pat);
                    std::smatch matchRhs;
                    std::regex_search(rhsStr, matchRhs, pat);
                    return std::stoi(matchLhs[2]) < std::stoi(matchRhs[2]);
                });
        }
        return res;
    }();

    [&] {
        std::vector<fs::path> replayBalancesPaths;
        const std::regex pat{R"(^Replay-Balances-(\d+)-(\d+)\.json$)"};
        for (const auto& entry : fs::directory_iterator(replayDir)) {
            const auto path = entry.path();
            if (entry.is_regular_file() && std::regex_match(path.filename().string(), pat)) {
                replayBalancesPaths.push_back(path);
            }
        }
        ranges::sort(
            replayBalancesPaths,
            [&](auto&& lhs, auto&& rhs) {
                const auto lhsStr = lhs.filename().string();
                std::smatch matchLhs;
                std::regex_search(lhsStr, matchLhs, pat);
                const auto rhsStr = rhs.filename().string();
                std::smatch matchRhs;
                std::regex_search(rhsStr, matchRhs, pat);
                return std::stoi(matchLhs[1]) < std::stoi(matchRhs[1]);
            });
        for (const auto& [simulation, path] : views::zip(m_simulations, replayBalancesPaths)) {
            rapidjson::Document balancesJson = json::loadJson(path);
            for (const auto& member : balancesJson.GetObject()) {
                const auto name = member.name.GetString();
                const AgentId agentId = std::stoi(name);
                BookId bookId{};
                for (const auto& balsJson : balancesJson[name].GetArray()) {
                    auto& bals = simulation->exchange()->accounts().at(agentId).at(bookId);
                    bals.base = taosim::accounting::Balance(
                        taosim::json::getDecimal(balsJson["base"]),
                        "",
                        bals.m_roundParams.baseDecimals);
                    bals.quote = taosim::accounting::Balance(
                        taosim::json::getDecimal(balsJson["quote"]),
                        "",
                        bals.m_roundParams.quoteDecimals);
                    ++bookId;
                }
            }
        }
    }();

    for (const auto& replayLogFile : bookIdToReplayLogPaths.at(bookId)) {
        std::ifstream ifs{replayLogFile, std::ios::in};
        std::vector<std::string> lines;
        std::string buf;
        std::getline(ifs, buf);
        size_t lineCounter{1};
        while (true) {
            lines.clear();
            while (std::getline(ifs, buf)) {
                lines.push_back(buf);
                ++lineCounter;
            }
            if (lines.empty()) break;
            for (const auto& line : lines) {
                Message::Ptr msg = replay_helpers::createMessageFromLogFileEntry(line, lineCounter);
                if (simulation->isReplacedAgent(msg->source)) continue;
                simulation->queueMessage(msg);
                simulation->time().duration = msg->arrival;
            }
        }
    }

    simulation->simulate();
}

//-------------------------------------------------------------------------

void SimulationManager::runReplayAdvanced(
    const fs::path& replayDir, std::span<const std::string> replacedAgents)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    for (const auto& simulation : m_simulations) {
        simulation->replacedAgents() = {replacedAgents.begin(), replacedAgents.end()};
    }

    auto bookIdToReplayLogPaths = [&] {
        std::vector<fs::path> replayLogPaths;
        const std::regex pat{R"(^Replay-\d+\.\d{8}-\d{8}\.log$)"};
        for (const auto& entry : fs::directory_iterator(replayDir)) {
            const auto path = entry.path();
            if (entry.is_regular_file() && std::regex_match(path.filename().string(), pat)) {
                replayLogPaths.push_back(path);
            }
        }
        auto parseBookId = [](auto&& path) {
            const auto filenameStr = path.filename().string();
            const std::regex pat{R"(^Replay-(\d+).*)"};
            if (std::smatch match; std::regex_match(filenameStr, match, pat)) {
                return std::stoul(match[1]);
            }
            return std::numeric_limits<uint64_t>::max();
        };
        std::vector<std::vector<fs::path>> res;
        res.resize(m_blockInfo.count * m_blockInfo.dimension);
        for (auto&& path : replayLogPaths) {
            res.at(parseBookId(path)).push_back(path);
        }
        for (auto&& paths : res) {
            ranges::sort(
                paths,
                [&](auto&& lhs, auto&& rhs) {
                    const auto lhsStr = lhs.filename().string();
                    const auto rhsStr = rhs.filename().string();
                    const std::regex pat{R"(^Replay-(\d+)\.(\d{8})-(\d{8})\.log$)"};
                    std::smatch matchLhs;
                    std::regex_search(lhsStr, matchLhs, pat);
                    std::smatch matchRhs;
                    std::regex_search(rhsStr, matchRhs, pat);
                    return std::stoi(matchLhs[2]) < std::stoi(matchRhs[2]);
                });
        }
        return res;
    }();

    [&] {
        std::vector<fs::path> replayBalancesPaths;
        const std::regex pat{R"(^Replay-Balances-(\d+)-(\d+)\.json$)"};
        for (const auto& entry : fs::directory_iterator(replayDir)) {
            const auto path = entry.path();
            if (entry.is_regular_file() && std::regex_match(path.filename().string(), pat)) {
                replayBalancesPaths.push_back(path);
            }
        }
        ranges::sort(
            replayBalancesPaths,
            [&](auto&& lhs, auto&& rhs) {
                const auto lhsStr = lhs.filename().string();
                std::smatch matchLhs;
                std::regex_search(lhsStr, matchLhs, pat);
                const auto rhsStr = rhs.filename().string();
                std::smatch matchRhs;
                std::regex_search(rhsStr, matchRhs, pat);
                return std::stoi(matchLhs[1]) < std::stoi(matchRhs[1]);
            });
        ranges::sort(replayBalancesPaths);
        for (const auto& [simulation, path] : views::zip(m_simulations, replayBalancesPaths)) {
            rapidjson::Document balancesJson = json::loadJson(path);
            for (const auto& member : balancesJson.GetObject()) {
                const auto name = member.name.GetString();
                const AgentId agentId = std::stoi(name);
                BookId bookId{};
                for (const auto& balsJson : balancesJson[name].GetArray()) {
                    auto& bals = simulation->exchange()->accounts().at(agentId).at(bookId);
                    bals.base = taosim::accounting::Balance(
                        taosim::json::getDecimal(balsJson["base"]),
                        "",
                        bals.m_roundParams.baseDecimals);
                    bals.quote = taosim::accounting::Balance(
                        taosim::json::getDecimal(balsJson["quote"]),
                        "",
                        bals.m_roundParams.quoteDecimals);
                    ++bookId;
                }
            }
        }
    }();

    struct BookReplayFilesState
    {
        std::vector<std::ifstream> fileStreams;
        std::vector<size_t> lineCounters;
        size_t currentFileIdx{};

        [[nodiscard]] auto& currentFile()
        {
            return fileStreams.at(std::min(currentFileIdx, fileStreams.size() - 1));
        }
    
        [[nodiscard]] size_t currentLineCounter() const noexcept
        {
            return lineCounters.at(std::min(currentFileIdx, lineCounters.size() - 1));
        }
    
        [[nodiscard]] bool done() const noexcept { return currentFileIdx >= fileStreams.size(); }
    
        bool getLine(std::string& buf)
        {
            if (done()) return false;
            if (!std::getline(currentFile(), buf)) {
                ++currentFileIdx;
                if (done()) return false;
                std::getline(currentFile(), buf);
            }
            ++lineCounters.at(currentFileIdx);
            return true;
        }
    };

    auto bookIdToReplayFilesState = bookIdToReplayLogPaths
        | views::transform([](auto&& replayLogPaths) {
            return BookReplayFilesState{
                .fileStreams = replayLogPaths
                    | views::transform([](auto&& path) {
                        std::ifstream ifs{path, std::ios::in};
                        std::string sink;
                        std::getline(ifs, sink);  // Discard header.
                        return ifs;
                    })
                    | ranges::to<std::vector>,
                .lineCounters = std::vector<size_t>(replayLogPaths.size(), 1)
            };
        })
        | ranges::to<std::vector>;

    m_stepSignal.connect([&] {
        const auto& representativeSimulation = m_simulations.front();
        const auto& time = representativeSimulation->time();
        const auto cutoff = time.current + time.step;
        for (BookId bookId{}; bookId < bookIdToReplayFilesState.size(); ++bookId) {
            const auto& simulation = m_simulations.at(bookId / m_blockInfo.dimension);
            auto& state = bookIdToReplayFilesState.at(bookId);
            if (state.done()) continue;
            std::string lineBuf;
            while (true) {
                if (!state.getLine(lineBuf)) break;
                const auto msg = replay_helpers::createMessageFromLogFileEntry(
                    lineBuf, state.currentLineCounter() - 1);
                if (simulation->isReplacedAgent(msg->source)) continue;
                simulation->queueMessage(msg);
                simulation->time().duration = msg->arrival;
                if (msg->arrival >= cutoff) break;
            }
        }
    });

    runSimulations();
}

//-------------------------------------------------------------------------

void SimulationManager::publishStartInfo()
{
    if (!online()) return;

    rapidjson::Document json = [this] {
        const auto& representativeSimulation = m_simulations.front();
        const auto msg = Message::create(
            representativeSimulation->time().start,
            0,
            "SIMULATION",
            "*",
            "EVENT_SIMULATION_START",
            MessagePayload::create<StartSimulationPayload>(m_logDir.generic_string()));
        rapidjson::Document json{rapidjson::kObjectType};
        auto& allocator = json.GetAllocator();
        json.AddMember(
            "messages",
            [&] {
                rapidjson::Document messagesJson{rapidjson::kArrayType, &allocator};
                rapidjson::Document msgJson{&allocator};
                msg->jsonSerialize(msgJson);
                messagesJson.PushBack(msgJson, allocator);
                return messagesJson;
            }().Move(),
            allocator);
        return json;
    }();
    rapidjson::Document res;

    net::io_context ctx;
    net::co_spawn(
        ctx, asyncSendOverNetwork(json, m_netInfo.generalMsgEndpoint, res), net::detached);
    ctx.run();
}

//-------------------------------------------------------------------------

void SimulationManager::publishEndInfo()
{
    if (!online()) return;

    rapidjson::Document json = [this] {
        const auto& representativeSimulation = m_simulations.front();
        const auto msg = Message::create(
            representativeSimulation->time().start,
            0,
            "SIMULATION",
            "*",
            "EVENT_SIMULATION_END",
            MessagePayload::create<EmptyPayload>());
        rapidjson::Document json{rapidjson::kObjectType};
        auto& allocator = json.GetAllocator();
        json.AddMember(
            "messages",
            [&] {
                rapidjson::Document messagesJson{rapidjson::kArrayType, &allocator};
                rapidjson::Document msgJson{&allocator};
                msg->jsonSerialize(msgJson);
                messagesJson.PushBack(msgJson, allocator);
                return messagesJson;
            }().Move(),
            allocator);
        return json;
    }();
    rapidjson::Document res;

    net::io_context ctx;
    net::co_spawn(
        ctx, asyncSendOverNetwork(json, m_netInfo.generalMsgEndpoint, res), net::detached);
    ctx.run();
}

//-------------------------------------------------------------------------

void SimulationManager::publishState()
{
    const auto& representativeSimulation = m_simulations.front();

    if (representativeSimulation->currentTimestamp() < m_gracePeriod || !online()) return;

    rapidjson::Document stateJson = makeStateJson();
    rapidjson::Document resJson;

    net::io_context ctx;
    net::co_spawn(
        ctx, asyncSendOverNetwork(stateJson, m_netInfo.bookStateEndpoint, resJson), net::detached);
    ctx.run();

    const Timestamp now = representativeSimulation->currentTimestamp();

    for (const auto& response : resJson["responses"].GetArray()) {
        const auto [msg, blockIdx] = decanonize(
            Message::fromJsonResponse(response, now, representativeSimulation->proxy()->name()),
            m_blockInfo.dimension);
        if (!blockIdx) {
            for (const auto& simulation : m_simulations) {
                simulation->queueMessage(msg);
            }
            continue;
        }
        m_simulations.at(*blockIdx)->queueMessage(msg);
    }
}

//-------------------------------------------------------------------------

void SimulationManager::publishStateMessagePack()
{
    static constexpr auto ctx = std::source_location::current().function_name();

    const auto& representativeSimulation = m_simulations.front();
    const auto now = representativeSimulation->currentTimestamp();

    if (now < m_gracePeriod || !online()) return;

    taosim::serialization::HumanReadableStream stream{1uz << 27};
    const serialization::ValidatorRequest req{.mngr = this};
    msgpack::pack(stream, req);

    bipc::shared_memory_object shmReq{
        bipc::open_or_create,
        s_statePublishShmName.data(),
        bipc::read_write
    };
    shmReq.truncate(stream.size());
    bipc::mapped_region reqRegion{shmReq, bipc::read_write};
    std::memcpy(reqRegion.get_address(), stream.data(), stream.size());

    retryMessagePack:

    const size_t packedSize = stream.size();
    m_validatorReqMessageQueue->flush();
    const bool mqSendSuccess = m_validatorReqMessageQueue->send(
        std::span<const char>{std::bit_cast<const char*>(&packedSize), sizeof(packedSize)});
    if (!mqSendSuccess) {
        fmt::println("Sending to /{} timed out, flushing and retrying...", s_validatorReqMessageQueueName);
        goto retryMessagePack;
    }

    size_t resByteSize;
    const bool mqRecvSuccess = m_validatorResMessageQueue->receive(
        std::span<char>{std::bit_cast<char*>(&resByteSize), sizeof(resByteSize)}) != -1;
    if (!mqRecvSuccess) {
        fmt::println("Receive from /{} timed out, flushing and retrying...", s_validatorResMessageQueueName);
        goto retryMessagePack;
    }

    bipc::shared_memory_object shmRes{
        bipc::open_only,
        s_remoteResponsesShmName.data(),
        bipc::read_write
    };
    bipc::mapped_region resRegion{shmRes, bipc::read_write};

    msgpack::object_handle oh =
        msgpack::unpack(std::bit_cast<const char*>(resRegion.get_address()), resByteSize);
    msgpack::object obj = oh.get();

    auto unpackResponse = [&](const msgpack::object& o) {
        if (o.type != msgpack::type::MAP) {
            throw taosim::serialization::MsgPackError{};
        }
        struct Response
        {
            std::optional<AgentId> agentId{};
            std::optional<Timestamp> delay{};
            std::string type{};
            MessagePayload::Ptr payload{};
        };
        Response res;
        for (const auto& [k, val] : o.via.map) {
            auto key = k.as<std::string_view>();
            if (key == "agentId") {
                res.agentId = std::make_optional(val.as<AgentId>());
            }
            else if (key == "delay") {
                res.delay = std::make_optional(val.as<Timestamp>());
            }
            else if (key == "type") {
                res.type = val.as<std::string>();
            }
        }
        if (!res.agentId) {
            throw taosim::serialization::MsgPackError{};
        }
        if (!res.delay) {
            throw taosim::serialization::MsgPackError{};
        }
        if (res.type.empty()) {
            throw taosim::serialization::MsgPackError{};
        }
        for (const auto& [k, val] : o.via.map) {
            auto key = k.as<std::string_view>();
            if (key == "payload") {
                res.payload = PayloadFactory::createFromMessagePack(val, res.type);
                break;
            }
        }
        if (res.payload == nullptr) {
            throw taosim::serialization::MsgPackError{};
        }
        auto msg = std::make_shared<Message>();
        msg->occurrence = now;
        msg->arrival = now + *res.delay;
        msg->source = representativeSimulation->proxy()->name();
        msg->targets = {representativeSimulation->exchange()->name()};
        msg->type = fmt::format("{}_{}", "DISTRIBUTED", res.type);
        msg->payload =
            MessagePayload::create<DistributedAgentResponsePayload>(*res.agentId, res.payload);
        return msg;
    };

    if (obj.type != msgpack::type::MAP || obj.via.map.size != 1) {
        return;
    }
    const auto& val = obj.via.map.ptr[0].val;
    if (val.type != msgpack::type::ARRAY || val.via.array.size == 0) {
        return;
    }
    std::vector<Message::Ptr> unpackedResponses;
    size_t responseIdx{};
    size_t errorCounter{};
    std::map<size_t, std::string> responseIdxToError;
    for (const auto& response : val.via.array) {
        auto handleError = [&](auto&& e) {
            responseIdxToError[responseIdx] = e.what();
            ++errorCounter;
        };
        try {
            unpackedResponses.push_back(unpackResponse(response));
        } catch (const std::exception& e) {
            handleError(e);
        }
        ++responseIdx;
    }
    if (errorCounter > 0) {
        rapidjson::Document json{rapidjson::kObjectType};
        auto& allocator = json.GetAllocator();
        const auto errorRatio = static_cast<float>(errorCounter) / val.via.array.size;
        json.AddMember(
            "messages",
            [&] {
                rapidjson::Value messagesJson{rapidjson::kArrayType};
                rapidjson::Value messageJson{rapidjson::kObjectType};
                messageJson.AddMember(
                    "type", rapidjson::Value{"RESPONSES_ERROR_REPORT", allocator}, allocator);
                messageJson.AddMember("timestamp", rapidjson::Value{now}, allocator);
                messageJson.AddMember("errorRatio", rapidjson::Value{errorRatio}, allocator);
                for (const auto& [key, val] : responseIdxToError) {
                    messageJson.AddMember(
                        rapidjson::Value{std::to_string(key).c_str(), allocator},
                        rapidjson::Value{val.c_str(), allocator},
                        allocator);
                }
                messagesJson.PushBack(messageJson, allocator);
                return messagesJson;
            }().Move(),
            allocator);
        rapidjson::Document res;
        net::io_context io;
        net::co_spawn(
            io, asyncSendOverNetwork(json, m_netInfo.generalMsgEndpoint, res), net::detached);
        io.run();
        if (res.HasMember("continue") && res["continue"].GetBool() == false) {
            throw std::runtime_error{fmt::format(
                "{}: Teardown requested by validator; latest error rate: {}; details: {{{}}}",
                ctx,
                errorRatio,
                fmt::join(
                    responseIdxToError
                    | views::transform([](const auto& pair) {
                        return fmt::format("{} -> {}", pair.first, pair.second);
                    }),
                    ", "
                ))};
        }
    }
    for (const auto& response : unpackedResponses) {
        const auto [msg, blockIdx] = decanonize(response, m_blockInfo.dimension);
        if (!blockIdx) {
            for (const auto& simulation : m_simulations) {
                simulation->queueMessage(msg);
            }
            continue;
        }
        m_simulations.at(*blockIdx)->queueMessage(msg);
    }
}

//-------------------------------------------------------------------------

rapidjson::Document SimulationManager::makeStateJson() const
{
    const auto& representativeSimulation = m_simulations.front();

    const auto bookStatePublishMsg = Message::create(
        representativeSimulation->currentTimestamp(),
        0,
        representativeSimulation->exchange()->name(),
        representativeSimulation->proxy()->name(),
        "MULTIBOOK_STATE_PUBLISH",
        MessagePayload::create<BookStateMessagePayload>(makeCollectiveBookStateJson()));

    rapidjson::Document json;
    auto& allocator = json.GetAllocator();
    bookStatePublishMsg->jsonSerialize(json);
    json["payload"].AddMember(
        "notices",
        [&] {
            rapidjson::Document noticesJson{rapidjson::kArrayType, &allocator};
            std::unordered_map<decltype(Message::type), uint32_t> msgTypeToCount{
                { "RESPONSE_DISTRIBUTED_RESET_AGENT", 0 },
                { "ERROR_RESPONSE_DISTRIBUTED_RESET_AGENT", 0 }
            };
            auto checkGlobalDuplicate = [&](Message::Ptr msg) -> bool {
                const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
                if (payload == nullptr) return false;
                auto relevantPayload = [&] {
                    const auto pld = payload->payload;
                    return std::dynamic_pointer_cast<ResetAgentsResponsePayload>(pld) != nullptr
                        || std::dynamic_pointer_cast<ResetAgentsErrorResponsePayload>(pld) != nullptr;
                };
                if (!relevantPayload()) return true;
                auto it = msgTypeToCount.find(msg->type);
                if (it == msgTypeToCount.end()) return true;
                if (it->second > 0) return false;
                it->second++;
                return true;
            };
            for (const auto& [blockIdx, simulation] : views::enumerate(m_simulations)) {
                for (const auto msg : simulation->proxy()->messages()) {
                    if (!checkGlobalDuplicate(msg)) continue;
                    canonize(msg, blockIdx, m_blockInfo.dimension);
                    rapidjson::Document msgJson{&allocator};
                    msg->jsonSerialize(msgJson);
                    noticesJson.PushBack(msgJson, allocator);
                }
                simulation->proxy()->clearMessages();
            }
            return noticesJson;
        }().Move(),
        allocator);
    
    return json;
}

//-------------------------------------------------------------------------

rapidjson::Document SimulationManager::makeCollectiveBookStateJson() const
{
    auto serialize = [this](rapidjson::Document& json) {
        auto& allocator = json.GetAllocator();
        // Log directory.
        json.AddMember("logDir", rapidjson::Value{m_logDir.c_str(), allocator}, allocator);
        // Books.
        auto serializeBooks = [this](rapidjson::Document& json) {
            json.SetArray();
            auto& allocator = json.GetAllocator();
            for (const auto& [blockIdx, simulation] : views::enumerate(m_simulations)) {
                const auto exchange = simulation->exchange();
                for (const auto book : exchange->books()) {
                    json.PushBack(
                        [&] {
                            rapidjson::Document bookJson{rapidjson::kObjectType, &allocator};
                            const BookId bookIdCanon = blockIdx * m_blockInfo.dimension + book->id();
                            bookJson.AddMember("bookId", rapidjson::Value{bookIdCanon}, allocator);
                            exchange->L3Record().at(book->id()).jsonSerialize(bookJson, "record");
                            rapidjson::Document bidAskJson{&allocator};
                            book->jsonSerialize(bidAskJson);
                            bookJson.AddMember("bid", bidAskJson["bid"], allocator);
                            bookJson.AddMember("ask", bidAskJson["ask"], allocator);
                            return bookJson;
                        }().Move(),
                        allocator);
                }
            }
        };
        json::serializeHelper(json, "books", serializeBooks);
        // Accounts.
        auto serializeAccounts = [this](rapidjson::Document& json) {
            json.SetObject();
            auto& allocator = json.GetAllocator();
            const auto& representativeSimulation = m_simulations.front();
            for (AgentId agentId : views::keys(representativeSimulation->exchange()->accounts())) {
                if (agentId < 0) continue;
                const auto agentIdStr = std::to_string(agentId);
                const char* agentIdCStr = agentIdStr.c_str();
                json.AddMember(
                    rapidjson::Value{agentIdCStr, allocator},
                    rapidjson::Document{rapidjson::kObjectType, &allocator}.Move(),
                    allocator);
                json[agentIdCStr].AddMember("agentId", rapidjson::Value{agentId}, allocator);
                json[agentIdCStr].AddMember("holdings", rapidjson::Document{rapidjson::kArrayType, &allocator}, allocator);
                json[agentIdCStr].AddMember("orders", rapidjson::Document{rapidjson::kArrayType, &allocator}, allocator);
                json[agentIdCStr].AddMember("loans", rapidjson::Document{rapidjson::kArrayType, &allocator}, allocator);
                rapidjson::Document feesJson{rapidjson::kObjectType, &allocator};
                for (const auto& [blockIdx, simulation] : views::enumerate(m_simulations)) {
                    const auto exchange = simulation->exchange();
                    const auto books = exchange->books();
                    const auto& account = exchange->accounts().at(agentId);
                    const auto feePolicy = exchange->clearingManager().feePolicy();
                    for (const auto book : books) {
                        const BookId bookIdCanon = blockIdx * m_blockInfo.dimension + book->id();
                        json[agentIdCStr]["orders"].PushBack(
                            rapidjson::Document{rapidjson::kArrayType, &allocator}.Move(), allocator);
                        json[agentIdCStr]["holdings"].PushBack(
                            [&] {
                                rapidjson::Document holdingsJson{&allocator};
                                account.at(book->id()).jsonSerialize(holdingsJson);
                                return holdingsJson;
                            }().Move(),
                            allocator);
                        json[agentIdCStr]["loans"].PushBack(
                            [&] {
                                rapidjson::Document loansObjectJson{rapidjson::kObjectType, &allocator};
                                for (const auto& [id, loan] : account.at(book->id()).m_loans) {
                                    rapidjson::Document loanJson{rapidjson::kObjectType, &allocator};
                                    loanJson.AddMember("id", rapidjson::Value{id}, allocator);
                                    loanJson.AddMember("amount", rapidjson::Value{taosim::util::decimal2double(loan.amount())}, allocator);
                                    loanJson.AddMember("currency", rapidjson::Value{
                                        std::to_underlying(loan.direction() == OrderDirection::BUY ? Currency::QUOTE : Currency::BASE)
                                    }, allocator);
                                    loanJson.AddMember("baseCollateral", rapidjson::Value{taosim::util::decimal2double(loan.collateral().base())}, allocator);
                                    loanJson.AddMember("quoteCollateral", rapidjson::Value{taosim::util::decimal2double(loan.collateral().quote())}, allocator);
                                    const auto idStr = std::to_string(id);
                                    const char* idCStr = idStr.c_str();
                                    loansObjectJson.AddMember(rapidjson::Value{idCStr, allocator}, loanJson, allocator);
                                }
                                return loansObjectJson;
                            }().Move(),
                            allocator);
                        json::serializeHelper(
                            feesJson,
                            std::to_string(bookIdCanon).c_str(),
                            [&](rapidjson::Document& feeJson) {
                                feeJson.SetObject();
                                auto& allocator = feeJson.GetAllocator();
                                feeJson.AddMember(
                                    "volume",
                                    rapidjson::Value{util::decimal2double(
                                        feePolicy->agentVolume(book->id(), agentId))},
                                    allocator);
                                const auto rates = feePolicy->getRates(book->id(), agentId);
                                feeJson.AddMember(
                                    "makerFeeRate",
                                    rapidjson::Value{util::decimal2double(rates.maker)},
                                    allocator);
                                feeJson.AddMember(
                                    "takerFeeRate",
                                    rapidjson::Value{util::decimal2double(rates.taker)},
                                    allocator);
                            });
                    }
                }
                json[agentIdCStr].AddMember("fees", feesJson, allocator);
            }
            for (const auto& [blockIdx, simulation] : views::enumerate(m_simulations)) {
                const auto books = simulation->exchange()->books();
                for (const auto book : books) {
                    const BookId bookIdCanon = blockIdx * m_blockInfo.dimension + book->id();
                    auto serializeSide = [&](OrderDirection side) {
                        const auto& levels =
                            side == OrderDirection::BUY ? book->buyQueue() : book->sellQueue();
                        for (const auto& level : levels) {
                            for (const auto tick : level) {
                                const auto [agentId, clientOrderId] =
                                    books[book->id()]->orderClientContext(tick->id());
                                if (agentId < 0) continue;
                                const auto agentIdStr = std::to_string(agentId);
                                const char* agentIdCStr = agentIdStr.c_str();
                                rapidjson::Document orderJson{&allocator};
                                tick->jsonSerialize(orderJson);
                                json::setOptionalMember(orderJson, "clientOrderId", clientOrderId);
                                json[agentIdCStr]["orders"][bookIdCanon].PushBack(orderJson, allocator);
                            }
                        }
                    };
                    serializeSide(OrderDirection::BUY);
                    serializeSide(OrderDirection::SELL);
                }
            }
        };
        json::serializeHelper(json, "accounts", serializeAccounts);
    };

    rapidjson::Document json{rapidjson::kObjectType};
    serialize(json);
    return json;
}

//-------------------------------------------------------------------------

std::unique_ptr<SimulationManager> SimulationManager::fromConfig(const fs::path& path)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());
    fmt::println(" - '{}' loaded successfully", path.c_str());
    pugi::xml_node node = doc.child("Simulation");

    auto mngr = std::make_unique<SimulationManager>();

    mngr->m_blockInfo = [&] -> SimulationBlockInfo {
        static constexpr const char* attrName = "blockCount";
        pugi::xml_attribute attr = node.attribute(attrName);
        const auto threadCount = [&] {
            const auto threadCount = attr.as_uint(1);
            if (threadCount > std::thread::hardware_concurrency()) {
                throw std::runtime_error{fmt::format(
                    "{}: requested thread count ({}) exceeds count available ({})",
                    ctx, threadCount, std::thread::hardware_concurrency()
                )};
            }
            return threadCount;
        }();
        const auto booksNode = node.child("Agents").child("MultiBookExchangeAgent").child("Books");
        if (!booksNode) {
            throw std::runtime_error{fmt::format(
                "{}: missing node 'Agents/MultiBookExchangeAgent/Books'",
                ctx
            )};
        }
        return {
            .count = threadCount,
            .dimension = booksNode.attribute("instanceCount").as_uint(1)
        };
    }();
    mngr->m_threadPool = std::make_unique<boost::asio::thread_pool>(mngr->m_blockInfo.count);
    boost::asio::signal_set{mngr->m_io, SIGINT, SIGTERM}.async_wait(
        [&](boost::system::error_code, int) {
            mngr->m_threadPool->stop();
            mngr->m_io.stop();
        });

    mngr->setupLogDir(node);
    mngr->m_simulations =
        views::iota(0u, mngr->m_blockInfo.count)
        | views::transform([&](auto blockIdx) {
            auto simulation = std::make_unique<Simulation>(blockIdx, mngr->m_logDir);
            simulation->configure(node);
            return simulation;
        })
        | ranges::to<std::vector>;

    mngr->m_gracePeriod = node.child("Agents")
        .child("MultiBookExchangeAgent")
        .attribute("gracePeriod")
        .as_ullong();

    mngr->m_netInfo = {
        .host = node.attribute("host").as_string(),
        .port = node.attribute("port").as_string(),
        .bookStateEndpoint = node.attribute("bookStateEndpoint").as_string("/"),
        .generalMsgEndpoint = node.attribute("generalMsgEndpoint").as_string("/"),
        .resolveTimeout = node.attribute("resolveTimeout").as_llong(1),
        .connectTimeout = node.attribute("connectTimeout").as_llong(3),
        .writeTimeout = node.attribute("writeTimeout").as_llong(15),
        .readTimeout = node.attribute("readTimeout").as_llong(60)
    };

    mngr->m_stepSignal.connect([&] {
        for (auto& simulation : mngr->m_simulations) {
            simulation->exchange()->L3Record().clear();
        }
    });

    if (node.attribute("traceTime").as_bool()) {
        mngr->m_stepSignal.connect([&] {
            const auto& representativeSimulation = mngr->m_simulations.front();
            uint64_t total, seconds, hours, minutes, nanos;
            total = representativeSimulation->time().current / 1'000'000'000;
            minutes = total / 60;
            seconds = total % 60;
            hours = minutes / 60;
            minutes = minutes % 60;
            nanos = representativeSimulation->time().current % 1'000'000'000;
            fmt::println("TIME : {:02d}:{:02d}:{:02d}.{:09d}", hours, minutes, seconds, nanos); 
        });
    }

    mngr->m_validatorReqMessageQueue = std::make_unique<ipc::PosixMessageQueue>(
        ipc::PosixMessageQueueDesc{.name = s_validatorReqMessageQueueName.data()});
    mngr->m_validatorResMessageQueue = std::make_unique<ipc::PosixMessageQueue>(
        ipc::PosixMessageQueueDesc{.name = s_validatorResMessageQueueName.data()});

    mngr->m_useMessagePack = node.attribute("useMessagePack").as_bool();

    return mngr;
}

//-------------------------------------------------------------------------

std::unique_ptr<SimulationManager> SimulationManager::fromReplay(const fs::path& replayDir)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    pugi::xml_document doc;
    const fs::path configPath = replayDir / "config.xml";;
    pugi::xml_parse_result result = doc.load_file(configPath.c_str());
    fmt::println(" - '{}' loaded successfully", configPath.c_str());
    pugi::xml_node node = doc.child("Simulation");
    node.attribute("id").set_value(
        fmt::format(
            "{}-replay",
            [&] -> fs::path {
                const auto replayDirStr = replayDir.string();
                if (replayDirStr.ends_with(fs::path::preferred_separator)) {
                    return replayDirStr.substr(0, replayDirStr.size() - 1);
                }
                return replayDir;
            }().filename().c_str()).c_str());
    
    static constexpr const char* replayNodeName = "Replay";
    auto replayLogNode = node.child("Agents")
        .child("MultiBookExchangeAgent")
        .child("Logging")
        .child(replayNodeName);
    if (replayLogNode) {
        replayLogNode.parent().remove_child(replayNodeName);
    }

    auto mngr = std::make_unique<SimulationManager>();

    mngr->m_blockInfo = [&] -> SimulationBlockInfo {
        static constexpr const char* attrName = "blockCount";
        pugi::xml_attribute attr = node.attribute(attrName);
        const auto threadCount = [&] {
            const auto threadCount = attr.as_uint(1);
            if (threadCount > std::thread::hardware_concurrency()) {
                throw std::runtime_error{fmt::format(
                    "{}: requested thread count ({}) exceeds count available ({})",
                    ctx, threadCount, std::thread::hardware_concurrency()
                )};
            }
            return threadCount;
        }();
        const auto booksNode = node.child("Agents").child("MultiBookExchangeAgent").child("Books");
        if (!booksNode) {
            throw std::runtime_error{fmt::format(
                "{}: missing node 'Agents/MultiBookExchangeAgent/Books'",
                ctx
            )};
        }
        return {
            .count = threadCount,
            .dimension = booksNode.attribute("instanceCount").as_uint(1)
        };
    }();
    mngr->m_threadPool = std::make_unique<boost::asio::thread_pool>(mngr->m_blockInfo.count);
    boost::asio::signal_set{mngr->m_io, SIGINT, SIGTERM}.async_wait(
        [&](boost::system::error_code, int) {
            mngr->m_threadPool->stop();
            mngr->m_io.stop();
        });

    mngr->setupLogDir(node);
    mngr->m_simulations =
        views::iota(0u, mngr->m_blockInfo.count)
        | views::transform([&](auto blockIdx) {
            auto simulation = std::make_unique<Simulation>(blockIdx, mngr->m_logDir);
            simulation->configure(node);
            simulation->exchange()->replayMode() = true;
            return simulation;
        })
        | ranges::to<std::vector>;

    mngr->m_gracePeriod = node.child("Agents")
        .child("MultiBookExchangeAgent")
        .attribute("gracePeriod")
        .as_ullong();

    if (node.attribute("traceTime").as_bool()) {
        mngr->m_stepSignal.connect([&] {
            const auto& representativeSimulation = mngr->m_simulations.front();
            uint64_t total, seconds, hours, minutes, nanos;
            total = representativeSimulation->time().current / 1'000'000'000;
            minutes = total / 60;
            seconds = total % 60;
            hours = minutes / 60;
            minutes = minutes % 60;
            nanos = representativeSimulation->time().current % 1'000'000'000;
            fmt::println("TIME : {:02d}:{:02d}:{:02d}.{:09d}", hours, minutes, seconds, nanos); 
        });
    }

    mngr->m_disallowPublish = true;

    mngr->m_useMessagePack = node.attribute("useMessagePack").as_bool();

    return mngr;
}

//-------------------------------------------------------------------------

void SimulationManager::setupLogDir(pugi::xml_node node)
{
    struct ChildAttributeGetter
    {
        std::vector<std::string> searchContext;

        std::string operator()(
            pugi::xml_node node,
            const std::string& searchPath,
            const std::string& attrName,
            std::function<bool(pugi::xml_node)> criterion = [](auto) { return true; })
        {
            const auto [current, rest] = [&searchPath] -> std::pair<std::string, std::string> {
                auto splitPos = searchPath.find_first_of("/");
                return {
                    searchPath.substr(0, splitPos),
                    splitPos != std::string::npos
                        ? searchPath.substr(splitPos + 1, searchPath.size())
                        : ""
                };
            }();
            searchContext.push_back(current);

            if (!rest.empty()) {
                pugi::xml_node child = node.find_child([&current](pugi::xml_node child) {
                    return std::string_view{child.name()} == current;
                });
                if (!child) {
                    const auto searchContextCopy = searchContext;
                    searchContext.clear();
                    throw std::runtime_error(fmt::format(
                        "{}: cannot find node '{}'",
                        std::source_location::current().function_name(),
                        fmt::join(searchContextCopy, "/")));
                }
                return operator()(child, rest, attrName);
            }
    
            pugi::xml_attribute attr;
            for (pugi::xml_node child : node.children()) {
                if (std::string_view{child.name()} == current && criterion(child)) {
                    attr = child.attribute(attrName.c_str());
                    break;
                }
            }
            if (!attr) {
                const auto searchContextCopy = searchContext;
                searchContext.clear();
                throw std::runtime_error(fmt::format(
                    "{}: node '{}' has no attribute '{}'",
                    std::source_location::current().function_name(),
                    fmt::join(searchContextCopy, "/"),
                    attrName));
            }
            searchContext.clear();
            return attr.as_string();
        }
    };

    auto createLogDir = [&] {
        m_logDir = fs::current_path() / "logs" / m_logDir;
        fs::create_directories(m_logDir);
        pugi::xml_document doc;
        doc.append_copy(node);
        doc.save_file((m_logDir / "config.xml").c_str());
    };

    m_logDir = node.attribute("id").as_string();
    if (m_logDir != "{{BG_CONFIG}}") {
        if (m_logDir.empty()) {
            m_logDir = boost::uuids::to_string(boost::uuids::random_generator{}());
        }
        createLogDir();
        return;
    }
    
    pugi::xml_node agentsNode;
    if (agentsNode = node.child("Agents"); !agentsNode) {
        throw std::invalid_argument(fmt::format(
            "{}: missing required child 'Agents'",
            std::source_location::current().function_name()));
    }

    auto getAttr = ChildAttributeGetter{};

    const std::string dt = date::format(
        "%Y%m%d_%H%M",
        date::make_zoned(date::current_zone(), std::chrono::system_clock::now()));
    const std::string duration = node.attribute("duration").as_string();
    const std::string books = std::to_string(m_blockInfo.count * m_blockInfo.dimension);

    const auto balances = [&] -> std::string {
        try {
            return fmt::format(
                "{}_{}",
                getAttr(agentsNode, "MultiBookExchangeAgent/Balances/Base", "total"),
                getAttr(agentsNode, "MultiBookExchangeAgent/Balances/Quote", "total"));
        }
        catch (...) {
            return fmt::format(
                "{}_{}",
                getAttr(agentsNode, "MultiBookExchangeAgent/Balances", "type"),
                getAttr(agentsNode, "MultiBookExchangeAgent/Balances", "wealth"));
        }
    }();

    const std::string priceDecimals = getAttr(agentsNode, "MultiBookExchangeAgent", "priceDecimals");
    const std::string volumeDecimals = getAttr(agentsNode, "MultiBookExchangeAgent", "volumeDecimals");
    const std::string baseDecimals = getAttr(agentsNode, "MultiBookExchangeAgent", "baseDecimals");
    const std::string quoteDecimals = getAttr(agentsNode, "MultiBookExchangeAgent", "quoteDecimals");
    const std::string iCount = getAttr(agentsNode, "InitializationAgent", "instanceCount");
    const std::string iPrice = getAttr(agentsNode, "MultiBookExchangeAgent", "initialPrice");
    const std::string fWeight = getAttr(agentsNode, "StylizedTraderAgent", "sigmaF");
    const std::string cWeight = getAttr(agentsNode, "StylizedTraderAgent", "sigmaC");
    const std::string nWeight = getAttr(agentsNode, "StylizedTraderAgent", "sigmaN");
    const std::string tau = getAttr(agentsNode, "StylizedTraderAgent", "tau");
    const std::string sigmaEps = getAttr(agentsNode, "StylizedTraderAgent", "sigmaEps");
    const std::string riskAversion = getAttr(agentsNode, "StylizedTraderAgent", "r_aversion");

    m_logDir = fmt::format(
        "{}-{}-{}-{}-i{}_p{}-f{}_c{}_n{}_t{}_s{}_r{}_d{}_v{}_b{}_q{}",
        dt, duration, books, balances, iCount, iPrice, fWeight, cWeight, nWeight,
        tau, sigmaEps, riskAversion, priceDecimals, volumeDecimals, baseDecimals, quoteDecimals);

    createLogDir();
}

//-------------------------------------------------------------------------

net::awaitable<void> SimulationManager::asyncSendOverNetwork(
    const rapidjson::Value& reqBody, const std::string& endpoint, rapidjson::Document& resJson)
{
    const auto& representativeSimulation = m_simulations.front();

retry:
    auto resolver =
        use_nothrow_awaitable.as_default_on(tcp::resolver{co_await this_coro::executor});
    auto tcp_stream =
        use_nothrow_awaitable.as_default_on(beast::tcp_stream{co_await this_coro::executor});

    int attempts = 0;
    // Resolve.
    auto endpointsVariant = co_await (
        resolver.async_resolve(m_netInfo.host, m_netInfo.port) || timeout(m_netInfo.resolveTimeout));
    while (endpointsVariant.index() == 1) {
        fmt::println("tcp::resolver timed out on {}:{}", m_netInfo.host, m_netInfo.port);
        std::this_thread::sleep_for(10s);
        endpointsVariant = co_await (
            resolver.async_resolve(m_netInfo.host, m_netInfo.port) || timeout(m_netInfo.resolveTimeout));
    }
    auto [e1, endpoints] = std::get<0>(endpointsVariant);
    while (e1) {
        const auto loc = std::source_location::current();
        representativeSimulation->logDebug("{}#L{}: {}:{}: {}", loc.file_name(), loc.line(), m_netInfo.host, m_netInfo.port, e1.what());
        attempts++;
        fmt::println("Unable to resolve connection to validator at {}:{}{} - Retrying (Attempt {})", m_netInfo.host, m_netInfo.port, endpoint, attempts);
        std::this_thread::sleep_for(10s);
        endpointsVariant = co_await (resolver.async_resolve(m_netInfo.host, m_netInfo.port) || timeout(m_netInfo.resolveTimeout));
        auto [e11, endpoints1] = std::get<0>(endpointsVariant);
        e1 = e11;
        endpoints = endpoints1;
    }

    // Connect.
    attempts = 0;
    auto connectVariant = co_await (tcp_stream.async_connect(endpoints) || timeout(m_netInfo.connectTimeout));
    while (connectVariant.index() == 1) {
        fmt::println("tcp_stream::async_connect timed out on {}:{}", m_netInfo.host, m_netInfo.port);
        std::this_thread::sleep_for(10s);
        connectVariant = co_await (tcp_stream.async_connect(endpoints) || timeout(m_netInfo.connectTimeout));
    }
    auto [e2, _2] = std::get<0>(connectVariant);
    while (e2) {
        const auto loc = std::source_location::current();
        representativeSimulation->logDebug("{}#L{}: {}:{}: {}", loc.file_name(), loc.line(), m_netInfo.host, m_netInfo.port, e2.what());
        attempts++;
        fmt::println("Unable to connect to validator at {}:{}{} - Retrying (Attempt {})", m_netInfo.host, m_netInfo.port, endpoint, attempts);
        std::this_thread::sleep_for(10s);
        connectVariant = co_await (tcp_stream.async_connect(endpoints) || timeout(m_netInfo.connectTimeout));
        auto [e21, _21] = std::get<0>(connectVariant);
        e2 = e21;
        _2 = _21;
    }

    // Create the request.
    const auto req = makeHttpRequest(endpoint, taosim::json::json2str(reqBody));

    // Send the request.
    attempts = 0;
    auto writeVariant = co_await (http::async_write(tcp_stream, req) || timeout(m_netInfo.writeTimeout));
    while (writeVariant.index() == 1) {
        fmt::println("http::async_write timed out on {}:{}", m_netInfo.host, m_netInfo.port);
        std::this_thread::sleep_for(5s);
        writeVariant = co_await (http::async_write(tcp_stream, req) || timeout(m_netInfo.writeTimeout));
    }
    auto [e3, _3] = std::get<0>(writeVariant);
    while (e3) {
        const auto loc = std::source_location::current();
        representativeSimulation->logDebug("{}#L{}: {}:{}: {}", loc.file_name(), loc.line(), m_netInfo.host, m_netInfo.port, e3.what());
        attempts++;
        fmt::println("Unable to send request to validator at {}:{}{} - Retrying (Attempt {})", m_netInfo.host, m_netInfo.port, endpoint, attempts);
        goto retry;
    }

    // Receive the response.
    attempts = 0;
    beast::flat_buffer buf;
    http::response_parser<http::string_body> parser{http::response<http::string_body>{}};
    parser.eager(true);
    parser.body_limit(std::numeric_limits<size_t>::max());
    auto readVariant = co_await (http::async_read(tcp_stream, buf, parser) || timeout(m_netInfo.readTimeout));
    if (readVariant.index() == 1) {
        fmt::println("http::async_read timed out on {}:{}", m_netInfo.host, m_netInfo.port);
        goto retry;
    }
    auto [e4, _4] = std::get<0>(readVariant);
    while (e4) {
        const auto loc = std::source_location::current();
        representativeSimulation->logDebug("{}#L{}: {}:{}: {}", loc.file_name(), loc.line(), m_netInfo.host, m_netInfo.port, e4.what());
        attempts++;          
        fmt::println("Unable to read response from validator at {}:{}{} : {} - re-sending request.", m_netInfo.host, m_netInfo.port, endpoint, e4.what(), attempts);
        goto retry;
    }

    http::response<http::string_body> res = parser.release();
    resJson.Parse(res.body().c_str());
    fmt::println("SIMULATOR RECEIVED RESPONSE: {}", res.body().c_str());
}

//-------------------------------------------------------------------------

http::request<http::string_body> SimulationManager::makeHttpRequest(
    const std::string& target, const std::string& body)
{
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target(target);
    req.version(11);
    req.set(http::field::host, m_netInfo.host);
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
    return req;
}

//-------------------------------------------------------------------------

}  // namespace taosim::simulation

//-------------------------------------------------------------------------