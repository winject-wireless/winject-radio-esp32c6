#ifndef WINJECT_TX_POWER_MAP_H_
#define WINJECT_TX_POWER_MAP_H_

// Normalized TX power (dBm at the C6 antenna connector) <-> the C6 driver's
// power value q (esp_wifi_set_max_tx_power / libphy most_tpw).
//
// q is not 0.25 dBm on the C6 (IDF 5.5.5): output rises ~2 dB per 4 units,
// differs per rate group, and DSSS has a ~4 dB step between q 66 and 72.
// Each rate group therefore has its own calibration curve: points (q, dBm)
// joined by straight lines and extended past the ends along the end segments.
// Curves are per board, set over the m-plane (radio_tx_cal) and kept in NVS;
// the defaults were measured on the rover C6 (calibrated ESP32 power meter,
// settled output: after a change the C6 steps ~3 dB within 10..90 s).
//
// Pure integer code (host-tested).

#include <stddef.h>
#include <stdint.h>

#include <initializer_list>

namespace tx_power_map
{

enum class rate_class : uint8_t
{
    dsss,        // 1/2 M DSSS
    cck,         // 5.5/11 M CCK
    ofdm_low,    // OFDM 6..18 M, MCS0..2 (BPSK/QPSK)
    ofdm_mid,    // OFDM 24/36 M, MCS3..4 (16-QAM)
    ofdm_64qam,  // OFDM 48/54 M, MCS5..7
};
constexpr size_t k_classes = 5;

// m-plane names, indexed by rate_class.
inline const char* class_name(rate_class c)
{
    static const char* const k_names[k_classes] = {"dsss", "cck", "ofdm_low", "ofdm",
                                                   "ofdm_64qam"};
    return k_names[static_cast<size_t>(c)];
}

constexpr int k_qdbm_min = 8;       // driver minimum
constexpr int k_qdbm_api_max = 84;  // esp_wifi_set_max_tx_power maximum
constexpr int k_qdbm_max = 88;      // boost cap (libphy/libpp set directly; 96+ stops TX)
// Legacy 48/54 M raw inject clips above this (peer promisc RX fails).
constexpr int k_qdbm_legacy_64qam_max = 52;

constexpr size_t k_max_points = 12;
constexpr int32_t k_dbm_x100_min = -6000;
constexpr int32_t k_dbm_x100_max = 3000;

struct point
{
    uint8_t q = 0;
    int16_t dbm_x100 = 0;  // output at q, 0.01 dBm
};

// n >= 2 points, q strictly rising within [k_qdbm_min, k_qdbm_max], output
// not falling (so every request maps to one nearest q).
struct curve
{
    uint8_t n = 0;
    point p[k_max_points];
};

struct cal
{
    curve c[k_classes];
};

inline bool curve_valid(const curve& k)
{
    if (k.n < 2 || k.n > k_max_points)
    {
        return false;
    }
    for (size_t i = 0; i < k.n; i++)
    {
        if (k.p[i].q < k_qdbm_min || k.p[i].q > k_qdbm_max ||
            k.p[i].dbm_x100 < k_dbm_x100_min || k.p[i].dbm_x100 > k_dbm_x100_max)
        {
            return false;
        }
        if (i > 0 && (k.p[i].q <= k.p[i - 1].q || k.p[i].dbm_x100 < k.p[i - 1].dbm_x100))
        {
            return false;
        }
    }
    return true;
}

inline bool curves_equal(const curve& a, const curve& b)
{
    if (a.n != b.n)
    {
        return false;
    }
    for (size_t i = 0; i < a.n; i++)
    {
        if (a.p[i].q != b.p[i].q || a.p[i].dbm_x100 != b.p[i].dbm_x100)
        {
            return false;
        }
    }
    return true;
}

// Straight line through (q0, dbm0) with `scale_milli` output dB per 4 units,
// as a two-point curve over the whole driver range.
inline curve line_curve(int32_t scale_milli, int32_t dbm_x100_at_q0)
{
    curve k;
    k.n = 2;
    k.p[0].q = static_cast<uint8_t>(k_qdbm_min);
    k.p[0].dbm_x100 = static_cast<int16_t>(dbm_x100_at_q0 + k_qdbm_min * scale_milli / 40);
    k.p[1].q = static_cast<uint8_t>(k_qdbm_max);
    k.p[1].dbm_x100 = static_cast<int16_t>(dbm_x100_at_q0 + k_qdbm_max * scale_milli / 40);
    return k;
}

inline curve default_curve(rate_class c)
{
    curve k;
    auto set = [&k](std::initializer_list<point> pts) {
        k.n = 0;
        for (const point& p : pts)
        {
            k.p[k.n++] = p;
        }
    };
    // Rover C6 (Waveshare), 2026-11-04: direct into 30.9 dB -> ESP32 power
    // meter, output settled for 60 s per point, channel 1, PHY data low88.
    // Points below -12 dBm were masked by other channel-1 traffic; the curve
    // continues along its first segment. Driver value 88 adds +0.7 dB over
    // 84 (measured on OFDM 24M only, assumed for the other groups).
    // DSSS and OFDM 6M jump ~8 dB between 52 and 60 here; an earlier,
    // unsettled sweep put that step between 66 and 72, so near it expect a
    // few dB of error. ofdm_64qam: OFDM 54M read 4 s after each change (not
    // settled), capped at 52 for 48/54M.
    switch (c)
    {
    case rate_class::dsss:
        set({{36, -1431}, {44, -1039}, {52, -636}, {60, 220}, {68, 637}, {76, 1033},
             {84, 1414}, {88, 1484}});
        break;
    case rate_class::cck:
        set({{36, -1434}, {44, -1040}, {52, -632}, {60, -204}, {68, 247}, {76, 667},
             {84, 1142}, {88, 1212}});
        break;
    case rate_class::ofdm_low:
        set({{36, -1394}, {44, -1005}, {52, -596}, {60, 231}, {68, 648}, {76, 1043},
             {84, 1422}, {88, 1492}});
        break;
    case rate_class::ofdm_mid:
        set({{39, -1288}, {47, -900}, {55, -486}, {63, -76}, {71, 428}, {79, 857},
             {88, 1361}});
        break;
    case rate_class::ofdm_64qam:
        set({{9, -2834}, {19, -2376}, {29, -1728}, {39, -1305}, {47, -907}, {52, -560}});
        break;
    }
    return k;
}

inline cal default_cal()
{
    cal k;
    for (size_t i = 0; i < k_classes; i++)
    {
        k.c[i] = default_curve(static_cast<rate_class>(i));
    }
    return k;
}

inline bool cal_valid(const cal& k)
{
    for (size_t i = 0; i < k_classes; i++)
    {
        if (!curve_valid(k.c[i]))
        {
            return false;
        }
    }
    return true;
}

inline bool curve_is_default(const curve& k, rate_class c)
{
    return curves_equal(k, default_curve(c));
}

inline bool cal_is_default(const cal& k)
{
    for (size_t i = 0; i < k_classes; i++)
    {
        if (!curve_is_default(k.c[i], static_cast<rate_class>(i)))
        {
            return false;
        }
    }
    return true;
}

inline int32_t div_round(int32_t num, int32_t den)
{
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

// Output in 0.01 dBm for driver value q (curve must be valid).
inline int32_t dbm_x100_for_qdbm(int q, const curve& k)
{
    size_t i = 1;
    while (i + 1 < k.n && q > k.p[i].q)
    {
        i++;
    }
    const point& a = k.p[i - 1];
    const point& b = k.p[i];
    return a.dbm_x100 + div_round((q - a.q) * (b.dbm_x100 - a.dbm_x100), b.q - a.q);
}

// Rounded to whole dBm (for m-plane replies).
inline int dbm_for_qdbm(int q, const curve& k)
{
    return div_round(dbm_x100_for_qdbm(q, k), 100);
}

// Driver value whose output is nearest the requested dBm, within
// [k_qdbm_min, q_max]; on a tie the lower one.
inline int qdbm_for_dbm(int dbm, const curve& k, int q_max = k_qdbm_max)
{
    const int32_t target = dbm * 100;
    int best = k_qdbm_min;
    int32_t best_err = INT32_MAX;
    for (int q = k_qdbm_min; q <= q_max; q++)
    {
        const int32_t d = dbm_x100_for_qdbm(q, k) - target;
        const int32_t err = d < 0 ? -d : d;
        if (err < best_err)
        {
            best = q;
            best_err = err;
        }
    }
    return best;
}

}  // namespace tx_power_map

#endif  // WINJECT_TX_POWER_MAP_H_
