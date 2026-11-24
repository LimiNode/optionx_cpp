#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_POLICY_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_POLICY_HPP_INCLUDED

/// \file SimulatorPolicy.hpp
/// \brief Deterministic, side-effect-free trading-condition rules for the simulator.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace optionx::platforms {

    /// \brief Half-open UTC seconds-of-day window; a wrapped window crosses midnight.
    struct SimulatorTimeWindow {
        std::uint32_t start = 0;
        std::uint32_t end = 0;

        bool contains(std::uint32_t second) const noexcept {
            if (start < end) return start <= second && second < end;
            return second >= start || second < end;
        }
    };

    /// \brief Inclusive duration interval in whole seconds.
    struct SimulatorExpiryRange {
        std::uint32_t minimum = 1;
        std::uint32_t maximum = 1;

        bool contains(std::uint32_t seconds) const noexcept {
            return minimum <= seconds && seconds <= maximum;
        }
    };

    /// \brief Fields a rule can override independently; money uses native minor units.
    struct SimulatorRuleOverrides {
        std::optional<bool> available;
        std::optional<std::int64_t> payout_ppm;
        std::optional<std::int64_t> min_stake_minor;
        std::optional<std::int64_t> max_stake_minor;
        std::optional<std::uint32_t> max_active_global;
        std::optional<std::uint32_t> max_active_symbol;

        void apply_to(SimulatorRuleOverrides& target) const noexcept {
            if (available) target.available = available;
            if (payout_ppm) target.payout_ppm = payout_ppm;
            if (min_stake_minor) target.min_stake_minor = min_stake_minor;
            if (max_stake_minor) target.max_stake_minor = max_stake_minor;
            if (max_active_global) target.max_active_global = max_active_global;
            if (max_active_symbol) target.max_active_symbol = max_active_symbol;
        }

        bool overlaps_fields(const SimulatorRuleOverrides& other) const noexcept {
            return (available && other.available) ||
                   (payout_ppm && other.payout_ppm) ||
                   (min_stake_minor && other.min_stake_minor) ||
                   (max_stake_minor && other.max_stake_minor) ||
                   (max_active_global && other.max_active_global) ||
                   (max_active_symbol && other.max_active_symbol);
        }
    };

    /// \brief Optional selector dimensions; unset dimensions match every request.
    struct SimulatorRuleSelector {
        std::optional<std::string> symbol;
        std::optional<OrderType> direction;
        std::optional<SimulatorTimeWindow> utc_window;
        std::optional<SimulatorExpiryRange> expiry;

        std::uint32_t specificity() const noexcept {
            return static_cast<std::uint32_t>(symbol.has_value()) +
                   static_cast<std::uint32_t>(direction.has_value()) +
                   static_cast<std::uint32_t>(utc_window.has_value()) +
                   static_cast<std::uint32_t>(expiry.has_value());
        }

        bool matches(const std::string& requested_symbol,
                     OrderType requested_direction,
                     std::uint32_t utc_second,
                     std::uint32_t duration) const noexcept {
            return (!symbol || *symbol == requested_symbol) &&
                   (!direction || *direction == requested_direction) &&
                   (!utc_window || utc_window->contains(utc_second)) &&
                   (!expiry || expiry->contains(duration));
        }
    };

    /// \brief One stable, prioritized policy rule.
    struct SimulatorRule {
        std::string id;
        std::int32_t priority = 0;
        SimulatorRuleSelector selector;
        SimulatorRuleOverrides overrides;
    };

    /// \brief Request facts needed to resolve simulator conditions.
    struct SimulatorPolicyRequest {
        std::string symbol;
        OrderType direction = OrderType::UNKNOWN;
        std::uint32_t utc_second = 0;
        std::uint32_t duration_seconds = 0;
    };

    /// \brief Distinguishes invalid configuration from ineligible trade requests.
    enum class SimulatorPolicyError {
        NONE,
        INVALID_CONFIG,
        DUPLICATE_RULE_ID,
        AMBIGUOUS_RULES,
        INVALID_SYMBOL,
        INVALID_DIRECTION,
        INVALID_TIME,
        INVALID_DURATION,
        UNAVAILABLE
    };

    /// \brief Effective conditions and the ordered identities of matching rules.
    struct SimulatorPolicyResult {
        SimulatorPolicyError error = SimulatorPolicyError::NONE;
        SimulatorRuleOverrides values;
        std::vector<std::string> applied_rule_ids;
    };

    /// \class SimulatorPolicy
    /// \brief Validated rule configuration with deterministic per-field resolution.
    class SimulatorPolicy {
    public:
        std::uint32_t version = 1;
        SimulatorRuleOverrides defaults;
        std::map<std::string, SimulatorRuleOverrides> symbols;
        SimulatorExpiryRange allowed_expiry{60, 30000};
        std::set<std::uint32_t> excluded_expiries;
        std::vector<SimulatorRule> rules;

        /// \brief Validates static bounds and equal-rank overlapping field writers.
        /// \return NONE when configuration can resolve without ambiguous ties.
        SimulatorPolicyError validate() const {
            if (version == 0 || !valid_values(defaults) ||
                allowed_expiry.minimum == 0 ||
                allowed_expiry.minimum > allowed_expiry.maximum) {
                return SimulatorPolicyError::INVALID_CONFIG;
            }
            for (const auto& [symbol, values] : symbols) {
                if (symbol.empty() || !valid_values(values)) {
                    return SimulatorPolicyError::INVALID_CONFIG;
                }
                SimulatorRuleOverrides effective = defaults;
                values.apply_to(effective);
                if (!valid_values(effective)) return SimulatorPolicyError::INVALID_CONFIG;
            }
            for (const auto duration : excluded_expiries) {
                if (!allowed_expiry.contains(duration)) return SimulatorPolicyError::INVALID_CONFIG;
            }
            std::set<std::string> ids;
            for (const auto& rule : rules) {
                if (rule.id.empty() || !valid_values(rule.overrides) ||
                    (rule.selector.symbol &&
                     symbols.find(*rule.selector.symbol) == symbols.end()) ||
                    (rule.selector.direction && *rule.selector.direction == OrderType::UNKNOWN) ||
                    (rule.selector.utc_window && !valid_window(*rule.selector.utc_window)) ||
                    (rule.selector.expiry &&
                     (rule.selector.expiry->minimum == 0 ||
                      rule.selector.expiry->minimum > rule.selector.expiry->maximum))) {
                    return SimulatorPolicyError::INVALID_CONFIG;
                }
                if (!ids.insert(rule.id).second) return SimulatorPolicyError::DUPLICATE_RULE_ID;
            }
            for (std::size_t i = 0; i < rules.size(); ++i) {
                for (std::size_t j = i + 1; j < rules.size(); ++j) {
                    const auto& a = rules[i];
                    const auto& b = rules[j];
                    if (a.priority == b.priority &&
                        a.selector.specificity() == b.selector.specificity() &&
                        a.overrides.overlaps_fields(b.overrides) &&
                        selectors_overlap(a.selector, b.selector)) {
                        return SimulatorPolicyError::AMBIGUOUS_RULES;
                    }
                }
            }
            return SimulatorPolicyError::NONE;
        }

        /// \brief Resolves only effective conditions; no account or quote state is read.
        /// \param request Symbol, direction, UTC open time, and expiry duration.
        /// \return Typed result including matching rule IDs, or a validation error.
        SimulatorPolicyResult resolve(const SimulatorPolicyRequest& request) const {
            SimulatorPolicyResult result;
            result.error = validate();
            if (result.error != SimulatorPolicyError::NONE) return result;
            const auto symbol = symbols.find(request.symbol);
            if (symbol == symbols.end()) {
                result.error = SimulatorPolicyError::INVALID_SYMBOL;
                return result;
            }
            if (request.direction != OrderType::BUY && request.direction != OrderType::SELL) {
                result.error = SimulatorPolicyError::INVALID_DIRECTION;
                return result;
            }
            if (request.utc_second >= 86400) {
                result.error = SimulatorPolicyError::INVALID_TIME;
                return result;
            }
            if (!allowed_expiry.contains(request.duration_seconds) ||
                excluded_expiries.count(request.duration_seconds) != 0) {
                result.error = SimulatorPolicyError::INVALID_DURATION;
                return result;
            }

            result.values = defaults;
            symbol->second.apply_to(result.values);
            std::vector<const SimulatorRule*> matched;
            for (const auto& rule : rules) {
                if (rule.selector.matches(request.symbol, request.direction,
                                          request.utc_second, request.duration_seconds)) {
                    matched.push_back(&rule);
                }
            }
            std::sort(matched.begin(), matched.end(), [](const auto* a, const auto* b) {
                return std::make_tuple(a->priority, a->selector.specificity(), a->id) <
                       std::make_tuple(b->priority, b->selector.specificity(), b->id);
            });
            for (const auto* rule : matched) {
                rule->overrides.apply_to(result.values);
                result.applied_rule_ids.push_back(rule->id);
            }
            if (!valid_values(result.values) || !result.values.payout_ppm ||
                !result.values.min_stake_minor || !result.values.max_stake_minor) {
                result.error = SimulatorPolicyError::INVALID_CONFIG;
            } else if (result.values.available && !*result.values.available) {
                result.error = SimulatorPolicyError::UNAVAILABLE;
            }
            return result;
        }

    private:
        static bool valid_values(const SimulatorRuleOverrides& values) noexcept {
            return (!values.payout_ppm || *values.payout_ppm >= 0) &&
                   (!values.min_stake_minor || *values.min_stake_minor > 0) &&
                   (!values.max_stake_minor || *values.max_stake_minor > 0) &&
                   (!values.min_stake_minor || !values.max_stake_minor ||
                    *values.min_stake_minor <= *values.max_stake_minor);
        }

        static bool valid_window(const SimulatorTimeWindow& window) noexcept {
            return window.start < 86400 && window.end < 86400 &&
                   window.start != window.end;
        }

        static bool windows_overlap(const SimulatorTimeWindow& a,
                                    const SimulatorTimeWindow& b) {
            const auto intervals = [](const SimulatorTimeWindow& w) {
                if (w.start < w.end) {
                    return std::vector<std::pair<std::uint32_t, std::uint32_t>>{{w.start, w.end}};
                }
                return std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                    {w.start, 86400}, {0, w.end}};
            };
            for (const auto& x : intervals(a)) {
                for (const auto& y : intervals(b)) {
                    if (x.first < y.second && y.first < x.second) return true;
                }
            }
            return false;
        }

        static bool selectors_overlap(const SimulatorRuleSelector& a,
                                      const SimulatorRuleSelector& b) {
            return (!a.symbol || !b.symbol || *a.symbol == *b.symbol) &&
                   (!a.direction || !b.direction || *a.direction == *b.direction) &&
                   (!a.utc_window || !b.utc_window ||
                    windows_overlap(*a.utc_window, *b.utc_window)) &&
                   (!a.expiry || !b.expiry ||
                    (a.expiry->minimum <= b.expiry->maximum &&
                     b.expiry->minimum <= a.expiry->maximum));
        }
    };

} // namespace optionx::platforms

#endif // OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_POLICY_HPP_INCLUDED
