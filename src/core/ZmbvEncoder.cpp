#include "ZmbvEncoder.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace adm::core {
namespace {

constexpr uint8_t kFlagKeyframe = 1;
constexpr uint8_t kFormat32bpp = 8;
constexpr int kBlock = 16;
// How far the global scroll search looks, in pixels. ZMBV stores each component as a signed
// byte doubled, so anything up to 63 is representable; 16 covers any scroll a game does in a
// frame while keeping the search to a millisecond or so.
constexpr int kSearchRange = 16;
constexpr int kBytesPerPixel = 4;
// zlib level. The encoder runs on the emulation thread, and a delta frame of an Amiga screen is
// mostly zeros, which every level compresses about as well; keyframes are where the level
// shows, and one arrives every few seconds.
constexpr int kCompressionLevel = 4;

struct Vector
{
    int dx = 0;
    int dy = 0;
    bool operator==(const Vector& o) const { return dx == o.dx && dy == o.dy; }
};

} // namespace

struct ZmbvEncoder::Impl
{
    int width = 0;
    int height = 0;
    int keyframeInterval = 300;
    int frameIndex = 0; // since the last keyframe
    bool active = false;
    z_stream zs{};
    bool zsInitialised = false;
    std::vector<uint8_t> prev; // the frame the next delta is taken against
    std::vector<uint8_t> work; // uncompressed payload being assembled
    std::vector<uint8_t> packet;
    std::vector<Vector> candidates;
    std::vector<Vector> lastVectors; // per block, from the previous delta frame
};

ZmbvEncoder::ZmbvEncoder() : m_impl(std::make_unique<Impl>()) {}

ZmbvEncoder::~ZmbvEncoder() { End(); }

bool ZmbvEncoder::Begin(int width, int height, int keyframeInterval, std::string& outError)
{
    End();
    auto& s = *m_impl;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        outError = "ZMBV needs a frame size between 1 and 8192";
        return false;
    }
    s.width = width;
    s.height = height;
    s.keyframeInterval = std::max(1, keyframeInterval);
    s.frameIndex = 0;

    s.zs = z_stream{};
    if (deflateInit(&s.zs, kCompressionLevel) != Z_OK) {
        outError = "zlib would not initialise";
        return false;
    }
    s.zsInitialised = true;

    const size_t frameBytes = static_cast<size_t>(width) * height * kBytesPerPixel;
    const int bx = (width + kBlock - 1) / kBlock;
    const int by = (height + kBlock - 1) / kBlock;
    const size_t vectorBytes = (static_cast<size_t>(bx) * by * 2 + 3) & ~size_t{3};
    s.prev.assign(frameBytes, 0);
    s.lastVectors.assign(static_cast<size_t>(bx) * by, Vector{});
    s.work.resize(vectorBytes + frameBytes);
    s.packet.resize(MaxPacketSize());
    s.active = true;
    return true;
}

void ZmbvEncoder::End()
{
    auto& s = *m_impl;
    if (s.zsInitialised) {
        deflateEnd(&s.zs);
        s.zsInitialised = false;
    }
    s.active = false;
}

size_t ZmbvEncoder::MaxPacketSize() const
{
    const auto& s = *m_impl;
    const size_t frameBytes = static_cast<size_t>(s.width) * s.height * kBytesPerPixel;
    const int bx = (s.width + kBlock - 1) / kBlock;
    const int by = (s.height + kBlock - 1) / kBlock;
    const size_t payload = ((static_cast<size_t>(bx) * by * 2 + 3) & ~size_t{3}) + frameBytes;
    // flags + keyframe header, zlib's own bound, and the sync-flush marker it does not count.
    return 1 + 6 + deflateBound(const_cast<z_streamp>(&s.zs), static_cast<uLong>(payload)) + 64;
}

