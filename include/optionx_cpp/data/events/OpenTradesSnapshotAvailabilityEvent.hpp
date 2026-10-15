#pragma once
#ifndef OPTIONX_HEADER_DATA_EVENTS_OPEN_TRADES_SNAPSHOT_AVAILABILITY_EVENT_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_EVENTS_OPEN_TRADES_SNAPSHOT_AVAILABILITY_EVENT_HPP_INCLUDED

/// \file OpenTradesSnapshotAvailabilityEvent.hpp
/// \brief Defines the event used to gate trade admission on broker snapshot availability.

namespace optionx::events {

    /// \class OpenTradesSnapshotAvailabilityEvent
    /// \brief Reports whether a current broker active-trades snapshot is authoritative.
    class OpenTradesSnapshotAvailabilityEvent : public utils::Event {
    public:
        bool ready = false; ///< True only after an authoritative snapshot is accepted.
        std::string reason; ///< Human-readable state transition reason.

        /// \brief Constructs a snapshot availability state event.
        /// \param ready True when the broker snapshot can be trusted for admission.
        /// \param reason Human-readable state transition reason.
        OpenTradesSnapshotAvailabilityEvent(
                bool ready,
                std::string reason)
            : ready(ready), reason(std::move(reason)) {}

        /// \brief Default virtual destructor.
        virtual ~OpenTradesSnapshotAvailabilityEvent() = default;

        std::type_index type() const override {
            return typeid(OpenTradesSnapshotAvailabilityEvent);
        }

        const char* name() const override {
            return "OpenTradesSnapshotAvailabilityEvent";
        }
    };

} // namespace optionx::events

#endif // OPTIONX_HEADER_DATA_EVENTS_OPEN_TRADES_SNAPSHOT_AVAILABILITY_EVENT_HPP_INCLUDED
