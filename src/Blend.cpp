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

    /// Distance from every node to the nearest node of `targets`: exact Euclidean distance transform (Felzenszwalb and
    /// Huttenlocher), squared distances per column, then the lower envelope of parabolas along each row.
    std::vector<float> DistanceTo(const BlendGrid& g, const std::vector<std::pair<int, int>>& targets)
    {
        std::vector<float> d(size_t(g.width) * g.height, 1e9f);
        if (targets.empty()) return d;
        constexpr int64_t kNone = -1;
        // Columns: squared distance to the nearest target above or below (kNone: the column has none).
        std::vector<int64_t> col(d.size(), kNone);
        std::vector<uint8_t> is(d.size(), 0);
        for (const auto& [tx, ty] : targets) is[g.At(tx, ty)] = 1;
        for (int x = 0; x < g.width; ++x)
        {
            int last = -1;
            for (int y = 0; y < g.height; ++y)
            {
                if (is[g.At(x, y)]) last = y;
                if (last >= 0) col[g.At(x, y)] = int64_t(y - last) * (y - last);
            }
            last = -1;
            for (int y = g.height - 1; y >= 0; --y)
            {
                if (is[g.At(x, y)]) last = y;
                if (last >= 0 && (col[g.At(x, y)] == kNone || int64_t(last - y) * (last - y) < col[g.At(x, y)]))
                    col[g.At(x, y)] = int64_t(last - y) * (last - y);
            }
        }
        // Rows: min over columns q of (x - q)^2 + col(q).
        std::vector<int> v(size_t(g.width));
        std::vector<double> z(size_t(g.width) + 1);
        for (int y = 0; y < g.height; ++y)
        {
            auto f = [&](int q) { return col[g.At(q, y)]; };
            auto cross = [&](int q, int p) { return (double(f(q) + int64_t(q) * q) - double(f(p) + int64_t(p) * p)) / (2.0 * (q - p)); };
            int k = -1;
            for (int q = 0; q < g.width; ++q)
            {
                if (f(q) == kNone) continue;
                double s = -1e300;
                while (k >= 0 && (s = cross(q, v[size_t(k)])) <= z[size_t(k)]) --k;
                if (k < 0) s = -1e300;
                v[size_t(++k)] = q;
                z[size_t(k)] = s;
            }
            if (k < 0) continue;   // no column of this row reaches a target (only with targets nowhere)
            z[size_t(k) + 1] = 1e300;
            int j = 0;
            for (int x = 0; x < g.width; ++x)
            {
                while (z[size_t(j) + 1] < x) ++j;
                const int q = v[size_t(j)];
                d[g.At(x, y)] = std::sqrt(float(int64_t(x - q) * (x - q) + f(q)));
            }
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
    // The nodes in row-major order, worked out once: CG applies L^T L up to 2000 times. Only those touching a free
    // node: CG reads the result on free nodes alone, and the others add nothing there.
    const size_t row = size_t(g.width);
    std::vector<size_t> centres;
    for (int y = 1; y < g.height - 1; ++y)
        for (int x = 1; x < g.width - 1; ++x)
            if (const size_t i = g.At(x, y); g.exists[i] && g.exists[i - 1] && g.exists[i + 1] && g.exists[i - row] && g.exists[i + row] &&
                                             (isFree[i] || isFree[i - 1] || isFree[i + 1] || isFree[i - row] || isFree[i + row]))
                centres.push_back(i);
    std::vector<float> lap(n, 0);
    auto applyLtL = [&](const std::vector<float>& v, std::vector<float>& out) {
        for (size_t i : centres) lap[i] = v[i - 1] + v[i + 1] + v[i - row] + v[i + row] - 4 * v[i];
        std::fill(out.begin(), out.end(), 0.0f);
        for (size_t i : centres)
        {
            const float l = lap[i];
            out[i] -= 4 * l;
            out[i - 1] += l;
            out[i + 1] += l;
            out[i - row] += l;
            out[i + row] += l;
        }
    };

    // Start the free nodes from the crossfade, then conjugate gradients on the free nodes only.
    // r and p stay 0 off the free nodes, so the vector steps only visit those.
    std::vector<size_t> freeNodes;
    for (size_t i = 0; i < n; ++i)
        if (isFree[i]) freeNodes.push_back(i);
    for (size_t i : freeNodes) lo[i] = loGround[i];
    std::vector<float> r(n), p(n, 0.0f), ap(n);
    applyLtL(lo, r);
    for (size_t i = 0; i < n; ++i) r[i] = isFree[i] ? -r[i] : 0.0f;
    p = r;
    double rr = 0;
    for (size_t i : freeNodes) rr += double(r[i]) * r[i];
    for (int iter = 0; iter < 2000 && rr > 1e-8; ++iter)
    {
        applyLtL(p, ap);
        double pap = 0;
        for (size_t i : freeNodes) pap += double(p[i]) * ap[i];
        if (pap <= 0) break;
        const float alpha = float(rr / pap);
        double rrNew = 0;
        for (size_t i : freeNodes)
        {
            lo[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            rrNew += double(r[i]) * r[i];
        }
        const float beta = float(rrNew / rr);
        for (size_t i : freeNodes) p[i] = r[i] + beta * p[i];
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
    if (worstStep >= 2.5f || AutoBlendWidth(g) < 12.0f || kNodeYards <= 4.0f) return false;

    // The distance transform against brute force, on scattered targets of a ragged grid.
    BlendGrid d;
    d.width = 37;
    d.height = 23;
    std::vector<std::pair<int, int>> targets;
    for (int k = 0; k < 9; ++k) targets.push_back({ (k * 17 + 3) % d.width, (k * 11 + 5) % d.height });
    const std::vector<float> fast = DistanceTo(d, targets);
    for (int y = 0; y < d.height; ++y)
        for (int x = 0; x < d.width; ++x)
        {
            int best = 1 << 30;
            for (const auto& [tx, ty] : targets) best = std::min(best, (tx - x) * (tx - x) + (ty - y) * (ty - y));
            if (fast[d.At(x, y)] != std::sqrt(float(best))) return false;
        }
    return true;
}
