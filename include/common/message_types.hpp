// include/common/message_types.hpp

#pragma once

// include all message headers here
// not used directly here but required wherever the macro is called
////
#include "basic_message.hpp"
////

// this is a central registry of all message types
// to add a new message, just add it to the list
#define MESSAGE_TYPES_LIST X(BasicMessage)