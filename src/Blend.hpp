#pragma once

#include <array>
#include <cstdint>
#include <vector>

// Seam blending for pastes, on a regular grid of terrain vertices. No editor or GPU types here.

/// One height field over a rectangular grid of outer vertices (4.17 yd apart).
struct BlendGrid
{
    int width = 0, height = 0;
    std::vector<float> ground;     // current terrain height per node
    std::vector<float> pasted;     // pasted height per node (only meaningful where inside)
    std::vector<uint8_t> inside;   // 1 = covered by the paste
    std::vector<uint8_t> exists;   // 0 = no terrain loaded at this node

    size_t At(int x, int y) const { return size_t(y) * size_t(width) + size_t(x); }
};

struct BlendResult
{
    std::vector<float> heights;      // final height per node (ground where untouched)
    std::vector<float> paste;        // 0..1: how much of the paste each node shows
    std::vector<float> insideDist;   // inside nodes: distance to the nearest outside node, in node units
};

/// Blends the ground into the paste across a band `width` nodes wide outside the paste's edge. The paste
/// itself is kept exactly; the ground around it gets a thin-plate (biharmonic) fill of its large-scale
/// shape, so height and slope both meet the paste's edge, while its own fine detail fades back in.
BlendResult BlendHeights(const BlendGrid& grid, float width);

/// Band width (yards) that keeps the added slope gentle for the largest height mismatch on the edge.
float AutoBlendWidth(const BlendGrid& grid);

/// WotLK terrain layers blend in sequence: colour = lerp(lerp(lerp(t0, t1, a1), t2, a2), t3, a3).
/// Converts the three alphas of one texel to the weight of each of the four layers, and back.
std::array<float, 4> LayerWeights(float a1, float a2, float a3);
std::array<float, 3> LayerAlphas(const std::array<float, 4>& weights);

/// Flat ground, a raised square pasted on it: core stays pasted, far ground stays put, no jumps in between.
bool BlendSelfTest();
