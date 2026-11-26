#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_QUOTE_BOOK_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_QUOTE_BOOK_HPP_INCLUDED
/// \file SimulatorQuoteBook.hpp
/// \brief Bounded owner-thread quote evidence; source and receive time are distinct.
#include <atomic>
#include <map>
#include <thread>

namespace optionx::platforms {

enum class SimulatorQuoteAvailability { VALID, MISSING, NOT_READY, STALE, BLOCKED };
struct SimulatorQuoteDecision {
    SimulatorQuoteAvailability availability = SimulatorQuoteAvailability::MISSING;
    std::optional<SimulatorTick> quote;
};

/// \class SimulatorQuoteBook
/// \brief One current source per symbol; strict zero-wait as-of selection, no implicit fallback.
class SimulatorQuoteBook : public std::enable_shared_from_this<SimulatorQuoteBook> {
public:
    explicit SimulatorQuoteBook(std::size_t max_events = 4096, std::size_t max_bindings = 64)
        : m_owner(std::this_thread::get_id()), m_max_events(max_events), m_max_bindings(max_bindings) {
        if (!max_events || max_events > 1000000 || !max_bindings || max_bindings > 256)
            throw std::invalid_argument("simulator quote book bounds");
        m_sources.reserve(max_bindings);
    }
    SimulatorQuoteBook(const SimulatorQuoteBook&) = delete;
    SimulatorQuoteBook& operator=(const SimulatorQuoteBook&) = delete;

    /// \brief Allocates a never-reused generation; replacement requires explicit unbind.
    std::uint64_t bind(std::string symbol, std::int64_t max_age_ms) {
        if (!owner() || m_blocked || symbol.empty() || symbol.size() > 64 || max_age_ms < 0 ||
            m_sources.size() == m_max_bindings) return 0;
        auto existing = m_current.find(symbol);
        if (existing != m_current.end() && m_sources[existing->second].active) return 0;
        const auto index = m_sources.size();
        try {
            m_sources.push_back({symbol, max_age_ms, true, false, {}});
            m_current[std::move(symbol)] = index;
        } catch (...) { block(); throw; }
        return index + 1;
    }
    void unbind(std::uint64_t generation) noexcept {
        if (auto* source = current(generation)) source->active = false;
    }
    void accept(std::uint64_t generation, bool accepted) noexcept {
        if (auto* source = current(generation)) source->accepted = accepted;
    }
    void block() noexcept { m_blocked.store(true); }

    bool status(std::uint64_t generation, bool ready, std::int64_t received) {
        auto* source = current(generation);
        if (!source || !stamp(received)) return false;
        if (!source->events.empty() && source->events.back().status &&
            source->events.back().ready == ready) return true;
        return append(*source, {received, true, ready, {}});
    }
    bool observe(std::uint64_t generation, SimulatorTick tick) {
        auto* source = current(generation);
        if (!source || !source->accepted || tick.symbol != source->symbol ||
            !stamp(tick.received_ms) || tick.timestamp_ms < 0 ||
            tick.timestamp_ms > tick.received_ms || tick.price_units <= 0) return false;
        bool ready = false;
        for (const auto& event : source->events) if (event.status) ready = event.ready;
        if (!ready) return false;
        tick.source_generation = generation;
        tick.observation_id = m_events + 1;
        // Suppress exact repeats only within the current READY epoch.
        for (auto it = source->events.rbegin(); it != source->events.rend(); ++it) {
            if (it->status) break;
            if (it->tick.timestamp_ms == tick.timestamp_ms && it->tick.price_units == tick.price_units)
                return true;
        }
        const auto received = tick.received_ms;
        return append(*source, {received, false, false, std::move(tick)});
    }
    SimulatorQuoteDecision select(const std::string& symbol, std::int64_t event_ms) const {
        if (!owner() || m_blocked) return {SimulatorQuoteAvailability::BLOCKED, {}};
        const auto found = m_current.find(symbol);
        if (found == m_current.end()) return {};
        const auto& source = m_sources[found->second];
        if (!source.active || !source.accepted) return {SimulatorQuoteAvailability::NOT_READY, {}};
        bool ready = false;
        std::optional<SimulatorTick> best;
        for (const auto& event : source.events) {
            if (event.received > event_ms) break;
            if (event.status) {
                if (!event.ready || !ready) best.reset();
                ready = event.ready;
            } else if (ready && event.tick.timestamp_ms <= event_ms &&
                       (!best || event.tick.timestamp_ms >= best->timestamp_ms)) best = event.tick;
        }
        if (!ready) return {SimulatorQuoteAvailability::NOT_READY, {}};
        if (!best) return {};
        if (event_ms - best->timestamp_ms > source.max_age)
            return {SimulatorQuoteAvailability::STALE, {}};
        return {SimulatorQuoteAvailability::VALID, std::move(best)};
    }
    /// \brief Selector captures weak lifetime only, never a raw binding or provider.
    SimulatorCore::quote_selector_t selector() {
        const std::weak_ptr<SimulatorQuoteBook> weak = shared_from_this();
        return [weak](const std::string& symbol, std::int64_t when) -> std::optional<SimulatorTick> {
            const auto book = weak.lock();
            return book ? book->select(symbol, when).quote : std::nullopt;
        };
    }
private:
    struct Event { std::int64_t received; bool status; bool ready; SimulatorTick tick; };
    struct Source {
        std::string symbol;
        std::int64_t max_age;
        bool active;
        bool accepted;
        std::vector<Event> events;
    };
    bool owner() const noexcept {
        if (m_owner == std::this_thread::get_id()) return true;
        m_blocked.store(true); return false;
    }
    Source* current(std::uint64_t generation) noexcept {
        if (!owner() || m_blocked || !generation || generation > m_sources.size()) return nullptr;
        auto& source = m_sources[static_cast<std::size_t>(generation - 1)];
        return source.active ? &source : nullptr;
    }
    bool stamp(std::int64_t value) noexcept {
        if (value < 0 || value < m_last_received) { block(); return false; }
        m_last_received = value; return true;
    }
    bool append(Source& source, Event event) {
        if (m_events == m_max_events) { block(); return false; }
        try { source.events.push_back(std::move(event)); ++m_events; return true; }
        catch (...) { block(); return false; }
    }
    const std::thread::id m_owner;
    const std::size_t m_max_events;
    const std::size_t m_max_bindings;
    mutable std::atomic_bool m_blocked{false};
    std::int64_t m_last_received = 0;
    std::size_t m_events = 0;
    std::vector<Source> m_sources;
    std::map<std::string, std::size_t> m_current;
};
} // namespace optionx::platforms
#endif // OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_QUOTE_BOOK_HPP_INCLUDED
