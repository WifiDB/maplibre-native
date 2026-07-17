package org.maplibre.android.maps;

public class RenderingStats {
  /// Frame CPU encoding time (seconds)
  public double encodingTime = 0.0;
  /// Frame CPU rendering time (seconds)
  public double renderingTime = 0.0;

  /// Number of frames rendered
  public int numFrames = 0;
  /// Number of draw calls (`glDrawElements`, `drawIndexedPrimitives`, etc.) executed during the most recent frame
  public int numDrawCalls = 0;
  /// Total number of draw calls executed during all the frames
  public int totalDrawCalls = 0;

  /// Total number of textures created
  public int numCreatedTextures = 0;
  /// Net textures
  public int numActiveTextures = 0;
  /// Net texture bindings
  public int numTextureBindings = 0;
  /// Number of times a texture was updated
  public int numTextureUpdates = 0;
  /// Number of bytes used in texture updates
  public long textureUpdateBytes = 0;

  /// Number of buffers created
  public long totalBuffers = 0;
  /// Number of SDK-specific buffers created
  public long totalBufferObjs = 0;
  /// Number of times a buffer is updated
  public long bufferUpdates = 0;
  /// Number of times an SDK-specific buffer is updated
  public long bufferObjUpdates = 0;
  /// Sum of update sizes
  public long bufferUpdateBytes = 0;

  /// Number of active buffers
  public int numBuffers = 0;
  /// Number of active offscreen frame buffers
  public int numFrameBuffers = 0;

  /// Number of active index buffers
  public int numIndexBuffers = 0;
  /// Sum of index buffers update sizes
  public long indexUpdateBytes = 0;

  /// Number of active vertex buffers
  public int numVertexBuffers = 0;
  /// Sum of vertex buffers update sizes
  public long vertexUpdateBytes = 0;

  /// Number of active uniform buffers
  public int numUniformBuffers = 0;
  /// Number of times a uniform buffer is updated
  public int numUniformUpdates = 0;
  /// Sum of uniform buffers update sizes
  public long uniformUpdateBytes = 0;

  /// Total texture memory
  public int memTextures = 0;
  /// Total buffer memory
  public int memBuffers = 0;
  /// Total index buffer memory
  public int memIndexBuffers = 0;
  /// Total vertex buffer memory
  public int memVertexBuffers = 0;
  /// Total uniform buffer memory
  public int memUniformBuffers = 0;

  /// Number of stencil buffer clears
  public int stencilClears = 0;
  /// Number of stencil buffer updates
  public int stencilUpdates = 0;

  /// Number of terrain drape targets actually re-rendered this frame (cache misses).
  /// Panning a static terrain scene should keep this at 0.
  public int numDrapeTargetsRendered = 0;

  /// Number of terrain drape targets that ran the full per-target coverage scan this
  /// frame (fast-path misses). Should be ~0 on a static scene once loaded.
  public int numDrapeCoverageScans = 0;

  /// Per-phase CPU time of the terrain path this frame (seconds).
  public double terrainUpdateTime = 0.0;
  public double terrainTweakerTime = 0.0;
  public double terrainDepthTime = 0.0;
}