namespace {

// Does the whole block at (x, y) in `cur` equal the one at (x+dx, y+dy) in `prev`?
bool BlockMatches(const uint8_t* cur, const uint8_t* prev, int stride, int x, int y, int bw, int bh,
                  int dx, int dy)
{
    const size_t rowBytes = static_cast<size_t>(bw) * kBytesPerPixel;
    for (int j = 0; j < bh; ++j) {
        const uint8_t* a = cur + (static_cast<size_t>(y + j) * stride + x) * kBytesPerPixel;
        const uint8_t* b =
            prev + (static_cast<size_t>(y + j + dy) * stride + (x + dx)) * kBytesPerPixel;
        if (std::memcmp(a, b, rowBytes) != 0)
            return false;
    }
    return true;
}

// How many pixels of the block differ - the cost of XOR-coding it against that offset.
int BlockDifference(const uint8_t* cur, const uint8_t* prev, int stride, int x, int y, int bw,
                    int bh, int dx, int dy)
{
    int differing = 0;
    for (int j = 0; j < bh; ++j) {
        const uint8_t* a = cur + (static_cast<size_t>(y + j) * stride + x) * kBytesPerPixel;
        const uint8_t* b =
            prev + (static_cast<size_t>(y + j + dy) * stride + (x + dx)) * kBytesPerPixel;
        for (int i = 0; i < bw; ++i) {
            if (std::memcmp(a + i * kBytesPerPixel, b + i * kBytesPerPixel, 3) != 0)
                ++differing;
        }
    }
    return differing;
}

// The offsets that best explain how the frame moved since the last one, estimated on a sparse
// sample. A scrolling playfield moves as a whole, so a handful of vectors found once per frame
// match most blocks - and a game with parallax has one per layer, which is why this returns the
// peaks of the score curves rather than only the best. Each block then tries these few candidates
// instead of searching on its own, which ffmpeg's encoder does at about 60 ms a frame.
void EstimateMotionCandidates(const uint8_t* cur, const uint8_t* prev, int w, int h,
                              std::vector<Vector>& out)
{
    constexpr int kStepX = 4;
    constexpr int kStepY = 8;
    constexpr int kPeaksPerAxis = 3;
    const auto score = [&](int dx, int dy) {
        int matches = 0;
        const int y0 = std::max(0, -dy);
        const int y1 = std::min(h, h - dy);
        const int x0 = std::max(0, -dx);
        const int x1 = std::min(w, w - dx);
        for (int y = y0; y < y1; y += kStepY) {
            const uint8_t* a = cur + static_cast<size_t>(y) * w * kBytesPerPixel;
            const uint8_t* b = prev + static_cast<size_t>(y + dy) * w * kBytesPerPixel;
            for (int x = x0; x < x1; x += kStepX) {
                if (std::memcmp(a + x * kBytesPerPixel, b + (x + dx) * kBytesPerPixel, 3) == 0)
                    ++matches;
            }
        }
        return matches;
    };

    const int n = 2 * kSearchRange + 1;
    std::vector<int> rowScore(n), colScore(n);
    for (int d = -kSearchRange; d <= kSearchRange; ++d) {
        rowScore[d + kSearchRange] = score(d, 0);
        colScore[d + kSearchRange] = score(0, d);
    }

    // The strongest local maxima of a score curve, zero excluded (it is always tried anyway).
    const auto peaks = [&](const std::vector<int>& curve) {
        std::vector<std::pair<int, int>> found; // score, offset
        for (int i = 0; i < n; ++i) {
            const int d = i - kSearchRange;
            if (d == 0 || curve[i] == 0)
                continue;
            const int left = i > 0 ? curve[i - 1] : -1;
            const int right = i + 1 < n ? curve[i + 1] : -1;
            if (curve[i] >= left && curve[i] >= right)
                found.emplace_back(curve[i], d);
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        if (found.size() > kPeaksPerAxis)
            found.resize(kPeaksPerAxis);
        return found;
    };
    const auto rowPeaks = peaks(rowScore);
    const auto colPeaks = peaks(colScore);

    const auto add = [&](int dx, int dy) {
        if (dx == 0 && dy == 0)
            return;
        const Vector v{dx, dy};
        if (std::find(out.begin(), out.end(), v) == out.end())
            out.push_back(v);
    };

    // A diagonal scroll: the best row and column offsets together, and their neighbours.
    if (!rowPeaks.empty() && !colPeaks.empty()) {
        Vector best{};
        int bestScore = -1;
        for (int dy = colPeaks[0].second - 1; dy <= colPeaks[0].second + 1; ++dy) {
            for (int dx = rowPeaks[0].second - 1; dx <= rowPeaks[0].second + 1; ++dx) {
                if (dx < -kSearchRange || dx > kSearchRange || dy < -kSearchRange ||
                    dy > kSearchRange || (dx == 0 && dy == 0))
                    continue;
                const int sc = score(dx, dy);
                if (sc > bestScore) {
                    bestScore = sc;
                    best = {dx, dy};
                }
            }
        }
        const int axisBest = std::max(rowPeaks[0].first, colPeaks[0].first);
        if (bestScore > axisBest)
            add(best.dx, best.dy);
    }
    for (const auto& [sc, d] : rowPeaks)
        add(d, 0);
    for (const auto& [sc, d] : colPeaks)
        add(0, d);
}

} // namespace

ZmbvEncoder::Packet ZmbvEncoder::EncodeFrame(const uint8_t* bgrx)
{
    auto& s = *m_impl;
    m_lastMovedBlocks = 0;
    m_lastCleanBlocks = 0;
    if (!s.active || !bgrx)
        return {};

    const int w = s.width;
    const int h = s.height;
    const size_t frameBytes = static_cast<size_t>(w) * h * kBytesPerPixel;
    const bool keyframe = s.frameIndex == 0;
    s.frameIndex = (s.frameIndex + 1) % s.keyframeInterval;

    // Sized once in Begin; resizing it per frame would zero megabytes every frame.
    uint8_t* out = s.packet.data();
    size_t outLen = 0;
    out[outLen++] = keyframe ? kFlagKeyframe : 0;

    size_t workLen = 0;
    if (keyframe) {
        // A keyframe starts a fresh zlib stream, so a decoder can begin here.
        deflateReset(&s.zs);
        out[outLen++] = 0; // version, high
        out[outLen++] = 1; // version, low
        out[outLen++] = 1; // compression: zlib
        out[outLen++] = kFormat32bpp;
        out[outLen++] = kBlock;
        out[outLen++] = kBlock;
        std::memcpy(s.work.data(), bgrx, frameBytes);
        workLen = frameBytes;
        std::fill(s.lastVectors.begin(), s.lastVectors.end(), Vector{});
    } else {
        const int bx = (w + kBlock - 1) / kBlock;
        const int by = (h + kBlock - 1) / kBlock;
        const size_t vectorBytes = (static_cast<size_t>(bx) * by * 2 + 3) & ~size_t{3};
        std::memset(s.work.data(), 0, vectorBytes);
        uint8_t* mv = s.work.data();
        workLen = vectorBytes;

        std::vector<Vector>& candidates = s.candidates;
        candidates.clear();
        EstimateMotionCandidates(bgrx, s.prev.data(), w, h, candidates);

        const auto inBounds = [&](int x, int y, int bw, int bh, const Vector& v) {
            // The source block has to lie wholly inside the previous frame: a decoder zeroes
            // whatever falls outside, which is never what we want.
            return x + v.dx >= 0 && y + v.dy >= 0 && x + v.dx + bw <= w && y + v.dy + bh <= h;
        };

        int blockIndex = 0;
        for (int y = 0; y < h; y += kBlock) {
            const int bh = std::min(kBlock, h - y);
            for (int x = 0; x < w; x += kBlock, mv += 2, ++blockIndex) {
                const int bw = std::min(kBlock, w - x);
                const uint8_t* prevFrame = s.prev.data();

                Vector chosen{};
                bool xored = true;
                const Vector last = s.lastVectors[blockIndex];
                const auto tryExact = [&](const Vector& v) {
                    if (!inBounds(x, y, bw, bh, v) ||
                        !BlockMatches(bgrx, prevFrame, w, x, y, bw, bh, v.dx, v.dy))
                        return false;
                    chosen = v;
                    xored = false;
                    return true;
                };
                // Unchanged first - the common case - then whatever this block did last frame (a
                // parallax layer keeps its speed), then the frame's scroll candidates.
                bool found = tryExact(Vector{}) || (!(last == Vector{}) && tryExact(last));
                for (size_t c = 0; !found && c < candidates.size(); ++c) {
                    if (!(candidates[c] == last))
                        found = tryExact(candidates[c]);
                }
                // Nothing matched as it stood: look a couple of pixels around each likely
                // source. A sprite or a layer that moved by a different amount than the frame
                // lands here, and an exact match anywhere near is far cheaper than an XOR.
                if (!found) {
                    constexpr int kRefine = 2;
                    Vector seeds[3] = {Vector{}, last,
                                       candidates.empty() ? Vector{} : candidates[0]};
                    for (int si = 0; !found && si < 3; ++si) {
                        if (si > 0 && seeds[si] == seeds[0])
                            continue;
                        if (si == 2 && seeds[2] == seeds[1])
                            continue;
                        for (int ry = -kRefine; !found && ry <= kRefine; ++ry) {
                            for (int rx = -kRefine; !found && rx <= kRefine; ++rx) {
                                if (rx == 0 && ry == 0)
                                    continue;
                                const Vector v{seeds[si].dx + rx, seeds[si].dy + ry};
                                if (v.dx < -63 || v.dx > 63 || v.dy < -63 || v.dy > 63)
                                    continue;
                                found = tryExact(v);
                            }
                        }
                    }
                }
                if (!found) {
                    // No exact source: XOR against whichever of the likely ones differs least.
                    int bestDiff = BlockDifference(bgrx, prevFrame, w, x, y, bw, bh, 0, 0);
                    const auto consider = [&](const Vector& v) {
                        if ((v == Vector{}) || !inBounds(x, y, bw, bh, v))
                            return;
                        const int d = BlockDifference(bgrx, prevFrame, w, x, y, bw, bh, v.dx, v.dy);
                        if (d < bestDiff) {
                            bestDiff = d;
                            chosen = v;
                        }
                    };
                    consider(last);
                    if (!candidates.empty())
                        consider(candidates[0]);
                }
                s.lastVectors[blockIndex] = chosen;
                const int dx = chosen.dx;
                const int dy = chosen.dy;

                mv[0] = static_cast<uint8_t>((dx * 2) | (xored ? 1 : 0));
                mv[1] = static_cast<uint8_t>(dy * 2);
                if (dx != 0 || dy != 0)
                    ++m_lastMovedBlocks;
                if (!xored) {
                    ++m_lastCleanBlocks;
                    continue;
                }
                // The XOR of the block against its source, row by row.
                for (int j = 0; j < bh; ++j) {
                    const uint8_t* a = bgrx + (static_cast<size_t>(y + j) * w + x) * kBytesPerPixel;
                    const uint8_t* b =
                        s.prev.data() +
                        (static_cast<size_t>(y + j + dy) * w + (x + dx)) * kBytesPerPixel;
                    uint8_t* dst = s.work.data() + workLen;
                    const size_t rowBytes = static_cast<size_t>(bw) * kBytesPerPixel;
                    for (size_t i = 0; i < rowBytes; ++i)
                        dst[i] = a[i] ^ b[i];
                    workLen += rowBytes;
                }
            }
        }
    }

    std::memcpy(s.prev.data(), bgrx, frameBytes);

    s.zs.next_in = s.work.data();
    s.zs.avail_in = static_cast<uInt>(workLen);
    s.zs.next_out = out + outLen;
    s.zs.avail_out = static_cast<uInt>(s.packet.size() - outLen);
    const uLong before = s.zs.total_out;
    // Z_SYNC_FLUSH, not Z_FINISH: delta frames continue this stream, and the decoder inflates
    // each packet with Z_SYNC_FLUSH too.
    if (deflate(&s.zs, Z_SYNC_FLUSH) != Z_OK || s.zs.avail_in != 0) {
        s.active = false; // the stream is now out of step with any decoder; stop producing
        return {};
    }
    outLen += static_cast<size_t>(s.zs.total_out - before);
    return {out, outLen, keyframe};
}

} // namespace adm::core
