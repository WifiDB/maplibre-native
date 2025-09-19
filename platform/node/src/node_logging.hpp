#pragma once

#include "util/async_queue.hpp"

#include <mbgl/util/logging.hpp>
#include <napi.h>

namespace node_mbgl {

namespace util {
template <typename T>
class AsyncQueue;
}

class NodeLogObserver : public mbgl::Log::Observer {
public:
    NodeLogObserver(Napi::Env env, Napi::Object target);
    ~NodeLogObserver() override;

    // Log::Observer implementation
    bool onRecord(mbgl::EventSeverity severity, mbgl::Event event, int64_t code, const std::string& text) override;

private:
    Napi::Env env_;
    Napi::ObjectReference module_;

    struct LogMessage;
    util::AsyncQueue<LogMessage>* queue;
};

} // namespace node_mbgl
