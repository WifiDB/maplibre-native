package org.maplibre.android.testapp.activity.style

import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.util.Locale
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.RenderingStats
import org.maplibre.android.maps.Style
import org.maplibre.android.style.layers.HillshadeLayer
import org.maplibre.android.style.layers.PropertyFactory.hillshadeExaggeration
import org.maplibre.android.style.layers.PropertyFactory.hillshadeMethod
import org.maplibre.android.style.layers.SymbolLayer
import org.maplibre.android.style.sources.RasterDemSource
import org.maplibre.android.style.terrain.Terrain
import org.maplibre.android.testapp.R
import org.maplibre.android.testapp.styles.TestStyles

/**
 * Test activity showcasing 3D terrain on a full-planet vector basemap:
 * the OpenFreeMap Liberty style combined with the Mapterhorn raster-dem
 * tiles (https://mapterhorn.com), both with worldwide coverage.
 *
 * The DEM sources, hillshade layer, and terrain configuration are added
 * at runtime with the style API. Terrain gets its own raster-dem source
 * (same tiles) per the maplibre-gl-js recommendation.
 */
class TerrainVectorMapActivity :
    AppCompatActivity(),
    MapView.OnDidFinishRenderingFrameWithStatsListener {
    private lateinit var mapView: MapView
    private lateinit var maplibreMap: MapLibreMap
    private lateinit var fpsView: TextView

    // Latest measured values; the overlay text is refreshed from the per-frame
    // stats callback, which is the same cadence the drape counter updates at.
    private var lastFps = 0.0
    private var peakDrapes = 0

    // Tap the map to cycle measurement modes, isolating each DEM cost from the
    // vector style: 0 = terrain + hillshade, 1 = hillshade only (no terrain),
    // 2 = pure vector (no terrain, no hillshade - "terrain OFF" alone still keeps
    // the hillshade raster-dem layer, so this is the true style-only baseline)
    private var mode = 0
    private val modeNames = arrayOf("terrain+hillshade", "hillshade only", "vector only")

    // Long-press toggles a "lite" pass that strips the heaviest Liberty layers
    // (POI/road-name/shield symbols, one-way arrows, 3D buildings, minor roads,
    // and tunnel/bridge road variants) to measure the FPS headroom in the style.
    private var lite = false

    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_fill_extrusion_layer)
        mapView = findViewById(R.id.mapView)
        mapView.onCreate(savedInstanceState)

        // Measure-first overlay in the top-left corner: FPS + terrain drape churn
        fpsView = TextView(this).apply {
            setBackgroundColor(Color.argb(128, 0, 0, 0))
            setTextColor(Color.WHITE)
            setPadding(24, 12, 24, 12)
            text = "FPS: --"
        }
        addContentView(
            fpsView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.TOP or Gravity.START }
        )

        mapView.addOnDidFinishRenderingFrameListener(this)

        mapView.getMapAsync { map ->
            maplibreMap = map
            map.setOnFpsChangedListener { fps -> lastFps = fps }
            map.cameraPosition = CameraPosition.Builder()
                .target(LatLng(47.26475, 11.40416)) // Innsbruck
                .zoom(12.0)
                .tilt(60.0)
                .bearing(20.0)
                .build()
            loadStyle()

            // Tap cycles the three measurement modes back-to-back (same thermal state)
            map.addOnMapClickListener {
                val style = map.style
                if (style != null && style.isFullyLoaded) {
                    mode = (mode + 1) % modeNames.size
                    applyMode(style)
                    peakDrapes = 0 // reset peak so the new mode's numbers are clean
                }
                true
            }

            // Long-press toggles the lite (stripped) style vs the full Liberty style
            map.addOnMapLongClickListener {
                lite = !lite
                loadStyle() // reload so turning lite off restores every layer cleanly
                peakDrapes = 0
                true
            }
        }
    }

    private fun loadStyle() {
        maplibreMap.setStyle(Style.Builder().fromUri(TestStyles.OPENFREEMAP_LIBERTY)) { style ->
            addTerrain(style)
            applyMode(style)
            if (lite) lightenStyle(style)
        }
    }

    private fun addTerrain(style: Style) {
        // The Mapterhorn TileJSON declares "encoding": "terrarium", which the
        // tileset parser picks up
        style.addSource(RasterDemSource(SOURCE_ID_HILLSHADE, DEM_TILEJSON))
        style.addSource(RasterDemSource(SOURCE_ID_TERRAIN, DEM_TILEJSON))
        addHillshade(style)
        style.setTerrain(Terrain(source = SOURCE_ID_TERRAIN, exaggeration = 1.0f))
    }

    private fun addHillshade(style: Style) {
        if (style.getLayer(LAYER_ID_HILLSHADE) != null) {
            return
        }
        // Insert the hillshade below the first symbol layer so labels stay on top
        val hillshade = HillshadeLayer(LAYER_ID_HILLSHADE, SOURCE_ID_HILLSHADE)
            .withProperties(
                hillshadeMethod("igor"),
                hillshadeExaggeration(0.4f)
            )
        val firstSymbolLayer = style.layers.firstOrNull { it is SymbolLayer }
        if (firstSymbolLayer != null) {
            style.addLayerBelow(hillshade, firstSymbolLayer.id)
        } else {
            style.addLayer(hillshade)
        }
    }

    // Strip the heaviest Liberty layers for a terrain overview: label/POI/shield
    // symbols (glyph shaping + collision, the priciest per layer), one-way arrows,
    // 3D buildings, minor/service/path roads, and the tunnel/bridge road variants
    // (indistinguishable from surface roads at this zoom). Keeps major roads, water,
    // landcover/landuse, boundaries, and place labels.
    private fun lightenStyle(style: Style) {
        val heavy = { id: String ->
            id.startsWith("poi") ||
                id.contains("one_way_arrow") ||
                id.contains("shield") ||
                id.startsWith("highway-name") ||
                id == "building-3d" ||
                id.startsWith("tunnel_") ||
                id.startsWith("bridge_") ||
                id.contains("service_track") ||
                id.contains("_minor") ||
                id.contains("path_pedestrian") ||
                id.contains("_link") ||
                id.contains("_label")
        }
        // Snapshot ids first: removeLayer mutates the layer list
        style.layers.map { it.id }.filter(heavy).forEach { style.removeLayer(it) }
    }

    // Apply the current measurement mode: terrain and hillshade are independent
    // style features, so "terrain off" alone still keeps the hillshade raster-dem
    // layer (and its DEM decode + prepare pass). Mode 2 removes both for a true
    // vector-only baseline.
    private fun applyMode(style: Style) {
        style.setTerrain(if (mode == 0) Terrain(SOURCE_ID_TERRAIN, exaggeration = 1.0f) else null)
        if (mode <= 1) {
            addHillshade(style)
        } else {
            style.removeLayer(LAYER_ID_HILLSHADE)
        }
    }

    // Fires once per rendered frame with that frame's stats. numDrapeTargetsRendered
    // is the number of terrain drape textures re-rendered this frame (cache misses):
    // it should fall to 0 on a static scene once tiles have loaded, and spike while
    // panning/zooming or streaming DEM. That is the direct signal for whether we are
    // paying for draping every frame.
    override fun onDidFinishRenderingFrame(fully: Boolean, stats: RenderingStats) {
        val drapes = stats.numDrapeTargetsRendered
        if (drapes > peakDrapes) peakDrapes = drapes

        // CPU time the engine spent producing this frame (encode + render command
        // building), in ms. Compare it to the wall-clock frame time (1000/FPS):
        //   cpuMs ~= frameMs  -> CPU-bound (the engine itself is the wall)
        //   cpuMs <<  frameMs -> GPU-bound (CPU submits fast, then waits on the GPU)
        val cpuMs = (stats.encodingTime + stats.renderingTime) * 1000.0
        val frameMs = if (lastFps > 0.0) 1000.0 / lastFps else 0.0
        val updMs = stats.terrainUpdateTime * 1000.0
        val twkMs = stats.terrainTweakerTime * 1000.0
        val depMs = stats.terrainDepthTime * 1000.0
        fpsView.text = String.format(
            Locale.US,
            "mode: %s  %s (tap / long-press)\n" +
                "FPS: %4.1f  (frame %.0fms)\n" +
                "cpu: %.1fms  gpu~: %.0fms\n" +
                "terrain cpu: upd %.0f / twk %.0f / dep %.0f ms\n" +
                "drapes: %d (peak %d)  scans: %d  draws: %d",
            modeNames[mode],
            if (lite) "[LITE]" else "[full]",
            lastFps,
            frameMs,
            cpuMs,
            (frameMs - cpuMs).coerceAtLeast(0.0),
            updMs,
            twkMs,
            depMs,
            drapes,
            peakDrapes,
            stats.numDrapeCoverageScans,
            stats.numDrawCalls
        )
    }

    companion object {
        private const val DEM_TILEJSON = "https://tiles.mapterhorn.com/tilejson.json"
        private const val SOURCE_ID_HILLSHADE = "mapterhorn"
        private const val SOURCE_ID_TERRAIN = "mapterhorn-terrain"
        private const val LAYER_ID_HILLSHADE = "mapterhorn-hillshade"
    }

    override fun onStart() {
        super.onStart()
        mapView.onStart()
    }

    override fun onResume() {
        super.onResume()
        mapView.onResume()
    }

    override fun onPause() {
        super.onPause()
        mapView.onPause()
    }

    override fun onStop() {
        super.onStop()
        mapView.onStop()
    }

    public override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    public override fun onDestroy() {
        super.onDestroy()
        mapView.removeOnDidFinishRenderingFrameListener(this)
        mapView.onDestroy()
    }
}
