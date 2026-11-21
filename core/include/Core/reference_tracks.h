#pragma once

#include <nlohmann/json_fwd.hpp>

class GNSS;
class TUM;

namespace HDMapping
{
    //! Encode GNSS measurements, the WGS84 origin and TUM poses in native session fields.
    nlohmann::json EncodeReferenceTracks(const GNSS& gnss, const TUM& tum);

    //! Read reference tracks from a session. Missing fields retain the caller's values.
    void DecodeReferenceTracks(const nlohmann::json& data, GNSS& gnss, TUM& tum);
} // namespace HDMapping
