/**
 * Extended patterns from OSSM-hardware community branches.
 * Each pattern is individually gated by a compile flag (default ON).
 *
 * Branch: cam/rad-1325-pattern-bezier
 * Source: https://github.com/KinkyMakers/OSSM-hardware/tree/cam/rad-1325-pattern-bezier
 * License: MIT (same as upstream pattern.h)
 *
 * This file is SlopDrive-32 machinery — adapted includes to patternShim.h
 * and wrapped in per-pattern #if gates. The pattern math is byte-identical
 * to the upstream branch commit.
 */

#pragma once

#include "patternShim.h"
#include <math.h>

// Per-pattern compile gates — set via platformio.ini build_flags.
// Default ON; set to 0 to disable a specific pattern if unstable.
#ifndef PATTERN_EXT_TESTPATTERN1
#define PATTERN_EXT_TESTPATTERN1 1
#endif
#ifndef PATTERN_EXT_TESTPATTERN2
#define PATTERN_EXT_TESTPATTERN2 1
#endif
#ifndef PATTERN_EXT_ARRAYPATTERN
#define PATTERN_EXT_ARRAYPATTERN 1  // base class for both — must be ON if either pattern is used
#endif

#if PATTERN_EXT_ARRAYPATTERN

// Number of samples to use when converting curved segments to linear strokes
#define BEZIER_SAMPLES_PER_SEGMENT 10

/**************************************************************************/
/*!
  @brief  struct to define a point in an array-based pattern
*/
/**************************************************************************/
typedef struct {
    float t;  //!< Time position in pattern cycle (0.0 to 1.0 where 1.0 = 100%)
    float y;  //!< Position value (0.0 to 1.0 where 0.0 = min, 1.0 = max)

    // Bezier control points for curved segments (0 = linear)
    float left_control_slope;       //!< Tangent angle entering point (radians)
    float left_control_magnitude;   //!< Tangent length entering (0.0 to 1.0)
    float right_control_slope;      //!< Tangent angle leaving point (radians)
    float right_control_magnitude;  //!< Tangent length leaving (0.0 to 1.0)
} PatternPoint;

/**************************************************************************/
/*!
  @brief  Helper function to evaluate cubic bezier curve at parameter u
  @param p0 Start point
  @param p1 First control point
  @param p2 Second control point
  @param p3 End point
  @param u Parameter from 0.0 to 1.0
  @return Interpolated value at u
*/
/**************************************************************************/
inline float cubicBezier(float p0, float p1, float p2, float p3, float u) {
    float u2 = u * u;
    float u3 = u2 * u;
    float inv_u = 1.0f - u;
    float inv_u2 = inv_u * inv_u;
    float inv_u3 = inv_u2 * inv_u;

    return p0 * inv_u3 +
           p1 * 3.0f * inv_u2 * u +
           p2 * 3.0f * inv_u * u2 +
           p3 * u3;
}

/**************************************************************************/
/*!
  @brief  Struct to hold a sampled stroke segment after curve processing
*/
/**************************************************************************/
typedef struct {
    float start_t;  //!< Start time (0.0 to 1.0)
    float end_t;    //!< End time (0.0 to 1.0)
    float start_y;  //!< Start position (0.0 to 1.0)
    float end_y;    //!< End position (0.0 to 1.0)
} StrokeSegment;

/**************************************************************************/
/*!
  @brief  Array-based pattern that interpolates between defined points.
  Linear segments become one stroke each. Curved segments are sampled
  into multiple linear strokes for smooth motion.
*/
/**************************************************************************/
class ArrayPattern : public Pattern {
  public:
    ArrayPattern(const char *name, const PatternPoint *points, int numPoints)
        : Pattern(name), _points(points), _numPoints(numPoints),
          _segments(nullptr), _numSegments(0) {
        if (_numPoints < 2) _numPoints = 2;
        _buildSegments();
    }

    ~ArrayPattern() {
        if (_segments) {
            delete[] _segments;
            _segments = nullptr;
        }
    }

    void setTimeOfStroke(float speed = 0) {
        int numIntervals = _numPoints - 1;
        _timeOfStroke = numIntervals * speed * 5.0f / 3.0f;
    }

