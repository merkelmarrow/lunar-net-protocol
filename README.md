## Dummary of how the protocol works

### Packets

Packets are framed with a start-of-transmission (STX = 0x2) and an end-of-transmission (ETX = 0x3). A fixed size 10 byte header follows the STX marker.

The header contains message type (ACK, CMD, DATA, etc), Priority (LOW, MEDIUM, HIGH), Sequence number (0 - 255), timestamp (32-bit), and payload length (16-bit).

The payload can be any length but prefferably it should be small enough that the packet won't be fragmented by the network.

A 1-byte CRC-8 checksum calculated on the header and payload comes before the ETX marker.

The LumenPacket class handles serialisation and deserialisation including CRC validation.

### Protocol Handling

This class is the main interface of the protocol.

It operates in either base station or rover mode. It receives raw data from UdpClient or UdpServer, and buffers it per endpoint. It uses from_bytes() to parse complete validated packets.

It asssigns sequence numbers to outgoing messages, and coordinates with ReliabilityManager for ACK/NAK handling and retransmissions.

It passes validated, deserialised payloads up to MessageManager via a callback.

It also includes logic to detect and attempt to process raw JSON messages without the lumen header received over UDP. This is for rover-to-rover.

### Reliability

The base station sends ACKs for received data packets. It only retransmits if it receives a NAK from the rover. It tracks ACK sequences to avoid processing duplicate packets.

The rover only sends NAKs for the sequence numbers it detects are missing. It retransmits packets based on a timeout with exponential backoff if ACKs are not received from the base station.

#### Messaging

Application level data is encapsulated in message objects derived from the Message base class. The message class defines the common interface with serialise(), get_type(), get_sender() etc. Concrete classes such as CommandMessage, StatusMessage etc implement the interface and define their specific data fields. Each message type has a lumen priority.

Messages are serialised into JSON strings. The format include msg_type, sender, timestamp, and then type specific fields. MessageManager handles the conversion between Message objects and binary payloads used by LumenProtocol.

The static deserialise() method parses a JSON string, identifies the msg_type, and invokes the correct subclass's from_json static method to create the object.

### Application layer

A rover and base station class handle the application side.

These classes manage the sessions state and handshake with internal commands.

Rover provides methods for sending telemetry and updating status which is sent periodically.

Provides send_message() which sends messages using the lumen protocol or send_raw_message() which sends raw json over UDP.

Rover discovery is implemented using scan_for_rovers(), and the handle_discovery_command callback using broadcast messages (ROVER_DISCOVER) and direct responses (ROVER_ANNOUNCE) sent as raw json CommandMessages.

Both Rover and Base route incoming messages via route_message() to internal handlers before it sends them to registered callbacks. This allows stuff like session management to be done separately to application logic.

## Usage

### Interoperating

- Discovery: use Rover::snan_for_rovers(discovery_port) to send a broadcast message. Implement the DiscoveryMessageHandler callback (set via Rover::set_discovery_message_handler) to process responses. The handler receives the discovered rover's ID and UDP endpoint.
- Direct communication: Once an endpoint is known, use Rover::send_raw_message() to send messages serialised to json directly via UDP, bypassing the lumen protocol. The receiving rover will detect this as raw json and pass it up through MessageManager to the ApplicationMessageHandler. Implement logic in the ApplicationMessageHandler to handle negotiation, agreement, commands from the base station, whatever.

### Creating telemetry data

- On the rover: populate a std::map<std::string, double> with sensor readings. Call Rover::send_telemetry(readings_map). This will automatically create a TelemetryMessage, serialise it, and send it via Lumen to the base station.

### Processing telemetry data

- On the base station, implement the StatusCallback function type. Register this callback using BaseStation::set_status_callback(callback). This callback will be invoked when either a StatusMessage or TelemetryMessage is received from the connected rover. You will need to inspect the data map within the callback to differentiate between them.

### Command line control

- Use BaseStaion::send_command(command_str, params_str) to send a commmand to the rover. This sends a CommandMessage via Lumen.
- Implement the ApplicationMessageHandler on the BaseStation/Rover and register it using BaseStation::set_application_message_handler(handler). This handler will receive any messages from the rover that aren't automatically handled internally (ie. handshake). You can process command responses or other general messages here.

### Adding new message types

If you need to define a new message type, there are four steps.

1. Define the message class in a header file, make it inherity from the Message class. Use the messages already defined as a guide.

2. Implement the constructor, all override methods required by the Message class, and any other specific methods.

3. Register the message type in the message_types.hpp file. Add an #include and a line for the new message class in the X-macro (eg. X(NegotiateMessage)).

4. Update CMake to include the new file.

- Remember that any message type will always have msg_type, sender, and timestamp no matter what (this is enforced by the base Message class).