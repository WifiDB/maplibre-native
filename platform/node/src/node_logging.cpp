#include "node_logging.hpp"
#include <mbgl/util/enum.hpp>

namespace node_mbgl {

struct NodeLogObserver::LogMessage {
    mbgl::EventSeverity severity;
    mbgl::Event event;
    int64_t code;
    std::string text;

    LogMessage(mbgl::EventSeverity severity_, mbgl::Event event_, int64_t code_, std::string text_)
        : severity(severity_),
          event(event_),
          code(code_),
          text(std::move(text_)) {}
};

NodeLogObserver::NodeLogObserver(Napi::Env env, Napi::Object target)
    : env_(env),
      module_(Napi::Persistent(target)),
      queue(new util::AsyncQueue<LogMessage>(env, [this](LogMessage& message) {
          Napi::HandleScope scope(env_);
          Napi::Object msg = Napi::Object::New(env_);

          msg.Set("class", Napi::String::New(env_, mbgl::Enum<mbgl::Event>::toString(message.event)));
          msg.Set("severity", Napi::String::New(env_, mbgl::Enum<mbgl::EventSeverity>::toString(message.severity)));

          if (message.code != -1) {
              msg.Set("code", Napi::Number::New(env_, message.code));
          }

          if (!message.text.empty()) {
              msg.Set("text", Napi::String::New(env_, message.text));
          }

          Napi::Object handle = module_.Value().As<Napi::Object>();
          Napi::Function emit = handle.Get("emit").As<Napi::Function>();
          emit.Call(handle, {Napi::String::New(env_, "message"), msg});
      })) {}

NodeLogObserver::~NodeLogObserver() {
    queue->stop();
}

bool NodeLogObserver::onRecord(mbgl::EventSeverity severity, mbgl::Event event, int64_t code, const std::string& text) {
    queue->send({severity, event, code, text});
    return true;
}

} // namespace node_mbgl
