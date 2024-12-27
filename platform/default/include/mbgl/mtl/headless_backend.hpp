#pragma once

#include "mbgl/gfx/headless_backend.hpp"
#include "mbgl/mtl/renderer_backend.hpp"
#include "mbgl/mtl/context.hpp"
#include "mbgl/gfx/renderable.hpp"
#include "mbgl/util/logging.hpp"

#include <memory>
#include <functional>

namespace mbgl {
namespace mtl {

class HeadlessBackend final : public mtl::RendererBackend, public gfx::HeadlessBackend {
public:
    HeadlessBackend(Size = {256, 256},
                    SwapBehaviour = SwapBehaviour::NoFlush,
                    gfx::ContextMode = gfx::ContextMode::Unique);
    ~HeadlessBackend() override;
    void updateAssumedState() override;
    gfx::Renderable& getDefaultRenderable() override;
    PremultipliedImage readStillImage() override;
    RendererBackend* getRendererBackend() override;
    SwapBehaviour getSwapBehaviour();

private:
    void activate() override;
    void deactivate() override;
private:
    bool active = false;
    SwapBehaviour swapBehaviour = SwapBehaviour::NoFlush;
    std::unique_ptr<mtl::Context> context;
    std::unique_ptr<gfx::Renderable> defaultRenderable;
};

} // namespace mtl
} // namespace mbgl
