// include/common/message_types.hpp

#pragma once

// includes required wherever the macro is called
#include "basic_message.hpp"
#include "command_message.hpp"
#include "status_message.hpp"
#include "telemetry_message.hpp"

// a central registry of all message types
// used by the message factory to dispatch deserialization
#define MESSAGE_TYPES_LIST                                                     \
  X(BasicMessage)                                                              \
  X(CommandMessage)                                                            \
  X(TelemetryMessage)                                                          \
  X(StatusMessage)