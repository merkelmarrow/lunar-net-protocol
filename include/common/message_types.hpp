#pragma once

// include all message headers here
////
#include "basic_message.hpp"
////

// this is a central registry of all message types
// to add a new message, just add it to the list
#define MESSAGE_TYPES_LIST                                                     \
  X(BasicMessage)                                                              \
  X(BasicAck)