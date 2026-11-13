#include "ScenarioFile.h"

#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "ByteStream.h"
#include "FrameInputCodec.h"

namespace adm::core {

namespace {

constexpr char kMagic[4] = {'P', 'K', 'S', 'C'};

void PutTag(std::vector<uint8_t>& out, const char (&tag)[5])
{
    out.insert(out.end(), tag, tag + 4);
}

// A chunk is written as tag + length + body. The body is built first because its length is
// part of the header, and every chunk here is small enough for that to be free next to the
// save state it sits beside.
void PutChunk(std::vector<uint8_t>& out, const char (&tag)[5], const std::vector<uint8_t>& body)
{
    PutTag(out, tag);
    PutLE<uint64_t>(out, static_cast<uint64_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

nlohmann::json ManifestToJson(const ScenarioManifest& m, uint64_t frameCount)
{
    nlohmann::json j;
    j["created"] = m.created;
    j["coreVersion"] = m.coreVersion;
    j["appCommit"] = m.appCommit;
    j["model"] = m.model;
    j["videoStandard"] = m.videoStandard;
    j["videoResolution"] = m.videoResolution;
    j["userVariables"] = m.userVariables;
    j["frameCount"] = frameCount;
    j["seed"] = m.coldBoot ? "coldboot" : "savestate";
    j["notes"] = m.notes;

    nlohmann::json media = nlohmann::json::array();
    for (const auto& item : m.media) {
        nlohmann::json entry;
        entry["drive"] = item.drive;
        entry["name"] = item.name;
        entry["size"] = item.size;
        entry["md5"] = item.md5;
        media.push_back(std::move(entry));
    }
    j["media"] = std::move(media);
    return j;
}

void ReadManifestFields(const nlohmann::json& j, ScenarioManifest& m);

bool ManifestFromJson(const nlohmann::json& j, ScenarioManifest& m, std::string& error)
{
    if (!j.is_object()) {
        error = "manifest is not a JSON object";
        return false;
    }
    // A wrong-typed field (hand-edited, or from a newer writer) is an error like every
    // binary-level problem, not a throw: SCEN PLAY's path has no catch.
    try {
        ReadManifestFields(j, m);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("manifest: ") + e.what();
        return false;
    }
    return true;
}

void ReadManifestFields(const nlohmann::json& j, ScenarioManifest& m)
{
    m.created = j.value("created", "");
    m.coreVersion = j.value("coreVersion", "");
    m.appCommit = j.value("appCommit", "");
    m.model = j.value("model", "");
    m.videoStandard = j.value("videoStandard", "");
    m.videoResolution = j.value("videoResolution", "");
    m.notes = j.value("notes", "");
    m.frameCount = j.value("frameCount", 0ull);
    m.coldBoot = j.value("seed", "coldboot") != "savestate";

    m.userVariables.clear();
    if (const auto it = j.find("userVariables"); it != j.end() && it->is_object())
        for (const auto& [key, value] : it->items())
            if (value.is_string())
                m.userVariables[key] = value.get<std::string>();

    m.media.clear();
    if (const auto it = j.find("media"); it != j.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (!entry.is_object())
                continue;
            ScenarioMedia item;
            item.drive = entry.value("drive", 0u);
            item.name = entry.value("name", "");
            item.size = entry.value("size", 0ull);
            item.md5 = entry.value("md5", "");
            m.media.push_back(std::move(item));
        }
    }
}

} // namespace

const ScenarioKeyframe* ScenarioGolden::NearestKeyframe(uint64_t frame) const
{
    const ScenarioKeyframe* best = nullptr;
    for (const auto& kf : keyframes) {
        if (kf.frame > frame)
            break; // written in ascending order
        best = &kf;
    }
    // Before the first keyframe there is nothing at or before `frame`; the first one after
    // it is still more use than nothing.
    if (!best && !keyframes.empty())
        best = &keyframes.front();
    return best;
}

std::vector<uint8_t> ScenarioFile::Serialize() const
{
    std::vector<uint8_t> out;
    out.reserve(seedState.size() + input.size() * 24 + 4096);
    out.insert(out.end(), kMagic, kMagic + 4);
    PutLE<uint32_t>(out, kVersion);

    {
        const std::string text =
            ManifestToJson(manifest, static_cast<uint64_t>(input.size())).dump();
        std::vector<uint8_t> body(text.begin(), text.end());
        PutChunk(out, "MANI", body);
    }

    if (!seedState.empty())
        PutChunk(out, "SEED", seedState);

    {
        std::vector<uint8_t> body;
        body.reserve(input.size() * 24);
        for (const auto& in : input)
            WriteFrameInput(body, in);
        PutChunk(out, "INPT", body);
    }

    if (!golden.Empty()) {
        std::vector<uint8_t> body;
        PutLE<uint64_t>(body, static_cast<uint64_t>(golden.frames.size()));
        for (const auto& f : golden.frames) {
            PutLE<uint64_t>(body, f.video);
            PutLE<uint64_t>(body, f.audio);
            PutLE<uint32_t>(body, f.audioSamples);
        }
        PutLE<uint64_t>(body, static_cast<uint64_t>(golden.ram.size()));
        for (const auto& r : golden.ram) {
            PutLE<uint64_t>(body, r.frame);
            PutLE<uint64_t>(body, r.digest);
        }
        PutLE<uint64_t>(body, static_cast<uint64_t>(golden.keyframes.size()));
        for (const auto& kf : golden.keyframes) {
            PutLE<uint64_t>(body, kf.frame);
            PutLE<uint32_t>(body, static_cast<uint32_t>(kf.png.size()));
            body.insert(body.end(), kf.png.begin(), kf.png.end());
        }
        PutChunk(out, "GOLD", body);
    }

    return out;
}

bool ScenarioFile::Deserialize(const uint8_t* data, size_t size, std::string& error)
{
    Clear();
    error.clear();

    if (!data || size < 8) {
        error = "not a scenario file (too short)";
        return false;
    }
    if (std::memcmp(data, kMagic, 4) != 0) {
        error = "not a scenario file (bad magic)";
        return false;
    }

    ByteReader r(data + 4, size - 4);
    const uint32_t version = r.Get<uint32_t>();
    if (!r.Ok() || version != kVersion) {
        error = "unsupported scenario version " + std::to_string(version);
        return false;
    }

    bool sawManifest = false;
    uint64_t manifestFrameCount = 0;

    while (r.Remaining() >= 12) {
        const uint8_t* tag = r.Take(4);
        const uint64_t length = r.Get<uint64_t>();
        if (!r.Ok() || length > r.Remaining()) {
            error = "truncated chunk";
            Clear();
            return false;
        }
        const uint8_t* body = r.Take(static_cast<size_t>(length));
        if (!body) {
            error = "truncated chunk body";
            Clear();
            return false;
        }
        const auto is = [tag](const char* name) { return std::memcmp(tag, name, 4) == 0; };

        if (is("MANI")) {
            const auto j = nlohmann::json::parse(body, body + length, nullptr, false);
            if (j.is_discarded() || !ManifestFromJson(j, manifest, error)) {
                if (error.empty())
                    error = "manifest is not valid JSON";
                Clear();
                return false;
            }
            manifestFrameCount = manifest.frameCount;
            sawManifest = true;
        } else if (is("SEED")) {
            seedState.assign(body, body + length);
        } else if (is("INPT")) {
            ByteReader ir(body, static_cast<size_t>(length));
            while (ir.Remaining() > 0) {
                FrameInput in;
                if (!ReadFrameInput(ir, in)) {
                    error = "malformed input stream at frame " + std::to_string(input.size());
                    Clear();
                    return false;
                }
                input.push_back(std::move(in));
            }
        } else if (is("GOLD")) {
            ByteReader gr(body, static_cast<size_t>(length));
            const uint64_t frames = gr.Get<uint64_t>();
            if (!gr.Ok() || frames > gr.Remaining() / 20) {
                error = "malformed golden table";
                Clear();
                return false;
            }
            golden.frames.reserve(static_cast<size_t>(frames));
            for (uint64_t i = 0; i < frames; ++i) {
                ScenarioFrameDigest d;
                d.video = gr.Get<uint64_t>();
                d.audio = gr.Get<uint64_t>();
                d.audioSamples = gr.Get<uint32_t>();
                golden.frames.push_back(d);
            }
            const uint64_t rams = gr.Get<uint64_t>();
            if (!gr.Ok() || rams > gr.Remaining() / 16) {
                error = "malformed golden RAM table";
                Clear();
                return false;
            }
            golden.ram.reserve(static_cast<size_t>(rams));
            for (uint64_t i = 0; i < rams; ++i) {
                ScenarioRamDigest d;
                d.frame = gr.Get<uint64_t>();
                d.digest = gr.Get<uint64_t>();
                golden.ram.push_back(d);
            }
            const uint64_t keyframes = gr.Get<uint64_t>();
            if (!gr.Ok() || keyframes > gr.Remaining() / 12) {
                error = "malformed golden keyframe table";
                Clear();
                return false;
            }
            for (uint64_t i = 0; i < keyframes; ++i) {
                ScenarioKeyframe kf;
                kf.frame = gr.Get<uint64_t>();
                const uint32_t pngLen = gr.Get<uint32_t>();
                if (!gr.Ok() || pngLen > gr.Remaining()) {
                    error = "malformed golden keyframe";
                    Clear();
                    return false;
                }
                if (const uint8_t* p = gr.Take(pngLen))
                    kf.png.assign(p, p + pngLen);
                golden.keyframes.push_back(std::move(kf));
            }
            if (!gr.Ok()) {
                error = "truncated golden table";
                Clear();
                return false;
            }
        }
        // Anything else is a chunk from a newer writer: skipped, already consumed.
    }

    if (!sawManifest) {
        error = "scenario has no manifest";
        Clear();
        return false;
    }

    // The manifest's count is what the writer believed; the input stream is what it
    // actually wrote. A disagreement means one of them is wrong and a replay would be
    // running an unknown number of frames.
    if (manifestFrameCount != input.size()) {
        error = "manifest says " + std::to_string(manifestFrameCount) +
                " frames, input stream has " + std::to_string(input.size());
        Clear();
        return false;
    }
    if (!golden.Empty() && golden.frames.size() != input.size()) {
        error = "golden table covers " + std::to_string(golden.frames.size()) +
                " frames, input stream has " + std::to_string(input.size());
        Clear();
        return false;
    }
    manifest.frameCount = input.size();
    return true;
}

bool ScenarioFile::Load(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    return Deserialize(bytes.data(), bytes.size(), error);
}

bool ScenarioFile::Save(const std::filesystem::path& path, std::string& error) const
{
    const std::vector<uint8_t> bytes = Serialize();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot write " + path.string();
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "write failed for " + path.string();
        return false;
    }
    return true;
}

void ScenarioFile::Clear()
{
    manifest = ScenarioManifest{};
    seedState.clear();
    input.clear();
    golden = ScenarioGolden{};
}

bool ScenarioFile::MachineMatches(const ScenarioManifest& other, std::string& why) const
{
    const auto mismatch = [&why](const std::string& what, const std::string& want,
                                 const std::string& got) {
        why = what + ": scenario recorded \"" + want + "\", machine is \"" + got + "\"";
        return false;
    };

    if (manifest.model != other.model)
        return mismatch("model", manifest.model, other.model);
    if (manifest.videoStandard != other.videoStandard)
        return mismatch("video standard", manifest.videoStandard, other.videoStandard);
    if (manifest.videoResolution != other.videoResolution)
        return mismatch("video resolution", manifest.videoResolution, other.videoResolution);

    if (manifest.media.size() != other.media.size()) {
        why = "media count: scenario has " + std::to_string(manifest.media.size()) +
              ", machine has " + std::to_string(other.media.size());
        return false;
    }
    for (size_t i = 0; i < manifest.media.size(); ++i) {
        const auto& want = manifest.media[i];
        const auto& got = other.media[i];
        if (want.drive != got.drive) {
            why = "media " + std::to_string(i) + ": scenario expects DF" +
                  std::to_string(want.drive) + ", machine has DF" + std::to_string(got.drive);
            return false;
        }
        // MD5 is the identity. A name difference is just a path and is not worth refusing.
        if (!want.md5.empty() && !got.md5.empty() && want.md5 != got.md5) {
            why = "DF" + std::to_string(want.drive) + ": scenario recorded \"" + want.name +
                  "\" (" + want.md5 + "), machine has \"" + got.name + "\" (" + got.md5 + ")";
            return false;
        }
    }

    why.clear();
    return true;
}

} // namespace adm::core