    motionParameter nextTarget(unsigned int index) {
        if (_numSegments == 0) {
            _nextMove.skip = true;
            return _nextMove;
        }

        int segmentIndex = index % _numSegments;
        StrokeSegment seg = _segments[segmentIndex];

        // Calculate target position: position = depth - stroke * (1.0 - y)
        _nextMove.stroke = _depth - int((float)_stroke * (1.0f - seg.end_y));

        // Time for this segment
        float segmentDuration = seg.end_t - seg.start_t;
        float timeForSegment = segmentDuration * _timeOfStroke;

        // Distance to travel
        int startPos = _depth - int((float)_stroke * (1.0f - seg.start_y));
        int distance = abs(_nextMove.stroke - startPos);
        if (distance < 1) distance = 1;

        // Trapezoidal profile (1/3, 1/3, 1/3)
        _nextMove.speed = int(1.5f * (float)distance / timeForSegment);
        _nextMove.acceleration = int(3.0f * (float)_nextMove.speed / timeForSegment);
        _nextMove.skip = false;
        _index = index;

        return _nextMove;
    }

  protected:
    const PatternPoint *_points;
    int _numPoints;
    StrokeSegment *_segments;
    int _numSegments;

    void _buildSegments() {
        int totalSegments = 0;
        for (int i = 0; i < _numPoints - 1; i++) {
            if (_isCurved(_points[i], _points[i + 1])) {
                totalSegments += BEZIER_SAMPLES_PER_SEGMENT;
            } else {
                totalSegments += 1;
            }
        }

        _segments = new StrokeSegment[totalSegments];
        _numSegments = 0;

        for (int i = 0; i < _numPoints - 1; i++) {
            PatternPoint start = _points[i];
            PatternPoint end = _points[i + 1];

            if (_isCurved(start, end)) {
                _sampleCurvedSegment(start, end);
            } else {
                _addLinearSegment(start.t, start.y, end.t, end.y);
            }
        }
    }

    bool _isCurved(const PatternPoint &start, const PatternPoint &end) {
        return (start.right_control_magnitude > 0.0001f ||
                end.left_control_magnitude > 0.0001f);
    }

    void _addLinearSegment(float t0, float y0, float t1, float y1) {
        _segments[_numSegments].start_t = t0;
        _segments[_numSegments].start_y = y0;
        _segments[_numSegments].end_t = t1;
        _segments[_numSegments].end_y = y1;
        _numSegments++;
    }

    void _sampleCurvedSegment(const PatternPoint &start, const PatternPoint &end) {
        float dt = end.t - start.t;
        float dy = end.y - start.y;

        float c1_t = start.t + start.right_control_magnitude * dt *
                                   cos(start.right_control_slope);
        float c1_y = start.y + start.right_control_magnitude * dy *
                                   sin(start.right_control_slope);

        float c2_t =
            end.t - end.left_control_magnitude * dt * cos(end.left_control_slope);
        float c2_y =
            end.y - end.left_control_magnitude * dy * sin(end.left_control_slope);

        float prev_t = start.t;
        float prev_y = start.y;

        for (int i = 1; i <= BEZIER_SAMPLES_PER_SEGMENT; i++) {
            float u = (float)i / (float)BEZIER_SAMPLES_PER_SEGMENT;
            float curr_t = start.t + dt * u;
            float curr_y = cubicBezier(start.y, c1_y, c2_y, end.y, u);

            _addLinearSegment(prev_t, prev_y, curr_t, curr_y);

            prev_t = curr_t;
            prev_y = curr_y;
        }
    }
};

#endif // PATTERN_EXT_ARRAYPATTERN

/**************************************************************************/
/*!
  @brief  Test Pattern 1 - Simple triangular wave with slow extension, fast retraction
*/
/**************************************************************************/
#if PATTERN_EXT_TESTPATTERN1 && PATTERN_EXT_ARRAYPATTERN
class TestPattern1 : public ArrayPattern {
  public:
    TestPattern1() : ArrayPattern("Test Pattern 1", _patternPoints, 3) {}

  private:
    static constexpr PatternPoint _patternPoints[3] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.8f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}
    };
};
#endif

/**************************************************************************/
/*!
  @brief  Test Pattern 2 - Parabolic curve with smooth acceleration/deceleration
*/
/**************************************************************************/
#if PATTERN_EXT_TESTPATTERN2 && PATTERN_EXT_ARRAYPATTERN
class TestPattern2 : public ArrayPattern {
  public:
    TestPattern2() : ArrayPattern("Test Pattern 2", _patternPoints, 3) {}

  private:
    static constexpr PatternPoint _patternPoints[3] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 1.0f, 0.0f, 0.25f, 0.0f, 0.25f},
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}
    };
};
#endif