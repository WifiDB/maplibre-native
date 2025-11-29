// src/mbgl/layout/line_dash_util.hpp
#pragma once

#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/renderer/paint_property_binder.hpp>
#include <mbgl/layout/pattern_layout.hpp>

#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace mbgl {

/**
 * Format a dash pattern array and line cap as a string key for LineAtlas lookup.
 * 
 * Examples:
 *   formatDashKey([2, 1], Square) → "2,1,square"
 *   formatDashKey([4, 2, 1], Round) → "4,2,1,round"
 */
inline std::string formatDashKey(const std::vector<float>& dasharray, LinePatternCap cap) {
    std::ostringstream oss;
    for (size_t i = 0; i < dasharray.size(); ++i) {
        if (i > 0) oss << ",";
        oss << dasharray[i];
    }
    oss << "," << (cap == LinePatternCap::Round ? "round" : "square");
    return oss.str();
}

/**
 * Parse a dash key back into dasharray and cap components.
 * 
 * Examples:
 *   parseDashKey("2,1,square") → ([2, 1], Square)
 *   parseDashKey("4,2,1,round") → ([4, 2, 1], Round)
 *   parseDashKey("image-name") → ([], Square)  // Not a dash key
 */
inline std::pair<std::vector<float>, LinePatternCap> parseDashKey(const std::string& key) {
    std::vector<float> dasharray;
    std::istringstream iss(key);
    std::string token;
    
    while (std::getline(iss, token, ',')) {
        if (token == "round") {
            return {dasharray, LinePatternCap::Round};
        } else if (token == "square") {
            return {dasharray, LinePatternCap::Square};
        }
        
        // Try to parse as float
        try {
            dasharray.push_back(std::stof(token));
        } catch (const std::exception&) {
            // Not a number - this is an image pattern key, not a dash key
            return {{}, LinePatternCap::Square};
        }
    }
    
    // No cap suffix found - assume square
    return {dasharray, LinePatternCap::Square};
}

/**
 * Check if a key represents a dash pattern (vs an image pattern).
 * Dash keys contain comma-separated numbers and end with "round" or "square".
 * Image pattern keys are just image names without commas.
 */
inline bool isDashKey(const std::string& key) {
    // Quick check: dash keys contain commas
    if (key.find(',') == std::string::npos) {
        return false;
    }
    
    // Check if it ends with "round" or "square"
    return (key.size() > 6 && key.substr(key.size() - 6) == "square") ||
           (key.size() > 5 && key.substr(key.size() - 5) == "round");
}

/**
 * Extract DashEntry from a DashPatternTexture for use in paint property binder.
 * 
 * @param texture The DashPatternTexture from LineAtlas
 * @param useFrom If true, use "from" position, otherwise use "to" position
 * @return DashEntry with y, height, width for vertex attributes
 */
inline DashEntry extractDashEntry(const DashPatternTexture& texture, bool useFrom) {
    const LinePatternPos& pos = useFrom ? texture.getFrom() : texture.getTo();
    return DashEntry{
        pos.y,      // Y position in atlas (normalized 0-1)
        pos.height, // Height in atlas (normalized 0-1)
        pos.width   // Width in pixels
    };
}

/**
 * Build a DashPositions map from PatternLayerMap using LineAtlas.
 * This converts the string keys (like "2,1,square") into actual atlas positions.
 * 
 * @param patternDependencies Map of layer IDs to their min/mid/max pattern keys
 * @param lineAtlas LineAtlas instance to lookup dash patterns
 * @return DashPositions map with keys pointing to atlas positions
 */
inline DashPositions prepareDashPositions(
    const PatternLayerMap& patternDependencies,
    LineAtlas& lineAtlas)
{
    DashPositions result;
    
    for (const auto& [layerId, dependency] : patternDependencies) {
        // Process min key (zoom - 1)
        if (isDashKey(dependency.min)) {
            auto [minDash, minCap] = parseDashKey(dependency.min);
            if (!minDash.empty()) {
                // Get or create DashPatternTexture
                // For crossfading, we use same pattern for both from/to at this zoom
                auto& texture = lineAtlas.getDashPatternTexture(minDash, minDash, minCap);
                result[dependency.min] = extractDashEntry(texture, true);
            }
        }
        
        // Process mid key (current zoom)
        if (isDashKey(dependency.mid)) {
            auto [midDash, midCap] = parseDashKey(dependency.mid);
            if (!midDash.empty()) {
                auto& texture = lineAtlas.getDashPatternTexture(midDash, midDash, midCap);
                result[dependency.mid] = extractDashEntry(texture, true);
            }
        }
        
        // Process max key (zoom + 1)
        if (isDashKey(dependency.max)) {
            auto [maxDash, maxCap] = parseDashKey(dependency.max);
            if (!maxDash.empty()) {
                auto& texture = lineAtlas.getDashPatternTexture(maxDash, maxDash, maxCap);
                result[dependency.max] = extractDashEntry(texture, true);
            }
        }
    }
    
    return result;
}

/**
 * Helper to check if a property is a data-driven dasharray.
 * Used to determine if we need to evaluate dasharray expressions.
 */
template <typename LayerPropertiesType>
inline bool hasDataDrivenDasharray(const LayerPropertiesType& properties) {
    const auto& dasharrayProp = properties.evaluated.template get<style::LineDasharray>();
    return !dasharrayProp.isConstant();
}

/**
 * Helper to get the LinePatternCap for a layer based on its line-cap property.
 */
template <typename LayerPropertiesType>
inline LinePatternCap getPatternCap(const LayerPropertiesType& properties) {
    const auto lineCap = properties.evaluated.template get<style::LineCap>();
    return (lineCap == style::LineCapType::Round) 
        ? LinePatternCap::Round 
        : LinePatternCap::Square;
}

} // namespace mbgl
