//*****************************************************************************
//
//! @file map_route.h
//!
//! @brief Navigation route from Taipei 101 to Taipei Main Station.
//!
//! The route itself is already painted into the base map (the cyan line,
//! #01C1FE). The polyline here holds the coordinates of that same line so the
//! navigation simulation knows where the vehicle should go and when it turns --
//! the base map is only pixels, and nothing can derive "how far to the next
//! junction" from it.
//!
//! Where the coordinates come from: the three tiles are stitched horizontally
//! into a 1404x468 world image, the largest connected region matching the route
//! colour is extracted, and its top and bottom edges are read column by column.
//! On a horizontal segment the column height equals the line width (11 px); at a
//! corner it jumps to several tens of pixels, and those positions are the
//! vertices. The centre of the line width is taken as the node.
//!
//! World coordinates: the origin is the top-left of the stitched image, x runs
//! right and y runs down, in pixels.
//
//*****************************************************************************

#ifndef MAP_ROUTE_H
#define MAP_ROUTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//*****************************************************************************
//
//! @brief A single route node.
//
//*****************************************************************************
typedef struct
{
    int16_t i16X;
    int16_t i16Y;
} map_pt_t;

//*****************************************************************************
//
//! @brief Route nodes in travel order (Taipei 101 -> Taipei Main Station).
//!
//! @details The start is Taipei 101 in the lower right and the destination is
//! Taipei Main Station in the upper left, with the path running north-west along
//! Xinyi Road and Zhongxiao East Road. Every segment is horizontal or vertical,
//! which is how the route is drawn on the base map, so there are no diagonals to
//! handle and the turn direction follows from comparing the axes of two
//! consecutive segments.
//
//*****************************************************************************
#define MAP_ROUTE_PT_COUNT      (10)

static const map_pt_t g_sMapRoute[MAP_ROUTE_PT_COUNT] =
{
    { 1300, 340 },      // Taipei 101 (Xinyi Road Section 5)
    { 1232, 340 },
    { 1232, 253 },      // Turn north, towards Sun Yat-sen Memorial Hall
    { 1023, 253 },
    { 1023, 197 },      // Join Zhongxiao East Road
    {  342, 197 },      // Due west, past the north side of Daan Forest Park
    {  342, 150 },
    {  198, 150 },
    {  198, 105 },      // Turn into Guanqian Road
    {  148, 105 }       // Taipei Main Station
};

//*****************************************************************************
//
//! @brief Index of the start and the destination on the route.
//!
//! @details Written as constants rather than plain 0 and COUNT-1 because the
//! on-screen markers and the HUD text must all point at the same place; extending
//! or reversing the route later means changing only these.
//
//*****************************************************************************
#define MAP_ROUTE_START_IDX     (0)
#define MAP_ROUTE_END_IDX       (MAP_ROUTE_PT_COUNT - 1)

//*****************************************************************************
//
//! @brief Landmark names.
//
//*****************************************************************************
#define MAP_ROUTE_ORIGIN_NAME   "Taipei 101"
#define MAP_ROUTE_DEST_NAME     "Taipei Main Station"

//*****************************************************************************
//
//! @brief Scale: how many metres one pixel represents.
//!
//! @details The real route is about 6.5 km long and the polyline totals 1387 px,
//! giving 4.7 m/px. The "x.x km remaining" on the HUD is converted with it.
//!
//! This value affects only the displayed number and no geometry -- map panning
//! and hit testing are all done in pixel coordinates.
//
//*****************************************************************************
#define MAP_METERS_PER_PX       (4.7f)

#ifdef __cplusplus
}
#endif

#endif // MAP_ROUTE_H
