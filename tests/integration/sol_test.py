import struct

def mqtt_encode_len(remaining_length):
    s = b""
    while remaining_length:
        byte = remaining_length % 128
        remaining_length //= 128
        # If there are more digits to encode, set the top bit of this digit
        if remaining_length > 0:
            byte |= 0x80

        s += struct.pack("!B", byte)
    return s


def create_connect(client_id=None, clean_session=True, keepalive=60,
                   username=None, password=None, will_topic=None, will_qos=0,
                   will_retain=False, will_payload=b""):
    remaining_length = 10
    if client_id is not None:
        client_id = client_id.encode("utf-8")
        remaining_length += 2 + len(client_id)
    else:
        remaining_length += 2

    flags = 0

    if clean_session:
        flags |= 0x02

    if will_topic is not None:
        will_topic = will_topic.encode("utf-8")
        remaining_length += 2 + len(will_topic) + 2 + len(will_payload)
        flags |= 0x04 | ((will_qos & 0x03) << 3)
        if will_retain:
            flags |= 32

    if username is not None:
        username = username.encode("utf-8")
        remaining_length += 2 + len(username)
        flags |= 0x80
        if password is not None:
            password = password.encode("utf-8")
            flags |= 0x40
            remaining_length += 2 + len(password)

    packet_len = mqtt_encode_len(remaining_length)
    packet = struct.pack("!B" + str(len(packet_len)) + "s", 0x10, packet_len)
    packet += struct.pack("!H4sBBH", len(b"MQTT"), b"MQTT", 5, flags, keepalive)

    if client_id != None:
        packet += struct.pack("!H" + str(len(client_id)) + "s", len(client_id), bytes(client_id))
    else:
        packet += struct.pack("!H", 0)

    if will_topic is not None:
        packet += will_properties
        packet += struct.pack("!H" + str(len(will_topic)) + "s", len(will_topic), will_topic)
        if len(will_payload) > 0:
            packet += struct.pack("!H" + str(len(will_payload)) + "s", len(will_payload), will_payload)
        else:
            packet += struct.pack("!H", 0)

    if username != None:
        packet += struct.pack("!H" + str(len(username)) + "s", len(username), username)
        if password is not None:
            packet += struct.pack("!H" + str(len(password)) + "s", len(password), password)
    return packet


def read_connack(packet):
    cmd, _, _, rc = struct.unpack('!BBBB', packet)
    return cmd, rc
