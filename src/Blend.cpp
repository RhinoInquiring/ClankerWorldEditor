#include "Blend.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kNodeYards = 1600.0f / 3.0f / 16.0f / 8.0f;   // 4.1667 yd between outer vertices

    float Smoothstep(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    }

    /// Gaussian blur over the nodes where `valid` is set, normalised so missing nodes do not darken edges.
    std::vector<float> Blur(const BlendGrid& g, const std::vector<float>& v, const std::vector<uint8_t>& valid, float sigma)
    {
        const int r = int(std::ceil(sigma * 2.5f));
        std::vector<float> kernel(size_t(2 * r + 1));
        for (int i = -r; i <= r; ++i) kernel[size_t(i + r)] = std::exp(-0.5f * i * i / (sigma * sigma));

        std::vector<float> tmp(v.size(), 0), out(v.size(), 0), tmpW(v.size(), 0);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
            {
                float sum = 0, wsum = 0;
                for (int i = -r; i <= r; ++i)
                {
                    const int xx = x + i;
                    if (xx < 0 || xx >= g.width || !valid[g.At(xx, y)]) continue;
                    sum += kernel[size_t(i + r)] * v[g.At(xx, y)];
                    wsum += kernel[size_t(i + r)];
                }
                tmp[g.At(x, y)] = sum;
                tmpW[g.At(x, y)] = wsum;
            }
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
            {
                float sum = 0, wsum = 0;
                for (int i = -r; i <= r; ++i)
                {
                    const int yy = y + i;
                    if (yy < 0 || yy >= g.height) continue;
                    sum += kernel[size_t(i + r)] * tmp[g.At(x, yy)];
                    wsum += kernel[size_t(i + r)] * tmpW[g.At(x, yy)];
                }
                out[g.At(x, y)] = wsum > 0 ? sum / wsum : v[g.At(x, y)];
            }
        return out;
    }

    /// Distance from every node to the nearest node of `targets` (brute force over the target list).
    std::vector<float> DistanceTo(const BlendGrid& g, const std::vector<std::pair<int, int>>& targets)
    {
        std::vector<float> d(size_t(g.width) * g.height, 1e9f);
        if (targets.empty()) return d;
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
            {
                float best = 1e18f;
                for (const auto& [tx, ty] : targets) best = std::min(best, float((tx - x) * (tx - x) + (ty - y) * (ty - y)));
                d[g.At(x, y)] = std::sqrt(best);
            }
        return d;
    }
}

BlendResult BlendHeights(const BlendGrid& g, float width)
{
    const size_t n = size_t(g.width) * g.height;
    const float w = std::max(width, 1.0f);
    BlendResult result;
    result.heights = g.ground;
    result.paste.assign(n, 0);
    result.insideDist.assign(n, 0);

    // Edge nodes on each side of the paste boundary.
    std::vector<std::pair<int, int>> insideEdge, outsideEdge;
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x)
        {
            const size_t i = g.At(x, y);
            if (!g.exists[i]) continue;
            bool touches = false;
            const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto& o : nb)
            {
                const int xx = x + o[0], yy = y + o[1];
                if (xx >= 0 && xx < g.width && yy >= 0 && yy < g.height && g.exists[g.At(xx, yy)] && g.inside[g.At(xx, yy)] != g.inside[i])
                    touches = true;
            }
            if (touches) (g.inside[i] ? insideEdge : outsideEdge).push_back({ x, y });
        }
    const std::vector<float> toOutside = DistanceTo(g, outsideEdge);   // meaningful for inside nodes
    const std::vector<float> toInside = DistanceTo(g, insideEdge);     // meaningful for outside nodes

    // t: 1 on the paste, falling to 0 over w nodes outside it.
    std::vector<float> t(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        if (!g.exists[i]) continue;
        t[i] = g.inside[i] ? 1.0f : Smoothstep(1.0f - toInside[i] / w);
        result.insideDist[i] = g.inside[i] ? toOutside[i] : 0.0f;
    }

    // The ground's large-scale shape; its detail is what remains.
    const float sigma = 1.5f;
    const std::vector<float> loGround = Blur(g, g.ground, g.exists, sigma);

    // Fixed nodes: the paste (exact) and the ground beyond the band. Free nodes: the band, thin-plate filled.
    std::vector<float> lo(n, 0);
    std::vector<uint8_t> isFree(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        if (!g.exists[i]) continue;
        lo[i] = g.inside[i] ? g.pasted[i] : loGround[i];
        isFree[i] = t[i] > 0.0f && t[i] < 1.0f;
    }

    // Minimise sum (Laplacian of lo)^2 over nodes whose four neighbours exist: (L^T L) x = 0 on free nodes.
    auto valid = [&](int x, int y) { return x > 0 && y > 0 && x < g.width - 1 && y < g.height - 1 && g.exists[g.At(x, y)] &&
                                            g.exists[g.At(x - 1, y)] && g.exists[g.At(x + 1, y)] && g.exists[g.At(x, y - 1)] && g.exists[g.At(x, y + 1)]; };
    auto applyLtL = [&](const std::vector<float>& v, std::vector<float>& out) {
        std::vector<float> lap(n, 0);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
                if (valid(x, y))
                    lap[g.At(x, y)] = v[g.At(x - 1, y)] + v[g.At(x + 1, y)] + v[g.At(x, y - 1)] + v[g.At(x, y + 1)] - 4 * v[g.At(x, y)];
        std::fill(out.begin(), out.end(), 0.0f);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x)
                if (valid(x, y))
                {
                    const float l = lap[g.At(x, y)];
                    out[g.At(x, y)] -= 4 * l;
                    out[g.At(x - 1, y)] += l;
                    out[g.At(x + 1, y)] += l;
                    out[g.At(x, y - 1)] += l;
                    out[g.At(x, y + 1)] += l;
                }
    };

    // Start the free nodes from the crossfade, then conjugate gradients on the free nodes only.
    for (size_t i = 0; i < n; ++i)
        if (isFree[i]) lo[i] = loGround[i];
    std::vector<float> r(n), p(n), ap(n);
    applyLtL(lo, r);
    for (size_t i = 0; i < n; ++i) r[i] = isFree[i] ? -r[i] : 0.0f;
    p = r;
    double rr = 0;
    for (size_t i = 0; i < n; ++i) rr += double(r[i]) * r[i];
    for (int iter = 0; iter < 2000 && rr > 1e-8; ++iter)
    {
        applyLtL(p, ap);
        double pap = 0;
        for (size_t i = 0; i < n; ++i)
            if (isFree[i]) pap += double(p[i]) * ap[i];
        if (pap <= 0) break;
        const float alpha = float(rr / pap);
        double rrNew = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (!isFree[i]) continue;
            lo[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            rrNew += double(r[i]) * r[i];
        }
        const float beta = float(rrNew / rr);
        for (size_t i = 0; i < n; ++i) p[i] = isFree[i] ? r[i] + beta * p[i] : 0.0f;
        rr = rrNew;
    }

    for (size_t i = 0; i < n; ++i)
    {
        if (!g.exists[i]) continue;
        result.paste[i] = t[i];
        if (t[i] >= 1.0f) result.heights[i] = g.pasted[i];
        else if (t[i] > 0.0f)
            result.heights[i] = lo[i] + (1 - t[i]) * (g.ground[i] - loGround[i]);   // ground detail fades back in
    }
    return result;
}

