#pragma once
#include "ur_modern_driver/log.h"
#include "ur_modern_driver/parser.h"
#include "ur_modern_driver/ur/state.h"
#include "ur_modern_driver/ur/rt_state.h"
#include "ur_modern_driver/ur/messages.h"

template <typename T>
class URStateParser : public Parser {
    std::unique_ptr<Packet> parse(BinParser &bp) {
        int32_t packet_size;
        message_type type;
        bp.parse(packet_size);
        bp.parse(type);

        if(type != message_type::ROBOT_STATE) {
            LOG_ERROR("Invalid message type recieved: %u", static_cast<uint8_t>(type));
            return std::unique_ptr<Packet>(nullptr);
        }

        std::unique_ptr<Packet> obj(new T);
        if(obj->parse_with(bp))
            return obj;
        
        return std::unique_ptr<Packet>(nullptr);
    }
};


template <typename T>
class URRTStateParser : public Parser {
    std::unique_ptr<Packet> parse(BinParser &bp) {
        int32_t packet_size = bp.peek<int32_t>();

        if(!bp.check_size(packet_size)) {
            LOG_ERROR("Buffer len shorter than expected packet length");
            return std::unique_ptr<Packet>(nullptr);        
        }

        bp.parse(packet_size); //consumes the peeked data

        std::unique_ptr<Packet> obj(new T);
        if(obj->parse_with(bp))
            return obj;
        
        return std::unique_ptr<Packet>(nullptr);
    }
};

class URMessageParser : public Parser {
    std::unique_ptr<Packet> parse(BinParser &bp) {
        int32_t packet_size = bp.peek<int32_t>();
        message_type type;

        if(!bp.check_size(packet_size)) {
            LOG_ERROR("Buffer len shorter than expected packet length");
            return std::unique_ptr<Packet>(nullptr);        
        }

        bp.parse(packet_size); //consumes the peeked data
        bp.parse(type);

        if(type != message_type::ROBOT_MESSAGE) {
            LOG_ERROR("Invalid message type recieved: %u", static_cast<uint8_t>(type));
            return std::unique_ptr<Packet>(nullptr);
        }

        uint64_t timestamp;
        uint8_t source;
        robot_message_type message_type;

        bp.parse(timestamp);
        bp.parse(source);
        bp.parse(message_type);

        std::unique_ptr<Packet> obj(nullptr);

        switch(message_type) {
            case robot_message_type::ROBOT_MESSAGE_VERSION:
                VersionMessage *vm = new VersionMessage();
                if(vm->parse_with(bp))
                    obj.reset(vm);
                break;
        }

        return obj;
    }
};