#pragma once

#include <string>
#include <vector>
#include <libpq-fe.h>
#include "headers/data.h"

namespace AltitudeService {

bool IsValidAltitude(const std::string& alt);

// Ensures telemetry.alt exists (DOUBLE PRECISION).
void EnsureAltitudeColumn(PGconn* con);

// Open-Meteo elevation API (no API key). Returns false on failure.
bool FetchElevationsOpenMeteo(const std::vector<std::pair<double, double>>& latLon,
                              std::vector<double>& outElevationsM);

// Fills missing alt in rows via API and writes values to PostgreSQL.
void BackfillMissingAltitudes(PGconn* con, std::vector<TelemetryData>& rows);

} // namespace AltitudeService