float AutoBlendWidth(const BlendGrid& g)
{
    float mismatch = 0;
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x)
        {
            const size_t i = g.At(x, y);
            if (!g.exists[i] || !g.inside[i]) continue;
            const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto& o : nb)
            {
                const int xx = x + o[0], yy = y + o[1];
                if (xx < 0 || xx >= g.width || yy < 0 || yy >= g.height) continue;
                const size_t j = g.At(xx, yy);
                if (g.exists[j] && !g.inside[j]) mismatch = std::max(mismatch, std::fabs(g.pasted[i] - g.ground[j]));
            }
        }
    // Spread the mismatch over a band so the added slope stays near 20 degrees; 12..120 yd.
    return std::clamp(mismatch / std::tan(20.0f * 3.14159265f / 180.0f), 12.0f, 120.0f);
}

std::array<float, 4> LayerWeights(float a1, float a2, float a3)
{
    return { (1 - a1) * (1 - a2) * (1 - a3), a1 * (1 - a2) * (1 - a3), a2 * (1 - a3), a3 };
}

std::array<float, 3> LayerAlphas(const std::array<float, 4>& w)
{
    std::array<float, 3> a{};
    float sum = w[0];
    for (int k = 1; k < 4; ++k)
    {
        sum += w[size_t(k)];
        a[size_t(k - 1)] = sum > 1e-6f ? std::clamp(w[size_t(k)] / sum, 0.0f, 1.0f) : 0.0f;
    }
    return a;
}

bool BlendSelfTest()
{
    // Weights and alphas round-trip.
    const auto wts = LayerWeights(0.3f, 0.6f, 0.2f);
    const auto back = LayerAlphas(wts);
    if (std::fabs(back[0] - 0.3f) > 1e-4f || std::fabs(back[1] - 0.6f) > 1e-4f || std::fabs(back[2] - 0.2f) > 1e-4f) return false;
    if (std::fabs(wts[0] + wts[1] + wts[2] + wts[3] - 1.0f) > 1e-5f) return false;

    // 60x60 nodes of flat ground at 0; a 20x20 square pasted at height 10 in the middle.
    BlendGrid g;
    g.width = g.height = 60;
    g.ground.assign(3600, 0.0f);
    g.pasted.assign(3600, 0.0f);
    g.inside.assign(3600, 0);
    g.exists.assign(3600, 1);
    for (int y = 20; y < 40; ++y)
        for (int x = 20; x < 40; ++x)
        {
            g.inside[g.At(x, y)] = 1;
            g.pasted[g.At(x, y)] = 10.0f;
        }
    const BlendResult r = BlendHeights(g, 10.0f);
    for (int y = 20; y < 40; ++y)
        for (int x = 20; x < 40; ++x)
            if (r.heights[g.At(x, y)] != 10.0f) return false;              // the paste is kept exactly, edge included
    if (std::fabs(r.heights[g.At(2, 2)]) > 1e-3f) return false;            // far ground untouched
    float worstStep = 0;
    for (int x = 1; x < 60; ++x) worstStep = std::max(worstStep, std::fabs(r.heights[g.At(x, 30)] - r.heights[g.At(x - 1, 30)]));
    // A hard edge would step 10 at once; spread over ~10 nodes the steepest step should be far smaller.
    return worstStep < 2.5f && AutoBlendWidth(g) >= 12.0f && kNodeYards > 4.0f;
}
