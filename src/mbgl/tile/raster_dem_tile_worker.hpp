#pragma once

#include <mbgl/actor/actor_ref.hpp>
#include <mbgl/util/tileset.hpp>

#include <memory>
#include <string>
#include <optional>

namespace mbgl {

class RasterDEMTile;

class RasterDEMTileWorker {
public:
    RasterDEMTileWorker(const ActorRef<RasterDEMTileWorker>&, ActorRef<RasterDEMTile>);

    void parse(const std::shared_ptr<const std::string>& data,
               uint64_t correlationID,
               Tileset::RasterEncoding encoding,
               std::optional<float> redFactor,
               std::optional<float> greenFactor,
               std::optional<float> blueFactor,
               std::optional<float> baseShift);

private:
    ActorRef<RasterDEMTile> parent;
};

} // namespace mbgl
