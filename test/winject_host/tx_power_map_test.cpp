#include "tx_power_map.h"

#include <gtest/gtest.h>

using tx_power_map::curve;
using tx_power_map::dbm_for_qdbm;
using tx_power_map::dbm_x100_for_qdbm;
using tx_power_map::qdbm_for_dbm;

static curve make(std::initializer_list<tx_power_map::point> pts)
{
    curve k;
    for (const auto& p : pts)
    {
        k.p[k.n++] = p;
    }
    return k;
}

// DSSS-like curve with a step between 66 and 72.
static const curve k_step = make({{8, -2800}, {66, 260}, {72, 950}, {84, 1560}});

TEST(TxPowerMapTest, InterpolatesBetweenPoints)
{
    EXPECT_EQ(dbm_x100_for_qdbm(8, k_step), -2800);
    EXPECT_EQ(dbm_x100_for_qdbm(66, k_step), 260);
    EXPECT_EQ(dbm_x100_for_qdbm(69, k_step), 605);   // halfway up the step
    EXPECT_EQ(dbm_x100_for_qdbm(78, k_step), 1255);
    EXPECT_EQ(dbm_for_qdbm(84, k_step), 16);
}

TEST(TxPowerMapTest, ExtendsAlongEndSegments)
{
    EXPECT_EQ(dbm_x100_for_qdbm(88, k_step), 1763);  // 1560 + 4 * 610 / 12
    const curve two = make({{20, -2000}, {60, 0}});
    EXPECT_EQ(dbm_x100_for_qdbm(8, two), -2600);
    EXPECT_EQ(dbm_x100_for_qdbm(88, two), 1400);
}

TEST(TxPowerMapTest, RequestPicksNearestDriverValue)
{
    EXPECT_EQ(qdbm_for_dbm(-28, k_step), 8);
    EXPECT_EQ(qdbm_for_dbm(10, k_step), 73);
    EXPECT_EQ(qdbm_for_dbm(6, k_step), 69);   // inside the step: nearest
    EXPECT_EQ(qdbm_for_dbm(30, k_step), 88);  // clamped to the boost cap
    EXPECT_EQ(qdbm_for_dbm(30, k_step, tx_power_map::k_qdbm_legacy_64qam_max), 52);
    EXPECT_EQ(qdbm_for_dbm(-40, k_step), 8);
}

TEST(TxPowerMapTest, RoundTripWithinHalfStep)
{
    for (int dbm = -27; dbm <= 15; ++dbm)
    {
        const int q = qdbm_for_dbm(dbm, k_step);
        const int back = dbm_x100_for_qdbm(q, k_step);
        // ~0.5 dB per unit, except across the 66..72 step (1.15 dB per unit).
        EXPECT_LE(std::abs(back - dbm * 100), 60) << dbm;
    }
}

TEST(TxPowerMapTest, CurveValidation)
{
    EXPECT_TRUE(tx_power_map::curve_valid(k_step));
    EXPECT_FALSE(tx_power_map::curve_valid(make({{8, 0}})));                    // one point
    EXPECT_FALSE(tx_power_map::curve_valid(make({{20, 0}, {20, 100}})));        // q not rising
    EXPECT_FALSE(tx_power_map::curve_valid(make({{20, 100}, {30, 0}})));        // output falls
    EXPECT_FALSE(tx_power_map::curve_valid(make({{4, 0}, {30, 100}})));         // q < 8
    EXPECT_FALSE(tx_power_map::curve_valid(make({{8, 0}, {89, 100}})));         // q > 88
    EXPECT_TRUE(tx_power_map::curve_valid(make({{8, 0}, {30, 0}})));            // flat ok
    EXPECT_TRUE(tx_power_map::cal_valid(tx_power_map::default_cal()));
    EXPECT_TRUE(tx_power_map::cal_is_default(tx_power_map::default_cal()));
}

TEST(TxPowerMapTest, LineCurve)
{
    const curve k = tx_power_map::line_curve(2000, -2700);
    ASSERT_EQ(k.n, 2);
    EXPECT_EQ(k.p[0].q, 8);
    EXPECT_EQ(k.p[0].dbm_x100, -2300);
    EXPECT_EQ(k.p[1].q, 88);
    EXPECT_EQ(k.p[1].dbm_x100, 1700);
    EXPECT_EQ(dbm_for_qdbm(84, k), 15);
}
