#include "AnnexB.h"

namespace dd::annexb
{
namespace
{

// Returns the index of the next start code at or after `from`, and its length (3 or 4).
bool FindStartCode(std::span<const uint8_t> d, size_t from, size_t& pos, size_t& len)
{
    for (size_t i = from; i + 3 <= d.size(); ++i)
    {
        if (d[i] == 0 && d[i + 1] == 0)
        {
            if (d[i + 2] == 1)
            {
                pos = i;
                len = 3;
                if (i > from && d[i - 1] == 0)
                {
                    pos = i - 1;
                    len = 4;
                }
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<Nal> Split(std::span<const uint8_t> data, Codec codec)
{
    std::vector<Nal> nals;
    size_t pos = 0;
    size_t len = 0;
    if (!FindStartCode(data, 0, pos, len))
    {
        return nals;
    }

    while (true)
    {
        const size_t payload = pos + len;
        size_t nextPos = data.size();
        size_t nextLen = 0;
        const bool more = FindStartCode(data, payload, nextPos, nextLen);
        if (!more)
        {
            nextPos = data.size();
        }

        Nal nal;
        nal.offset = pos;
        nal.size = nextPos - pos;
        if (payload < data.size())
        {
            nal.type = codec == Codec::Hevc ? static_cast<uint8_t>((data[payload] >> 1) & 0x3F)
                                            : static_cast<uint8_t>(data[payload] & 0x1F);
        }
        nals.push_back(nal);

        if (!more)
        {
            break;
        }
        pos = nextPos;
        len = nextLen;
    }
    return nals;
}

SplitAccessUnit SeparateParameterSets(std::span<const uint8_t> accessUnit, Codec codec)
{
    const bool hevc = codec == Codec::Hevc;
    const auto isSps = [&](uint8_t t) { return hevc ? t == kHevcNalSps : t == kNalSps; };
    const auto isParameterSet = [&](uint8_t t) {
        return hevc ? (t == kHevcNalVps || t == kHevcNalSps || t == kHevcNalPps) : (t == kNalSps || t == kNalPps);
    };
    // HEVC IRAP pictures (BLA/IDR/CRA) are types 16..21.
    const auto isKeyframe = [&](uint8_t t) { return hevc ? (t >= 16 && t <= 21) : t == kNalIdr; };

    SplitAccessUnit out;
    out.frame.reserve(accessUnit.size());
    const auto nals = Split(accessUnit, codec);

    // Some encoders (Intel QSV) repeat a lone PPS on ordinary frames. Only a full set including
    // the SPS is worth a CODEC_CONFIG message; a lone PPS stays inline, which decoders handle fine.
    bool hasSps = false;
    for (const Nal& nal : nals)
    {
        hasSps = hasSps || isSps(nal.type);
    }

    for (const Nal& nal : nals)
    {
        const auto bytes = accessUnit.subspan(nal.offset, nal.size);
        if (hasSps && isParameterSet(nal.type))
        {
            out.codecConfig.insert(out.codecConfig.end(), bytes.begin(), bytes.end());
        }
        else
        {
            out.frame.insert(out.frame.end(), bytes.begin(), bytes.end());
            out.keyframe = out.keyframe || isKeyframe(nal.type);
        }
    }
    return out;
}

} // namespace dd::annexb
