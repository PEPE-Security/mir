/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Robert Carr <robert.carr@canonical.com>
 */
#include "mir/input/event_filter_dispatcher_policy.h"

namespace mi = mir::input;

mi::EventFilterDispatcherPolicy::EventFilterDispatcherPolicy(std::shared_ptr<mi::EventFilter> const& event_filter) :
  event_filter(event_filter)
{
}

bool mi::EventFilterDispatcherPolicy::filterInputEvent(const android::InputEvent* input_event, uint32_t /*policy_flags*/)
{
    return !event_filter->filter_event(input_event);
}

void mi::EventFilterDispatcherPolicy::interceptKeyBeforeQueueing(const android::KeyEvent* /*key_event*/, uint32_t& policy_flags)
{
    policy_flags = android::POLICY_FLAG_FILTERED;
}
