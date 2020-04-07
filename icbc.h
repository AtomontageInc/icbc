

namespace icbc {

    void init();

    float compress_dxt1(const float input_colors[16 * 4], const float input_weights[16], const float color_weights[3], bool three_color_mode, bool hq, void * output);
    float compress_dxt1_fast(const float input_colors[16 * 4], const float input_weights[16], const float color_weights[3], void * output);
    void compress_dxt1_fast(const unsigned char input_colors[16 * 4], void * output);

    enum Decoder {
        Decoder_D3D10 = 0,
        Decoder_D3D9 = 1,
        Decoder_NVIDIA = 2
    };

    float evaluate_dxt1_error(const unsigned char rgba_block[16 * 4], const void * block, Decoder decoder = Decoder_D3D10);

}

#ifdef ICBC_IMPLEMENTATION

#ifndef ICBC_USE_SSE
#define ICBC_USE_SSE 2
#endif

#ifndef ICBC_DECODER
#define ICBC_DECODER 0       // 0 = d3d10, 1 = d3d9, 2 = nvidia
#endif

#define ICBC_USE_SIMD ICBC_USE_SSE

#include <stdint.h>
#include <string.h> // memset
#include <math.h>   // floorf
#include <float.h>  // FLT_MAX

#ifndef ICBC_ASSERT
#define ICBC_ASSERT assert
#include <assert.h>
#endif

namespace icbc {

///////////////////////////////////////////////////////////////////////////////////////////////////
// Basic Templates

template <typename T> inline void swap(T & a, T & b) {
    T temp(a);
    a = b;
    b = temp;
}

template <typename T> inline T max(const T & a, const T & b) {
    return (b < a) ? a : b;
}

template <typename T> inline T min(const T & a, const T & b) {
    return (a < b) ? a : b;
}

template <typename T> inline T clamp(const T & x, const T & a, const T & b) {
    return min(max(x, a), b);
}

template <typename T> inline T square(const T & a) {
    return a * a;
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Basic Types

typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint32_t uint;


struct Color16 {
    union {
        struct {
            uint16 b : 5;
            uint16 g : 6;
            uint16 r : 5;
        };
        uint16 u;
    };
};

struct Color32 {
    union {
        struct {
            uint8 b, g, r, a;
        };
        uint32 u;
    };
};

struct BlockDXT1 {
    Color16 col0;
    Color16 col1;
    uint32 indices;
};


struct Vector3 {
    float x;
    float y;
    float z;

    inline void operator+=(Vector3 v) {
        x += v.x; y += v.y; z += v.z;
    }
    inline void operator*=(Vector3 v) {
        x *= v.x; y *= v.y; z *= v.z;
    }
    inline void operator*=(float s) {
        x *= s; y *= s; z *= s;
    }
};

struct Vector4 {
    union {
        struct {
            float x, y, z, w;
        };
        Vector3 xyz;
    };
};


inline Vector3 operator*(Vector3 v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}

inline Vector3 operator*(float s, Vector3 v) {
    return { v.x * s, v.y * s, v.z * s };
}

inline Vector3 operator*(Vector3 a, Vector3 b) {
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

inline float dot(Vector3 a, Vector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vector3 operator+(Vector3 a, Vector3 b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

inline Vector3 operator-(Vector3 a, Vector3 b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

inline Vector3 operator/(Vector3 v, float s) {
    return { v.x / s, v.y / s, v.z / s };
}

inline float saturate(float x) {
    return clamp(x, 0.0f, 1.0f);
}

inline Vector3 saturate(Vector3 v) {
    return { saturate(v.x), saturate(v.y), saturate(v.z) };
}

inline Vector3 min(Vector3 a, Vector3 b) {
    return { min(a.x, b.x), min(a.y, b.y), min(a.z, b.z) };
}

inline Vector3 max(Vector3 a, Vector3 b) {
    return { max(a.x, b.x), max(a.y, b.y), max(a.z, b.z) };
}

inline Vector3 round(Vector3 v) {
    return { floorf(v.x+0.5f), floorf(v.y + 0.5f), floorf(v.z + 0.5f) };
}

inline Vector3 floor(Vector3 v) {
    return { floorf(v.x), floorf(v.y), floorf(v.z) };
}

inline bool operator==(const Vector3 & a, const Vector3 & b) {
    return memcmp(&a, &b, sizeof(Vector3));
}

inline Vector3 scalar_to_vector3(float f) {
    return {f, f, f};
}

inline float lengthSquared(Vector3 v) {
    return dot(v, v);
}

inline bool equal(float a, float b, float epsilon = 0.0001) {
    // http://realtimecollisiondetection.net/blog/?p=89
    //return fabsf(a - b) < epsilon * max(1.0f, max(fabsf(a), fabsf(b)));
    return fabsf(a - b) < epsilon;
}

inline bool equal(Vector3 a, Vector3 b, float epsilon) {
    return equal(a.x, b.x, epsilon) && equal(a.y, b.y, epsilon) && equal(a.z, b.z, epsilon);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// SIMD

#if ICBC_USE_SIMD

#include <xmmintrin.h>
#include <emmintrin.h>

#define SIMD_INLINE inline
#define SIMD_NATIVE __forceinline

#ifndef ALIGN_16
#if __GNUC__
#   define ALIGN_16 __attribute__ ((__aligned__ (16)))
#else // _MSC_VER
#   define ALIGN_16 __declspec(align(16))
#endif
#endif

class SimdVector
{
public:
    __m128 vec;

    typedef SimdVector const& Arg;

    SIMD_NATIVE SimdVector() {}

    SIMD_NATIVE explicit SimdVector(__m128 v) : vec(v) {}

    SIMD_NATIVE explicit SimdVector(float f) {
        vec = _mm_set1_ps(f);
    }

    SIMD_NATIVE explicit SimdVector(const float * v)
    {
        vec = _mm_load_ps(v);
    }

    SIMD_NATIVE SimdVector(float x, float y, float z, float w)
    {
        vec = _mm_setr_ps(x, y, z, w);
    }

    SIMD_NATIVE SimdVector(const SimdVector & arg) : vec(arg.vec) {}

    SIMD_NATIVE SimdVector & operator=(const SimdVector & arg)
    {
        vec = arg.vec;
        return *this;
    }

    SIMD_INLINE float toFloat() const
    {
        ALIGN_16 float f;
        _mm_store_ss(&f, vec);
        return f;
    }

    SIMD_INLINE Vector3 toVector3() const
    {
        ALIGN_16 float c[4];
        _mm_store_ps(c, vec);
        return { c[0], c[1], c[2] };
    }

#define SSE_SPLAT( a ) ((a) | ((a) << 2) | ((a) << 4) | ((a) << 6))
    SIMD_NATIVE SimdVector splatX() const { return SimdVector(_mm_shuffle_ps(vec, vec, SSE_SPLAT(0))); }
    SIMD_NATIVE SimdVector splatY() const { return SimdVector(_mm_shuffle_ps(vec, vec, SSE_SPLAT(1))); }
    SIMD_NATIVE SimdVector splatZ() const { return SimdVector(_mm_shuffle_ps(vec, vec, SSE_SPLAT(2))); }
    SIMD_NATIVE SimdVector splatW() const { return SimdVector(_mm_shuffle_ps(vec, vec, SSE_SPLAT(3))); }
#undef SSE_SPLAT

    SIMD_NATIVE SimdVector& operator+=(Arg v)
    {
        vec = _mm_add_ps(vec, v.vec);
        return *this;
    }

    SIMD_NATIVE SimdVector& operator-=(Arg v)
    {
        vec = _mm_sub_ps(vec, v.vec);
        return *this;
    }

    SIMD_NATIVE SimdVector& operator*=(Arg v)
    {
        vec = _mm_mul_ps(vec, v.vec);
        return *this;
    }
};


SIMD_NATIVE SimdVector operator+(SimdVector::Arg left, SimdVector::Arg right)
{
    return SimdVector(_mm_add_ps(left.vec, right.vec));
}

SIMD_NATIVE SimdVector operator-(SimdVector::Arg left, SimdVector::Arg right)
{
    return SimdVector(_mm_sub_ps(left.vec, right.vec));
}

SIMD_NATIVE SimdVector operator*(SimdVector::Arg left, SimdVector::Arg right)
{
    return SimdVector(_mm_mul_ps(left.vec, right.vec));
}

// Returns a*b + c
SIMD_INLINE SimdVector multiplyAdd(SimdVector::Arg a, SimdVector::Arg b, SimdVector::Arg c)
{
    return SimdVector(_mm_add_ps(_mm_mul_ps(a.vec, b.vec), c.vec));
}

// Returns -( a*b - c )
SIMD_INLINE SimdVector negativeMultiplySubtract(SimdVector::Arg a, SimdVector::Arg b, SimdVector::Arg c)
{
    return SimdVector(_mm_sub_ps(c.vec, _mm_mul_ps(a.vec, b.vec)));
}

SIMD_INLINE SimdVector reciprocal(SimdVector::Arg v)
{
    // get the reciprocal estimate
    __m128 estimate = _mm_rcp_ps(v.vec);

    // one round of Newton-Rhaphson refinement
    __m128 diff = _mm_sub_ps(_mm_set1_ps(1.0f), _mm_mul_ps(estimate, v.vec));
    return SimdVector(_mm_add_ps(_mm_mul_ps(diff, estimate), estimate));
}

SIMD_NATIVE SimdVector min(SimdVector::Arg left, SimdVector::Arg right)
{
    return SimdVector(_mm_min_ps(left.vec, right.vec));
}

SIMD_NATIVE SimdVector max(SimdVector::Arg left, SimdVector::Arg right)
{
    return SimdVector(_mm_max_ps(left.vec, right.vec));
}

SIMD_INLINE SimdVector truncate(SimdVector::Arg v)
{
#if (ICBC_USE_SSE == 1)
    // convert to ints
    __m128 input = v.vec;
    __m64 lo = _mm_cvttps_pi32(input);
    __m64 hi = _mm_cvttps_pi32(_mm_movehl_ps(input, input));

    // convert to floats
    __m128 part = _mm_movelh_ps(input, _mm_cvtpi32_ps(input, hi));
    __m128 truncated = _mm_cvtpi32_ps(part, lo);

    // clear out the MMX multimedia state to allow FP calls later
    _mm_empty();
    return SimdVector(truncated);
#else
    // use SSE2 instructions
    return SimdVector(_mm_cvtepi32_ps(_mm_cvttps_epi32(v.vec)));
#endif
}

SIMD_INLINE SimdVector select(SimdVector::Arg off, SimdVector::Arg on, SimdVector::Arg bits)
{
    __m128 a = _mm_andnot_ps(bits.vec, off.vec);
    __m128 b = _mm_and_ps(bits.vec, on.vec);

    return SimdVector(_mm_or_ps(a, b));
}

SIMD_INLINE bool compareAnyLessThan(SimdVector::Arg left, SimdVector::Arg right)
{
    __m128 bits = _mm_cmplt_ps(left.vec, right.vec);
    int value = _mm_movemask_ps(bits);
    return value != 0;
}

#endif // ICBC_USE_SIMD


///////////////////////////////////////////////////////////////////////////////////////////////////
// Color conversion functions.

static const float midpoints5[32] = {
    0.015686f, 0.047059f, 0.078431f, 0.111765f, 0.145098f, 0.176471f, 0.207843f, 0.241176f, 0.274510f, 0.305882f, 0.337255f, 0.370588f, 0.403922f, 0.435294f, 0.466667f, 0.5f,
    0.533333f, 0.564706f, 0.596078f, 0.629412f, 0.662745f, 0.694118f, 0.725490f, 0.758824f, 0.792157f, 0.823529f, 0.854902f, 0.888235f, 0.921569f, 0.952941f, 0.984314f, 1.0f
};

static const float midpoints6[64] = {
    0.007843f, 0.023529f, 0.039216f, 0.054902f, 0.070588f, 0.086275f, 0.101961f, 0.117647f, 0.133333f, 0.149020f, 0.164706f, 0.180392f, 0.196078f, 0.211765f, 0.227451f, 0.245098f, 
    0.262745f, 0.278431f, 0.294118f, 0.309804f, 0.325490f, 0.341176f, 0.356863f, 0.372549f, 0.388235f, 0.403922f, 0.419608f, 0.435294f, 0.450980f, 0.466667f, 0.482353f, 0.500000f, 
    0.517647f, 0.533333f, 0.549020f, 0.564706f, 0.580392f, 0.596078f, 0.611765f, 0.627451f, 0.643137f, 0.658824f, 0.674510f, 0.690196f, 0.705882f, 0.721569f, 0.737255f, 0.754902f, 
    0.772549f, 0.788235f, 0.803922f, 0.819608f, 0.835294f, 0.850980f, 0.866667f, 0.882353f, 0.898039f, 0.913725f, 0.929412f, 0.945098f, 0.960784f, 0.976471f, 0.992157f, 1.0f
};

/*void init_tables() {
    for (int i = 0; i < 31; i++) {
        float f0 = float(((i+0) << 3) | ((i+0) >> 2)) / 255.0f;
        float f1 = float(((i+1) << 3) | ((i+1) >> 2)) / 255.0f;
        midpoints5[i] = (f0 + f1) * 0.5;
    }
    midpoints5[31] = 1.0f;

    for (int i = 0; i < 63; i++) {
        float f0 = float(((i+0) << 2) | ((i+0) >> 4)) / 255.0f;
        float f1 = float(((i+1) << 2) | ((i+1) >> 4)) / 255.0f;
        midpoints6[i] = (f0 + f1) * 0.5;
    }
    midpoints6[63] = 1.0f;
}*/

static Color16 vector3_to_color16(const Vector3 & v) {

    // Truncate.
    uint r = uint(clamp(v.x * 31.0f, 0.0f, 31.0f));
	uint g = uint(clamp(v.y * 63.0f, 0.0f, 63.0f));
	uint b = uint(clamp(v.z * 31.0f, 0.0f, 31.0f));

    // Round exactly according to 565 bit-expansion.
    r += (v.x > midpoints5[r]);
    g += (v.y > midpoints6[g]);
    b += (v.z > midpoints5[b]);

    Color16 c;
    c.u = (r << 11) | (g << 5) | b;
    return c;
}

static Color32 bitexpand_color16_to_color32(Color16 c16) {
    Color32 c32;
    //c32.b = (c16.b << 3) | (c16.b >> 2);
    //c32.g = (c16.g << 2) | (c16.g >> 4);
    //c32.r = (c16.r << 3) | (c16.r >> 2);
    //c32.a = 0xFF;

    c32.u = ((c16.u << 3) & 0xf8) | ((c16.u << 5) & 0xfc00) | ((c16.u << 8) & 0xf80000);
    c32.u |= (c32.u >> 5) & 0x070007;
    c32.u |= (c32.u >> 6) & 0x000300;

    return c32;
}

inline Vector3 color_to_vector3(Color32 c) {
    return { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f };
}

inline Color32 vector3_to_color32(Vector3 v) {
    Color32 color;
    color.r = uint8(saturate(v.x) * 255 + 0.5f);
    color.g = uint8(saturate(v.y) * 255 + 0.5f);
    color.b = uint8(saturate(v.z) * 255 + 0.5f);
    color.a = 255;
    return color;
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Input block processing.

// Find similar colors and combine them together.
static int reduce_colors(const Vector4 * input_colors, const float * input_weights, Vector3 * colors, float * weights)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
    {
        Vector3 ci = input_colors[i].xyz;
        float wi = input_weights[i];

        float threshold = 1.0 / 256;

        if (wi > 0) {
            // Find matching color.
            int j;
            for (j = 0; j < n; j++) {
                if (equal(colors[j], ci, threshold)) {
                    weights[j] += wi;
                    break;
                }
            }

            // No match found. Add new color.
            if (j == n) {
                colors[n] = ci;
                weights[n] = wi;
                n++;
            }
        }
    }

    ICBC_ASSERT(n <= 16);

    return n;
}

static int reduce_colors(const uint8 * input_colors, Vector3 * colors, float * weights)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
    {
        Vector3 ci;
        ci.x = float(input_colors[4 * i + 0]);
        ci.y = float(input_colors[4 * i + 1]);
        ci.z = float(input_colors[4 * i + 2]);

        float threshold = 1.0 / 256;

        // Find matching color.
        int j;
        for (j = 0; j < n; j++) {
            if (equal(colors[j], ci, threshold)) {
                weights[j] += 1.0f;
                break;
            }
        }

        // No match found. Add new color.
        if (j == n) {
            colors[n] = ci;
            weights[n] = 1.0f;
            n++;
        }
    }

    ICBC_ASSERT(n <= 16);

    return n;
}




///////////////////////////////////////////////////////////////////////////////////////////////////
// Cluster Fit

class ClusterFit
{
public:
    ClusterFit() {}

    void setColorSet(const Vector3 * colors, const float * weights, int count);

    void setColorWeights(const Vector3 & w);
    float bestError() const;

    bool compress3(Vector3 * start, Vector3 * end);
    bool compress4(Vector3 * start, Vector3 * end);

private:

    uint m_count;

    // IC: Color and weight arrays are larger than necessary to avoid compiler warning.

#if ICBC_USE_SIMD
    ALIGN_16 SimdVector m_weighted[17];  // color | weight
    SimdVector m_metric;                    // vec3
    SimdVector m_metricSqr;                 // vec3
    SimdVector m_xxsum;                     // color | weight
    SimdVector m_xsum;                      // color | weight (wsum)
    SimdVector m_besterror;                 // scalar
#else
    Vector3 m_weighted[17];
    float m_weights[17];
    Vector3 m_metric;
    Vector3 m_metricSqr;
    Vector3 m_xxsum;
    Vector3 m_xsum;
    float m_wsum;
    float m_besterror;
#endif
};


static Vector3 computeCentroid(int n, const Vector3 *__restrict points, const float *__restrict weights, const Vector3 & metric)
{
    Vector3 centroid = { 0 };
    float total = 0.0f;

    for (int i = 0; i < n; i++)
    {
        total += weights[i];
        centroid += weights[i] * points[i];
    }
    centroid *= (1.0f / total);

    return centroid;
}

static Vector3 computeCovariance(int n, const Vector3 *__restrict points, const float *__restrict weights, const Vector3 & metric, float *__restrict covariance)
{
    // compute the centroid
    Vector3 centroid = computeCentroid(n, points, weights, metric);

    // compute covariance matrix
    for (int i = 0; i < 6; i++)
    {
        covariance[i] = 0.0f;
    }

    for (int i = 0; i < n; i++)
    {
        Vector3 a = (points[i] - centroid) * metric;    // @@ I think weight should be squared, but that seems to increase the error slightly.
        Vector3 b = weights[i] * a;

        covariance[0] += a.x * b.x;
        covariance[1] += a.x * b.y;
        covariance[2] += a.x * b.z;
        covariance[3] += a.y * b.y;
        covariance[4] += a.y * b.z;
        covariance[5] += a.z * b.z;
    }

    return centroid;
}

// @@ We should be able to do something cheaper...
static Vector3 estimatePrincipalComponent(const float * __restrict matrix)
{
    const Vector3 row0 = { matrix[0], matrix[1], matrix[2] };
    const Vector3 row1 = { matrix[1], matrix[3], matrix[4] };
    const Vector3 row2 = { matrix[2], matrix[4], matrix[5] };

    float r0 = lengthSquared(row0);
    float r1 = lengthSquared(row1);
    float r2 = lengthSquared(row2);

    if (r0 > r1 && r0 > r2) return row0;
    if (r1 > r2) return row1;
    return row2;
}

static inline Vector3 firstEigenVector_PowerMethod(const float *__restrict matrix)
{
    if (matrix[0] == 0 && matrix[3] == 0 && matrix[5] == 0)
    {
        return {0};
    }

    Vector3 v = estimatePrincipalComponent(matrix);

    const int NUM = 8;
    for (int i = 0; i < NUM; i++)
    {
        float x = v.x * matrix[0] + v.y * matrix[1] + v.z * matrix[2];
        float y = v.x * matrix[1] + v.y * matrix[3] + v.z * matrix[4];
        float z = v.x * matrix[2] + v.y * matrix[4] + v.z * matrix[5];

        float norm = max(max(x, y), z);

        v = { x, y, z };
        v *= (1.0f / norm);
    }

    return v;
}

static Vector3 computePrincipalComponent_PowerMethod(int n, const Vector3 *__restrict points, const float *__restrict weights, const Vector3 & metric)
{
    float matrix[6];
    computeCovariance(n, points, weights, metric, matrix);

    return firstEigenVector_PowerMethod(matrix);
}

void ClusterFit::setColorSet(const Vector3 * colors, const float * weights, int count)
{
    // initialise the best error
#if ICBC_USE_SIMD
    m_besterror = SimdVector(FLT_MAX);
    Vector3 metric = m_metric.toVector3();
#else
    m_besterror = FLT_MAX;
    Vector3 metric = m_metric;
#endif

    m_count = count;

    // I've tried using a lower quality approximation of the principal direction, but the best fit line seems to produce best results.
    Vector3 principal = computePrincipalComponent_PowerMethod(count, colors, weights, metric);

    // build the list of values
    int order[16];
    float dps[16];
    for (uint i = 0; i < m_count; ++i)
    {
        dps[i] = dot(colors[i], principal);
        order[i] = i;
    }

    // stable sort
    for (uint i = 0; i < m_count; ++i)
    {
        for (uint j = i; j > 0 && dps[j] < dps[j - 1]; --j)
        {
            swap(dps[j], dps[j - 1]);
            swap(order[j], order[j - 1]);
        }
    }

    // weight all the points
#if ICBC_USE_SIMD
    m_xxsum = SimdVector(0.0f);
    m_xsum = SimdVector(0.0f);
#else
    m_xxsum = { 0.0f };
    m_xsum = { 0.0f };
    m_wsum = 0.0f;
#endif

    for (uint i = 0; i < m_count; ++i)
    {
        int p = order[i];
#if ICBC_USE_SIMD
        ALIGN_16 Vector4 tmp;
        tmp.xyz = colors[p];
        tmp.w = 1;
        m_weighted[i] = SimdVector(&tmp.x) * SimdVector(weights[p]);
        m_xxsum += m_weighted[i] * m_weighted[i];
        m_xsum += m_weighted[i];
#else
        m_weighted[i] = colors[p] * weights[p];
        m_xxsum += m_weighted[i] * m_weighted[i];
        m_xsum += m_weighted[i];
        m_weights[i] = weights[p];
        m_wsum += m_weights[i];
#endif
    }
}



void ClusterFit::setColorWeights(const Vector3 & w)
{
#if ICBC_USE_SIMD
    ALIGN_16 Vector4 tmp;
    tmp.xyz = w;
    tmp.w = 1;
    m_metric = SimdVector(&tmp.x);
#else
    m_metric = w;
#endif
    m_metricSqr = m_metric * m_metric;
}

float ClusterFit::bestError() const
{
#if ICBC_USE_SIMD
    SimdVector x = m_xxsum * m_metricSqr;
    SimdVector error = m_besterror + x.splatX() + x.splatY() + x.splatZ();
    return error.toFloat();
#else
    return m_besterror + dot(m_xxsum, m_metricSqr);
#endif

}

#if ICBC_USE_SIMD

bool ClusterFit::compress3(Vector3 * start, Vector3 * end)
{
    const int count = m_count;
    const SimdVector one = SimdVector(1.0f);
    const SimdVector zero = SimdVector(0.0f);
    const SimdVector half(0.5f, 0.5f, 0.5f, 0.25f);
    const SimdVector two = SimdVector(2.0);
    const SimdVector grid(31.0f, 63.0f, 31.0f, 0.0f);
    const SimdVector gridrcp(1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f, 0.0f);

    // declare variables
    SimdVector beststart = SimdVector(0.0f);
    SimdVector bestend = SimdVector(0.0f);
    SimdVector besterror = SimdVector(FLT_MAX);

    SimdVector x0 = zero;

    // check all possible clusters for this total order
    for (int c0 = 0; c0 <= count; c0++)
    {
        SimdVector x1 = zero;

        for (int c1 = 0; c1 <= count - c0; c1++)
        {
            const SimdVector x2 = m_xsum - x1 - x0;

            //Vector3 alphax_sum = x0 + x1 * 0.5f;
            //float alpha2_sum = w0 + w1 * 0.25f;
            const SimdVector alphax_sum = multiplyAdd(x1, half, x0); // alphax_sum, alpha2_sum
            const SimdVector alpha2_sum = alphax_sum.splatW();

            //const Vector3 betax_sum = x2 + x1 * 0.5f;
            //const float beta2_sum = w2 + w1 * 0.25f;
            const SimdVector betax_sum = multiplyAdd(x1, half, x2); // betax_sum, beta2_sum
            const SimdVector beta2_sum = betax_sum.splatW();

            //const float alphabeta_sum = w1 * 0.25f;
            const SimdVector alphabeta_sum = (x1 * half).splatW(); // alphabeta_sum

            // const float factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);
            const SimdVector factor = reciprocal(negativeMultiplySubtract(alphabeta_sum, alphabeta_sum, alpha2_sum*beta2_sum));

            SimdVector a = negativeMultiplySubtract(betax_sum, alphabeta_sum, alphax_sum*beta2_sum) * factor;
            SimdVector b = negativeMultiplySubtract(alphax_sum, alphabeta_sum, betax_sum*alpha2_sum) * factor;

            // clamp to the grid
            a = min(one, max(zero, a));
            b = min(one, max(zero, b));
            a = truncate(multiplyAdd(grid, a, half)) * gridrcp;
            b = truncate(multiplyAdd(grid, b, half)) * gridrcp;

            // compute the error (we skip the constant xxsum)
            SimdVector e1 = multiplyAdd(a*a, alpha2_sum, b*b*beta2_sum);
            SimdVector e2 = negativeMultiplySubtract(a, alphax_sum, a*b*alphabeta_sum);
            SimdVector e3 = negativeMultiplySubtract(b, betax_sum, e2);
            SimdVector e4 = multiplyAdd(two, e3, e1);

            // apply the metric to the error term
            SimdVector e5 = e4 * m_metricSqr;
            SimdVector error = e5.splatX() + e5.splatY() + e5.splatZ();

            // keep the solution if it wins
            if (compareAnyLessThan(error, besterror))
            {
                besterror = error;
                beststart = a;
                bestend = b;
            }

            x1 += m_weighted[c0 + c1];
        }

        x0 += m_weighted[c0];
    }

    // save the block if necessary
    if (compareAnyLessThan(besterror, m_besterror))
    {
        *start = beststart.toVector3();
        *end = bestend.toVector3();

        // save the error
        m_besterror = besterror;

        return true;
    }

    return false;
}

bool ClusterFit::compress4(Vector3 * start, Vector3 * end)
{
    const int count = m_count;
    const SimdVector one = SimdVector(1.0f);
    const SimdVector zero = SimdVector(0.0f);
    const SimdVector half = SimdVector(0.5f);
    const SimdVector two = SimdVector(2.0);
    const SimdVector onethird(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 9.0f);
    const SimdVector twothirds(2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 4.0f / 9.0f);
    const SimdVector twonineths = SimdVector(2.0f / 9.0f);
    const SimdVector grid(31.0f, 63.0f, 31.0f, 0.0f);
    const SimdVector gridrcp(1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f, 0.0f);

    // declare variables
    SimdVector beststart = SimdVector(0.0f);
    SimdVector bestend = SimdVector(0.0f);
    SimdVector besterror = SimdVector(FLT_MAX);

    SimdVector x0 = zero;

    // check all possible clusters for this total order
    for (int c0 = 0; c0 <= count; c0++)
    {
        SimdVector x1 = zero;

        for (int c1 = 0; c1 <= count - c0; c1++)
        {
            SimdVector x2 = zero;

            for (int c2 = 0; c2 <= count - c0 - c1; c2++)
            {
                const SimdVector x3 = m_xsum - x2 - x1 - x0;

                //const Vector3 alphax_sum = x0 + x1 * (2.0f / 3.0f) + x2 * (1.0f / 3.0f);
                //const float alpha2_sum = w0 + w1 * (4.0f/9.0f) + w2 * (1.0f/9.0f);
                const SimdVector alphax_sum = multiplyAdd(x2, onethird, multiplyAdd(x1, twothirds, x0)); // alphax_sum, alpha2_sum
                const SimdVector alpha2_sum = alphax_sum.splatW();

                //const Vector3 betax_sum = x3 + x2 * (2.0f / 3.0f) + x1 * (1.0f / 3.0f);
                //const float beta2_sum = w3 + w2 * (4.0f/9.0f) + w1 * (1.0f/9.0f);
                const SimdVector betax_sum = multiplyAdd(x2, twothirds, multiplyAdd(x1, onethird, x3)); // betax_sum, beta2_sum
                const SimdVector beta2_sum = betax_sum.splatW();

                //const float alphabeta_sum = (w1 + w2) * (2.0f/9.0f);
                const SimdVector alphabeta_sum = twonineths * (x1 + x2).splatW(); // alphabeta_sum

                //const float factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);
                const SimdVector factor = reciprocal(negativeMultiplySubtract(alphabeta_sum, alphabeta_sum, alpha2_sum*beta2_sum));

                SimdVector a = negativeMultiplySubtract(betax_sum, alphabeta_sum, alphax_sum*beta2_sum) * factor;
                SimdVector b = negativeMultiplySubtract(alphax_sum, alphabeta_sum, betax_sum*alpha2_sum) * factor;

                // clamp to the grid
                a = min(one, max(zero, a));
                b = min(one, max(zero, b));
                a = truncate(multiplyAdd(grid, a, half)) * gridrcp;
                b = truncate(multiplyAdd(grid, b, half)) * gridrcp;

                // compute the error (we skip the constant xxsum)
                // error = a*a*alpha2_sum + b*b*beta2_sum + 2.0f*( a*b*alphabeta_sum - a*alphax_sum - b*betax_sum );
                SimdVector e1 = multiplyAdd(a*a, alpha2_sum, b*b*beta2_sum);
                SimdVector e2 = negativeMultiplySubtract(a, alphax_sum, a*b*alphabeta_sum);
                SimdVector e3 = negativeMultiplySubtract(b, betax_sum, e2);
                SimdVector e4 = multiplyAdd(two, e3, e1);

                // apply the metric to the error term
                SimdVector e5 = e4 * m_metricSqr;
                SimdVector error = e5.splatX() + e5.splatY() + e5.splatZ();

                // keep the solution if it wins
                if (compareAnyLessThan(error, besterror))
                {
                    besterror = error;
                    beststart = a;
                    bestend = b;
                }

                x2 += m_weighted[c0 + c1 + c2];
            }

            x1 += m_weighted[c0 + c1];
        }

        x0 += m_weighted[c0];
    }

    // save the block if necessary
    if (compareAnyLessThan(besterror, m_besterror))
    {
        *start = beststart.toVector3();
        *end = bestend.toVector3();

        // save the error
        m_besterror = besterror;

        return true;
    }

    return false;
}

#else

// This is the ideal way to round, but it's too expensive to do this in the inner loop.
inline Vector3 round565(const Vector3 & v) {
    static const Vector3 grid = { 31.0f, 63.0f, 31.0f };
    static const Vector3 gridrcp = { 1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f };

    Vector3 q = floor(grid * v);
    q.x += (v.x > midpoints5[int(q.x)]);
    q.y += (v.y > midpoints6[int(q.y)]);
    q.z += (v.z > midpoints5[int(q.z)]);
    q *= gridrcp;
    return q;
}

bool ClusterFit::compress3(Vector3 * start, Vector3 * end)
{
    const uint count = m_count;
    const Vector3 grid = { 31.0f, 63.0f, 31.0f };
    const Vector3 gridrcp = { 1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f };

    // declare variables
    Vector3 beststart = { 0.0f };
    Vector3 bestend = { 0.0f };
    float besterror = FLT_MAX;

    Vector3 x0 = { 0.0f };
    float w0 = 0.0f;

    int b0 = 0, b1 = 0;

    // check all possible clusters for this total order
    for (uint c0 = 0; c0 <= count; c0++)
    {
        Vector3 x1 = { 0.0f };
        float w1 = 0.0f;

        for (uint c1 = 0; c1 <= count - c0; c1++)
        {
            float w2 = m_wsum - w0 - w1;

            // These factors could be entirely precomputed.
            float const alpha2_sum = w0 + w1 * 0.25f;
            float const beta2_sum = w2 + w1 * 0.25f;
            float const alphabeta_sum = w1 * 0.25f;
            float const factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);

            Vector3 const alphax_sum = x0 + x1 * 0.5f;
            Vector3 const betax_sum = m_xsum - alphax_sum;

            Vector3 a = (alphax_sum*beta2_sum - betax_sum * alphabeta_sum) * factor;
            Vector3 b = (betax_sum*alpha2_sum - alphax_sum * alphabeta_sum) * factor;

            // clamp to the grid
            a = saturate(a);
            b = saturate(b);
#if 1
            a = round(grid * a) * gridrcp;
            b = round(grid * b) * gridrcp;
#else
            a = round565(a);
            b = round565(b);
#endif

            // compute the error
            Vector3 e1 = a * a*alpha2_sum + b * b*beta2_sum + 2.0f*(a*b*alphabeta_sum - a * alphax_sum - b * betax_sum);

            // apply the metric to the error term
            float error = dot(e1, m_metricSqr);

            // keep the solution if it wins
            if (error < besterror)
            {
                besterror = error;
                beststart = a;
                bestend = b;
                b0 = c0;
                b1 = c1;
            }

            x1 += m_weighted[c0 + c1];
            w1 += m_weights[c0 + c1];
        }

        x0 += m_weighted[c0];
        w0 += m_weights[c0];
    }

    // save the block if necessary
    if (besterror < m_besterror)
    {

        *start = beststart;
        *end = bestend;

        // save the error
        m_besterror = besterror;

        return true;
    }

    return false;
}

bool ClusterFit::compress4(Vector3 * start, Vector3 * end)
{
    const uint count = m_count;
    const Vector3 grid = { 31.0f, 63.0f, 31.0f };
    const Vector3 gridrcp = { 1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f };

    // declare variables
    Vector3 beststart = { 0.0f };
    Vector3 bestend = { 0.0f };
    float besterror = FLT_MAX;

    Vector3 x0 = { 0.0f };
    float w0 = 0.0f;
    int b0 = 0, b1 = 0, b2 = 0;

    // check all possible clusters for this total order
    for (uint c0 = 0; c0 <= count; c0++)
    {
        Vector3 x1 = { 0.0f };
        float w1 = 0.0f;

        for (uint c1 = 0; c1 <= count - c0; c1++)
        {
            Vector3 x2 = { 0.0f };
            float w2 = 0.0f;

            for (uint c2 = 0; c2 <= count - c0 - c1; c2++)
            {
                float w3 = m_wsum - w0 - w1 - w2;

                float const alpha2_sum = w0 + w1 * (4.0f / 9.0f) + w2 * (1.0f / 9.0f);
                float const beta2_sum = w3 + w2 * (4.0f / 9.0f) + w1 * (1.0f / 9.0f);
                float const alphabeta_sum = (w1 + w2) * (2.0f / 9.0f);
                float const factor = 1.0f / (alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum);

                Vector3 const alphax_sum = x0 + x1 * (2.0f / 3.0f) + x2 * (1.0f / 3.0f);
                Vector3 const betax_sum = m_xsum - alphax_sum;

                Vector3 a = (alphax_sum*beta2_sum - betax_sum * alphabeta_sum)*factor;
                Vector3 b = (betax_sum*alpha2_sum - alphax_sum * alphabeta_sum)*factor;

                // clamp to the grid
                a = saturate(a);
                b = saturate(b);
#if 1
                a = round(a * grid) * gridrcp;
                b = round(b * grid) * gridrcp;
#else
                a = round565(a);
                b = round565(b);
#endif
                // @@ It would be much more accurate to evaluate the error exactly. 

                // compute the error
                Vector3 e1 = a * a*alpha2_sum + b * b*beta2_sum + 2.0f*(a*b*alphabeta_sum - a * alphax_sum - b * betax_sum);

                // apply the metric to the error term
                float error = dot(e1, m_metricSqr);

                // keep the solution if it wins
                if (error < besterror)
                {
                    besterror = error;
                    beststart = a;
                    bestend = b;
                    b0 = c0;
                    b1 = c1;
                    b2 = c2;
                }

                x2 += m_weighted[c0 + c1 + c2];
                w2 += m_weights[c0 + c1 + c2];
            }

            x1 += m_weighted[c0 + c1];
            w1 += m_weights[c0 + c1];
        }

        x0 += m_weighted[c0];
        w0 += m_weights[c0];
    }

    // save the block if necessary
    if (besterror < m_besterror)
    {
        *start = beststart;
        *end = bestend;

        // save the error
        m_besterror = besterror;

        return true;
    }

    return false;
}

#endif // ICBC_USE_SIMD


///////////////////////////////////////////////////////////////////////////////////////////////////
// Palette evaluation.

inline void evaluate_palette4(Color16 c0, Color16 c1, Color32 palette[4], bool d3d9_bias) {
#if DECODER == 0 || DECODER == 1
    palette[2].r = (2 * palette[0].r + palette[1].r + d3d9_bias) / 3;
    palette[2].g = (2 * palette[0].g + palette[1].g + d3d9_bias) / 3;
    palette[2].b = (2 * palette[0].b + palette[1].b + d3d9_bias) / 3;
    palette[3].r = (2 * palette[1].r + palette[0].r + d3d9_bias) / 3;
    palette[3].g = (2 * palette[1].g + palette[0].g + d3d9_bias) / 3;
    palette[3].b = (2 * palette[1].b + palette[0].b + d3d9_bias) / 3;
#else
    int dg = palette[1].g - palette[0].g;
    palette[2].r = ((2 * c0.r + c1.r) * 22) / 8;
    palette[2].g = (256 * palette[0].g + dg * 80 + dg / 4 + 128) / 256;
    palette[2].b = ((2 * c0.b + c1.b) * 22) / 8;
    palette[3].r = ((2 * c1.r + c0.r) * 22) / 8;
    palette[3].g = (256 * palette[1].g - dg * 80 - dg / 4 + 128) / 256;
    palette[3].b = ((2 * c1.b + c0.b) * 22) / 8;
#endif
}

inline void evaluate_palette3(Color16 c0, Color16 c1, Color32 palette[4]) {
#if DECODER == 0 || DECODER == 1
    palette[2].r = (palette[0].r + palette[1].r) / 2;
    palette[2].g = (palette[0].g + palette[1].g) / 2;
    palette[2].b = (palette[0].b + palette[1].b) / 2;
#else
    int dg = palette[1].g - palette[0].g;
    palette[2].r = ((c0.r + c1.r) * 33) / 8;
    palette[2].g = (256 * palette[0].g + dg * 128 + dg / 4 + 128) / 256;
    palette[2].b = ((c0.b + c1.b) * 33) / 8;
#endif
    palette[3].r = 0;
    palette[3].g = 0;
    palette[3].b = 0;
}

static void evaluate_palette(Color16 c0, Color16 c1, Color32 palette[4], bool d3d9_bias) {
    palette[0] = bitexpand_color16_to_color32(c0);
    palette[1] = bitexpand_color16_to_color32(c1);
    if (c0.u > c1.u) {
        evaluate_palette4(c0, c1, palette, d3d9_bias);
    }
    else {
        evaluate_palette3(c0, c1, palette);
    }
}

static void evaluate_palette_nv(Color16 c0, Color16 c1, Color32 palette[4]) {
    palette[0].r = (3 * c0.r * 22) / 8;
    palette[0].g = (c0.g << 2) | (c0.g >> 4);
    palette[0].b = (3 * c0.b * 22) / 8;
    palette[1].a = 255;
    palette[1].r = (3 * c1.r * 22) / 8;
    palette[1].g = (c1.g << 2) | (c1.g >> 4);
    palette[1].b = (3 * c1.b * 22) / 8;
    palette[1].a = 255;

    int gdiff = palette[1].g - palette[0].g;
    if (c0.u > c1.u) {
        palette[2].r = ((2 * c0.r + c1.r) * 22) / 8;
        palette[2].g = (256 * palette[0].g + gdiff / 4 + 128 + gdiff * 80) / 256;
        palette[2].b = ((2 * c0.b + c1.b) * 22) / 8;
        palette[2].a = 0xFF;

        palette[3].r = ((2 * c1.r + c0.r) * 22) / 8;
        palette[3].g = (256 * palette[1].g - gdiff / 4 + 128 - gdiff * 80) / 256;
        palette[3].b = ((2 * c1.b + c0.b) * 22) / 8;
        palette[3].a = 0xFF;
    }
    else {
        palette[2].r = ((c0.r + c1.r) * 33) / 8;
        palette[2].g = (256 * palette[0].g + gdiff / 4 + 128 + gdiff * 128) / 256;
        palette[2].b = ((c0.b + c1.b) * 33) / 8;
        palette[2].a = 0xFF;
        palette[3].u = 0;
    }
}

static void evaluate_palette(Color16 c0, Color16 c1, Color32 palette[4]) {
#if DECODER == Decoder_D3D10
    evaluate_palette(c0, c1, palette, false);
#elif DECODER == Decoder_D3D9
    evaluate_palette(c0, c1, palette, true);
#elif DECODER == Decoder_NVIDIA
    evaluate_palette_nv(c0, c1, palette);
#endif
}

static void evaluate_palette(Color16 c0, Color16 c1, Vector3 palette[4]) {
    Color32 palette32[4];
    evaluate_palette(c0, c1, palette32);

    for (int i = 0; i < 4; i++) {
        palette[i] = color_to_vector3(palette32[i]);
    }
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Error evaluation.

// Different ways of estimating the error.

static float evaluate_mse(const Vector3 & p, const Vector3 & c, const Vector3 & w) {
    Vector3 d = (p - c) * w * 255;
    return dot(d, d);
}

static float evaluate_mse(const Color32 & p, const Vector3 & c, const Vector3 & w) {
    Vector3 d = (color_to_vector3(p) - c) * w * 255;
    return dot(d, d);
}


/*static float evaluate_mse(const Vector3 & p, const Vector3 & c, const Vector3 & w) {
    return ww.x * square(p.x-c.x) + ww.y * square(p.y-c.y) + ww.z * square(p.z-c.z);
}*/

static int evaluate_mse(const Color32 & p, const Color32 & c) {
    return (square(int(p.r)-c.r) + square(int(p.g)-c.g) + square(int(p.b)-c.b));
}

/*static float evaluate_mse(const Vector3 palette[4], const Vector3 & c, const Vector3 & w) {
    float e0 = evaluate_mse(palette[0], c, w);
    float e1 = evaluate_mse(palette[1], c, w);
    float e2 = evaluate_mse(palette[2], c, w);
    float e3 = evaluate_mse(palette[3], c, w);
    return min(min(e0, e1), min(e2, e3));
}*/

static int evaluate_mse(const Color32 palette[4], const Color32 & c) {
    int e0 = evaluate_mse(palette[0], c);
    int e1 = evaluate_mse(palette[1], c);
    int e2 = evaluate_mse(palette[2], c);
    int e3 = evaluate_mse(palette[3], c);
    return min(min(e0, e1), min(e2, e3));
}

// Returns MSE error in [0-255] range.
static int evaluate_mse(const BlockDXT1 * output, Color32 color, int index) {
    Color32 palette[4];
    evaluate_palette(output->col0, output->col1, palette);

    return evaluate_mse(palette[index], color);
}

// Returns weighted MSE error in [0-255] range.
static float evaluate_palette_error(Color32 palette[4], const Color32 * colors, const float * weights, int count) {
    
    float total = 0.0f;
    for (int i = 0; i < count; i++) {
        total += weights[i] * evaluate_mse(palette, colors[i]);
    }

    return total;
}

static float evaluate_palette_error(Color32 palette[4], const Color32 * colors, int count) {

    float total = 0.0f;
    for (int i = 0; i < count; i++) {
        total += evaluate_mse(palette, colors[i]);
    }

    return total;
}

#if 0
static float evaluate_mse(const BlockDXT1 * output, const Vector3 colors[16]) {
    Color32 palette[4];
    output->evaluatePalette(palette, /*d3d9=*/false);

    // convert palette to float.
    Vector3 vector_palette[4];
    for (int i = 0; i < 4; i++) {
        vector_palette[i] = color_to_vector3(palette[i]);
    }

    // evaluate error for each index.
    float error = 0.0f;
    for (int i = 0; i < 16; i++) {
        int index = (output->indices >> (2*i)) & 3; // @@ Is this the right order?
        error += evaluate_mse(vector_palette[index], colors[i]);
    }

    return error;
}
#endif

static float evaluate_mse(const Vector4 input_colors[16], const float input_weights[16], const Vector3 & color_weights, const BlockDXT1 * output) {
    Color32 palette[4];
    evaluate_palette(output->col0, output->col1, palette);

    // convert palette to float.
    /*Vector3 vector_palette[4];
    for (int i = 0; i < 4; i++) {
        vector_palette[i] = color_to_vector3(palette[i]);
    }*/

    // evaluate error for each index.
    float error = 0.0f;
    for (int i = 0; i < 16; i++) {
        int index = (output->indices >> (2 * i)) & 3;
        error += input_weights[i] * evaluate_mse(palette[index], input_colors[i].xyz, color_weights);
    }
    return error;
}

float evaluate_dxt1_error(const uint8 rgba_block[16*4], const BlockDXT1 * block, Decoder decoder) {
    Color32 palette[4];
    if (decoder == Decoder_NVIDIA) {
        evaluate_palette_nv(block->col0, block->col1, palette);

    }
    else {
        evaluate_palette(block->col0, block->col1, palette, /*d3d9=*/decoder == Decoder_D3D9);
    }

    // evaluate error for each index.
    float error = 0.0f;
    for (int i = 0; i < 16; i++) {
        int index = (block->indices >> (2 * i)) & 3;
        Color32 c;
        c.r = rgba_block[4 * i + 0];
        c.g = rgba_block[4 * i + 1];
        c.b = rgba_block[4 * i + 2];
        c.a = 255;
        error += evaluate_mse(palette[index], c);
    }
    return error;
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Index selection

static uint compute_indices4(const Vector4 input_colors[16], const Vector3 & color_weights, const Vector3 palette[4]) {
    
    uint indices = 0;
    for (int i = 0; i < 16; i++) {
        float d0 = evaluate_mse(palette[0], input_colors[i].xyz, color_weights);
        float d1 = evaluate_mse(palette[1], input_colors[i].xyz, color_weights);
        float d2 = evaluate_mse(palette[2], input_colors[i].xyz, color_weights);
        float d3 = evaluate_mse(palette[3], input_colors[i].xyz, color_weights);

        uint b0 = d0 > d3;
        uint b1 = d1 > d2;
        uint b2 = d0 > d2;
        uint b3 = d1 > d3;
        uint b4 = d2 > d3;

        uint x0 = b1 & b2;
        uint x1 = b0 & b3;
        uint x2 = b0 & b4;

        indices |= (x2 | ((x0 | x1) << 1)) << (2 * i);
    }

    return indices;
}


static uint compute_indices4(const Vector3 input_colors[16], const Vector3 palette[4]) {

    uint indices = 0;
    for (int i = 0; i < 16; i++) {
        float d0 = evaluate_mse(palette[0], input_colors[i], {1,1,1});
        float d1 = evaluate_mse(palette[1], input_colors[i], {1,1,1});
        float d2 = evaluate_mse(palette[2], input_colors[i], {1,1,1});
        float d3 = evaluate_mse(palette[3], input_colors[i], {1,1,1});

        uint b0 = d0 > d3;
        uint b1 = d1 > d2;
        uint b2 = d0 > d2;
        uint b3 = d1 > d3;
        uint b4 = d2 > d3;

        uint x0 = b1 & b2;
        uint x1 = b0 & b3;
        uint x2 = b0 & b4;

        indices |= (x2 | ((x0 | x1) << 1)) << (2 * i);
    }

    return indices;
}


static uint compute_indices(const Vector4 input_colors[16], const Vector3 & color_weights, const Vector3 palette[4]) {
    
    uint indices = 0;
    for (int i = 0; i < 16; i++) {
        float d0 = evaluate_mse(palette[0], input_colors[i].xyz, color_weights);
        float d1 = evaluate_mse(palette[1], input_colors[i].xyz, color_weights);
        float d2 = evaluate_mse(palette[2], input_colors[i].xyz, color_weights);
        float d3 = evaluate_mse(palette[3], input_colors[i].xyz, color_weights);

        uint index;
        if (d0 < d1 && d0 < d2 && d0 < d3) index = 0;
        else if (d1 < d2 && d1 < d3) index = 1;
        else if (d2 < d3) index = 2;
        else index = 3;

		indices |= index << (2 * i);
	}

	return indices;
}


static void output_block3(const Vector4 input_colors[16], const Vector3 & color_weights, const Vector3 & v0, const Vector3 & v1, BlockDXT1 * block)
{
    Color16 color0 = vector3_to_color16(v0);
    Color16 color1 = vector3_to_color16(v1);

    if (color0.u > color1.u) {
        swap(color0, color1);
    }

    Vector3 palette[4];
    evaluate_palette(color0, color1, palette);

    block->col0 = color0;
    block->col1 = color1;
    block->indices = compute_indices(input_colors, color_weights, palette);
}

static void output_block4(const Vector4 input_colors[16], const Vector3 & color_weights, const Vector3 & v0, const Vector3 & v1, BlockDXT1 * block)
{
    Color16 color0 = vector3_to_color16(v0);
    Color16 color1 = vector3_to_color16(v1);

    if (color0.u < color1.u) {
        swap(color0, color1);
    }

    Vector3 palette[4];
    evaluate_palette(color0, color1, palette);

    block->col0 = color0;
    block->col1 = color1;
    block->indices = compute_indices4(input_colors, color_weights, palette);
}


static void output_block4(const Vector3 input_colors[16], const Vector3 & v0, const Vector3 & v1, BlockDXT1 * block)
{
    Color16 color0 = vector3_to_color16(v0);
    Color16 color1 = vector3_to_color16(v1);

    if (color0.u < color1.u) {
        swap(color0, color1);
    }

    Vector3 palette[4];
    evaluate_palette(color0, color1, palette);

    block->col0 = color0;
    block->col1 = color1;
    block->indices = compute_indices4(input_colors, palette);
}

// Least squares fitting of color end points for the given indices. @@ Take weights into account.
static bool optimize_end_points4(uint indices, const Vector4 * colors, /*const float * weights,*/ int count, Vector3 * a, Vector3 * b)
{
    float alpha2_sum = 0.0f;
    float beta2_sum = 0.0f;
    float alphabeta_sum = 0.0f;
    Vector3 alphax_sum = { 0,0,0 };
    Vector3 betax_sum = { 0,0,0 };

    for (int i = 0; i < count; i++)
    {
        const uint bits = indices >> (2 * i);

        float beta = float(bits & 1);
        if (bits & 2) beta = (1 + beta) / 3.0f;
        float alpha = 1.0f - beta;

        alpha2_sum += alpha * alpha;
        beta2_sum += beta * beta;
        alphabeta_sum += alpha * beta;
        alphax_sum += alpha * colors[i].xyz;
        betax_sum += beta * colors[i].xyz;
    }

    float denom = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
    if (equal(denom, 0.0f)) return false;

    float factor = 1.0f / denom;

    *a = saturate((alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor);
    *b = saturate((betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor);

    return true;
}

static bool optimize_end_points4(uint indices, const Vector3 * colors, int count, Vector3 * a, Vector3 * b)
{
    float alpha2_sum = 0.0f;
    float beta2_sum = 0.0f;
    float alphabeta_sum = 0.0f;
    Vector3 alphax_sum = { 0,0,0 };
    Vector3 betax_sum = { 0,0,0 };

    for (int i = 0; i < count; i++)
    {
        const uint bits = indices >> (2 * i);

        float beta = float(bits & 1);
        if (bits & 2) beta = (1 + beta) / 3.0f;
        float alpha = 1.0f - beta;

        alpha2_sum += alpha * alpha;
        beta2_sum += beta * beta;
        alphabeta_sum += alpha * beta;
        alphax_sum += alpha * colors[i];
        betax_sum += beta * colors[i];
    }

    float denom = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
    if (equal(denom, 0.0f)) return false;

    float factor = 1.0f / denom;

    *a = saturate((alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor);
    *b = saturate((betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor);

    return true;
}


// Least squares fitting of color end points for the given indices. @@ This does not support black/transparent index. @@ Take weights into account.
static bool optimize_end_points3(uint indices, const Vector3 * colors, /*const float * weights,*/ int count, Vector3 * a, Vector3 * b)
{
    float alpha2_sum = 0.0f;
    float beta2_sum = 0.0f;
    float alphabeta_sum = 0.0f;
    Vector3 alphax_sum = { 0,0,0 };
    Vector3 betax_sum = { 0,0,0 };

    for (int i = 0; i < count; i++)
    {
        const uint bits = indices >> (2 * i);

        float beta = float(bits & 1);
        if (bits & 2) beta = 0.5f;
        float alpha = 1.0f - beta;

        alpha2_sum += alpha * alpha;
        beta2_sum += beta * beta;
        alphabeta_sum += alpha * beta;
        alphax_sum += alpha * colors[i];
        betax_sum += beta * colors[i];
    }

    float denom = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
    if (equal(denom, 0.0f)) return false;

    float factor = 1.0f / denom;

    *a = saturate((alphax_sum * beta2_sum - betax_sum * alphabeta_sum) * factor);
    *b = saturate((betax_sum * alpha2_sum - alphax_sum * alphabeta_sum) * factor);

    return true;
}

// @@ After optimization we need to round end points. Round in all possible directions, and pick best.



// find minimum and maximum colors based on bounding box in color space
inline static void fit_colors_bbox(const Vector3 * colors, int count, Vector3 * __restrict c0, Vector3 * __restrict c1)
{
    *c0 = { 0,0,0 };
    *c1 = { 1,1,1 };

    for (int i = 0; i < count; i++) {
        *c0 = max(*c0, colors[i]);
        *c1 = min(*c1, colors[i]);
    }
}

inline static void select_diagonal(const Vector3 * colors, int count, Vector3 * __restrict c0, Vector3 * __restrict c1)
{
    Vector3 center = (*c0 + *c1) * 0.5f;

    /*Vector3 center = colors[0];
    for (int i = 1; i < count; i++) {
        center = center * float(i-1) / i + colors[i] / i;
    }*/
    /*Vector3 center = colors[0];
    for (int i = 1; i < count; i++) {
        center += colors[i];
    }
    center /= count;*/

    float cov_xz = 0.0f;
    float cov_yz = 0.0f;
    for (int i = 0; i < count; i++) {
        Vector3 t = colors[i] - center;
        cov_xz += t.x * t.z;
        cov_yz += t.y * t.z;
    }

    float x0 = c0->x;
    float y0 = c0->y;
    float x1 = c1->x;
    float y1 = c1->y;

    if (cov_xz < 0) {
        swap(x0, x1);
    }
    if (cov_yz < 0) {
        swap(y0, y1);
    }

    *c0 = { x0, y0, c0->z };
    *c1 = { x1, y1, c1->z };
}

inline static void inset_bbox(Vector3 * __restrict c0, Vector3 * __restrict c1)
{
    float bias = (8.0f / 255.0f) / 16.0f;
    Vector3 inset = (*c0 - *c1) / 16.0f - scalar_to_vector3(bias);
    *c0 = saturate(*c0 - inset);
    *c1 = saturate(*c1 + inset);
}



// Single color lookup tables from:
// https://github.com/nothings/stb/blob/master/stb_dxt.h
static uint8 match5[256][2];
static uint8 match6[256][2];

static int Mul8Bit(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static inline int Lerp13(int a, int b)
{
#ifdef DXT_USE_ROUNDING_BIAS
    // with rounding bias
    return a + Mul8Bit(b - a, 0x55);
#else
    // without rounding bias
    // replace "/ 3" by "* 0xaaab) >> 17" if your compiler sucks or you really need every ounce of speed.
    return (a * 2 + b) / 3;
#endif
}

static void PrepareOptTable(uint8 * table, const uint8 * expand, int size)
{
    for (int i = 0; i < 256; i++) {
        int bestErr = 256 * 100;

        for (int min = 0; min < size; min++) {
            for (int max = 0; max < size; max++) {
                int mine = expand[min];
                int maxe = expand[max];

                int err = abs(Lerp13(maxe, mine) - i) * 100;

                // DX10 spec says that interpolation must be within 3% of "correct" result,
                // add this as error term. (normally we'd expect a random distribution of
                // +-1.5% error, but nowhere in the spec does it say that the error has to be
                // unbiased - better safe than sorry).
                err += abs(max - min) * 3;

                if (err < bestErr) {
                    bestErr = err;
                    table[i * 2 + 0] = max;
                    table[i * 2 + 1] = min;
                }
            }
        }
    }
}

static void init_dxt1_tables()
{
    // Prepare single color lookup tables.
    uint8 expand5[32];
    uint8 expand6[64];
    for (int i = 0; i < 32; i++) expand5[i] = (i << 3) | (i >> 2);
    for (int i = 0; i < 64; i++) expand6[i] = (i << 2) | (i >> 4);

    PrepareOptTable(&match5[0][0], expand5, 32);
    PrepareOptTable(&match6[0][0], expand6, 64);
}

// Single color compressor, based on:
// https://mollyrocket.com/forums/viewtopic.php?t=392
static void compress_dxt1_single_color_optimal(Color32 c, BlockDXT1 * output)
{
    output->col0.r = match5[c.r][0];
    output->col0.g = match6[c.g][0];
    output->col0.b = match5[c.b][0];
    output->col1.r = match5[c.r][1];
    output->col1.g = match6[c.g][1];
    output->col1.b = match5[c.b][1];
    output->indices = 0xaaaaaaaa;
    
    if (output->col0.u < output->col1.u)
    {
        swap(output->col0.u, output->col1.u);
        output->indices ^= 0x55555555;
    }
}


// Compress block using the average color.
static float compress_dxt1_single_color(const Vector3 * colors, const float * weights, int count, const Vector3 & color_weights, BlockDXT1 * output)
{
    // Compute block average.
    Vector3 color_sum = { 0,0,0 };
    float weight_sum = 0;

    for (int i = 0; i < count; i++) {
        color_sum += colors[i] * weights[i];
        weight_sum += weights[i];
    }

    // Compress optimally.
    compress_dxt1_single_color_optimal(vector3_to_color32(color_sum / weight_sum), output);

    // Decompress block color.
    Color32 palette[4];
    evaluate_palette(output->col0, output->col1, palette);
    //output->evaluatePalette(palette, /*d3d9=*/false);

    Vector3 block_color = color_to_vector3(palette[output->indices & 0x3]);

    // Evaluate error.
    float error = 0;
    for (int i = 0; i < count; i++) {
        error += weights[i] * evaluate_mse(block_color, colors[i], color_weights);
    }
    return error;
}


static float compress_dxt1_bounding_box_exhaustive(const Vector4 input_colors[16], const Vector3 * colors, const float * weights, int count, const Vector3 & color_weights, bool three_color_mode, int max_volume, BlockDXT1 * output)
{
    // Compute bounding box.
    Vector3 min_color = { 1,1,1 };
    Vector3 max_color = { 0,0,0 };

    for (int i = 0; i < count; i++) {
        min_color = min(min_color, colors[i]);
        max_color = max(max_color, colors[i]);
    }

    // Convert to 5:6:5
    int min_r = int(31 * min_color.x);
    int min_g = int(63 * min_color.y);
    int min_b = int(31 * min_color.z);
    int max_r = int(31 * max_color.x + 1);
    int max_g = int(63 * max_color.y + 1);
    int max_b = int(31 * max_color.z + 1);

    // Expand the box.
    int range_r = max_r - min_r;
    int range_g = max_g - min_g;
    int range_b = max_b - min_b;

    min_r = max(0, min_r - range_r / 2 - 2);
    min_g = max(0, min_g - range_g / 2 - 2);
    min_b = max(0, min_b - range_b / 2 - 2);

    max_r = min(31, max_r + range_r / 2 + 2);
    max_g = min(63, max_g + range_g / 2 + 2);
    max_b = min(31, max_b + range_b / 2 + 2);

    // Estimate size of search space.
    int volume = (max_r-min_r+1) * (max_g-min_g+1) * (max_b-min_b+1);

    // if size under search_limit, then proceed. Note that search_volume is sqrt of number of evaluations.
    if (volume > max_volume) {
        return FLT_MAX;
    }

    // @@ Convert to fixed point before building box?
    Color32 colors32[16];
    for (int i = 0; i < count; i++) {
        colors32[i] = vector3_to_color32(colors[i]);
    }

    float best_error = FLT_MAX;
    Color16 best0, best1;           // @@ Record endpoints as Color16?

    Color16 c0, c1;
    Color32 palette[4];

    for(int r0 = min_r; r0 <= max_r; r0++)
    for(int g0 = min_g; g0 <= max_g; g0++)
    for(int b0 = min_b; b0 <= max_b; b0++)
    {
        c0.r = r0; c0.g = g0; c0.b = b0;
        palette[0] = bitexpand_color16_to_color32(c0);

        for(int r1 = min_r; r1 <= max_r; r1++)
        for(int g1 = min_g; g1 <= max_g; g1++)
        for(int b1 = min_b; b1 <= max_b; b1++)
        {
            c1.r = r1; c1.g = g1; c1.b = b1;
            palette[1] = bitexpand_color16_to_color32(c1);

            if (c0.u > c1.u) {
                // Evaluate error in 4 color mode.
                evaluate_palette4(c0, c1, palette, false);
            }
            else {
                if (three_color_mode) {
                    // Evaluate error in 3 color mode.
                    evaluate_palette3(c0, c1, palette);
                }
                else {
                    // Skip 3 color mode.
                    continue;
                }
            }

            float error = evaluate_palette_error(palette, colors32, weights, count);

            if (error < best_error) {
                best_error = error;
                best0 = c0;
                best1 = c1;
            }
        }
    }

    output->col0 = best0;
    output->col1 = best1;

    Vector3 vector_palette[4];
    evaluate_palette(output->col0, output->col1, vector_palette);

    output->indices = compute_indices(input_colors, color_weights, vector_palette);

    return best_error / (255 * 255);
}


static void compress_dxt1_cluster_fit(const Vector4 input_colors[16], const Vector3 * colors, const float * weights, int count, const Vector3 & color_weights, bool three_color_mode, BlockDXT1 * output)
{
    ClusterFit fit;
    fit.setColorWeights(color_weights);
    fit.setColorSet(colors, weights, count);

    // start & end are in [0, 1] range.
    Vector3 start, end;
    fit.compress4(&start, &end);

    if (three_color_mode && fit.compress3(&start, &end)) {
        output_block3(input_colors, color_weights, start, end, output);
    }
    else {
        output_block4(input_colors, color_weights, start, end, output);
    }
}


static float compress_dxt1(const Vector4 input_colors[16], const float input_weights[16], const Vector3 & color_weights, bool three_color_mode, bool hq, BlockDXT1 * output)
{
    Vector3 colors[16];
    float weights[16];
    int count = reduce_colors(input_colors, input_weights, colors, weights);

    if (count == 0) {
        // Output trivial block.
        output->col0.u = 0;
        output->col1.u = 0;
        output->indices = 0;
        return 0;
    }


    float error = FLT_MAX;

    // Sometimes the single color compressor produces better results than the exhaustive. This introduces discontinuities between blocks that
    // use different compressors. For this reason, this is not enabled by default.
    if (0) {
        error = compress_dxt1_single_color(colors, weights, count, color_weights, output);

        if (error == 0.0f || count == 1) {
            // Early out.
            return error;
        }
    }

    // This is too expensive, even with a low threshold.
    // If high quality:
    if (/* DISABLES CODE */ (0)) {
        BlockDXT1 exhaustive_output;
        float exhaustive_error = compress_dxt1_bounding_box_exhaustive(input_colors, colors, weights, count, color_weights, three_color_mode, 1400, &exhaustive_output);

        if (exhaustive_error != FLT_MAX) {
            float exhaustive_error2 = evaluate_mse(input_colors, input_weights, color_weights, &exhaustive_output);

            // The exhaustive compressor does not use color_weights, so the results may be different.
            //nvCheck(equal(exhaustive_error, exhaustive_error2));

            if (exhaustive_error2 < error) {
                *output = exhaustive_output;
                error = exhaustive_error;
            }
        }
    }

    // Cluster fit cannot handle single color blocks, so encode them optimally if we haven't encoded them already.
    if (error == FLT_MAX && count == 1) {
        compress_dxt1_single_color_optimal(vector3_to_color32(colors[0]), output);
        return evaluate_mse(input_colors, input_weights, color_weights, output);
    }

    if (count > 1) {
        // Fast box fit encoding:
        {
            BlockDXT1 box_fit_output;

            Vector3 colors[16];
            for (int i = 0; i < 16; i++) {
                colors[i] = input_colors[i].xyz;
            }
            int count = 16;

            // Quick end point selection.
            Vector3 c0, c1;
            fit_colors_bbox(colors, count, &c0, &c1);
            inset_bbox(&c0, &c1);
            select_diagonal(colors, count, &c0, &c1);
            output_block4(input_colors, color_weights, c0, c1, &box_fit_output);

            float box_fit_error = evaluate_mse(input_colors, input_weights, color_weights, &box_fit_output);
            if (box_fit_error < error) {
                error = box_fit_error;
                *output = box_fit_output;

                // Refine color for the selected indices.
                if (optimize_end_points4(output->indices, input_colors, 16, &c0, &c1)) {
                    output_block4(input_colors, color_weights, c0, c1, &box_fit_output);

                    box_fit_error = evaluate_mse(input_colors, input_weights, color_weights, &box_fit_output);
                    if (box_fit_error < error) {
                        error = box_fit_error;
                        *output = box_fit_output;
                    }
                }
            }
        }

        // Try cluster fit.
        BlockDXT1 cluster_fit_output;
        compress_dxt1_cluster_fit(input_colors, colors, weights, count, color_weights, three_color_mode, &cluster_fit_output);

        float cluster_fit_error = evaluate_mse(input_colors, input_weights, color_weights, &cluster_fit_output);

        if (cluster_fit_error < error) {
            *output = cluster_fit_output;
            error = cluster_fit_error;
        }

        if (hq) {
            // TODO:
            // - Optimize palette evaluation when updating only one channel.
            // - try all diagonals.

            // Things that don't help:
            // - Alternate endpoint updates.
            // - Randomize order.
            // - If one direction does not improve, test opposite direction next.

            static const int8 deltas[16][3] = {
                {1,0,0},
                {0,1,0},
                {0,0,1},

                {-1,0,0},
                {0,-1,0},
                {0,0,-1},

                {1,1,0},
                {1,0,1},
                {0,1,1},

                {-1,-1,0},
                {-1,0,-1},
                {0,-1,-1},

                {-1,1,0},
                //{-1,0,1},

                {1,-1,0},
                {0,-1,1},

                //{1,0,-1},
                {0,1,-1},
            };

            int lastImprovement = 0;
            for (int i = 0; i < 256; i++) {
                BlockDXT1 refined = *output;
                int8 delta[3] = { deltas[i % 16][0], deltas[i % 16][1], deltas[i % 16][2] };

                if ((i / 16) & 1) {
                    refined.col0.r += delta[0];
                    refined.col0.g += delta[1];
                    refined.col0.b += delta[2];
                }
                else {
                    refined.col1.r += delta[0];
                    refined.col1.g += delta[1];
                    refined.col1.b += delta[2];
                }

                if (!three_color_mode) {
                    if (refined.col0.u == refined.col1.u) refined.col1.g += 1;
                    if (refined.col0.u < refined.col1.u) swap(refined.col0.u, refined.col1.u);
                }

                Vector3 palette[4];
                evaluate_palette(output->col0, output->col1, palette);

                refined.indices = compute_indices(input_colors, color_weights, palette);

                float refined_error = evaluate_mse(input_colors, input_weights, color_weights, &refined);
                if (refined_error < error) {
                    *output = refined;
                    error = refined_error;
                    lastImprovement = i;
                }

                // Early out if the last 32 steps didn't improve error.
                if (i - lastImprovement > 32) break;
            }
        }
    }

    return error;
}


static float compress_dxt1_fast(const Vector4 input_colors[16], const float input_weights[16], const Vector3 & color_weights, BlockDXT1 * output)
{
    Vector3 colors[16];
    for (int i = 0; i < 16; i++) {
        colors[i] = input_colors[i].xyz;
    }
    int count = 16;

    /*float error = FLT_MAX;
    error = compress_dxt1_single_color(colors, input_weights, count, color_weights, output);

    if (error == 0.0f || count == 1) {
        // Early out.
        return error;
    }*/

    // Quick end point selection.
    Vector3 c0, c1;
    fit_colors_bbox(colors, count, &c0, &c1);
    if (c0 == c1) {
        compress_dxt1_single_color_optimal(vector3_to_color32(c0), output);
        return evaluate_mse(input_colors, input_weights, color_weights, output);
    }
    inset_bbox(&c0, &c1);
    select_diagonal(colors, count, &c0, &c1);
    output_block4(input_colors, color_weights, c0, c1, output);

    // Refine color for the selected indices.
    if (optimize_end_points4(output->indices, input_colors, 16, &c0, &c1)) {
        output_block4(input_colors, color_weights, c0, c1, output);
    }

    return evaluate_mse(input_colors, input_weights, color_weights, output);
}


static void compress_dxt1_fast(const uint8 input_colors[16*4], BlockDXT1 * output) {

    Vector3 vec_colors[16];
    for (int i = 0; i < 16; i++) {
        vec_colors[i] = { input_colors[4 * i + 0] / 255.0f, input_colors[4 * i + 1] / 255.0f, input_colors[4 * i + 2] / 255.0f };
    }

    // Quick end point selection.
    Vector3 c0, c1;
    //fit_colors_bbox(colors, count, &c0, &c1);
    //select_diagonal(colors, count, &c0, &c1);
    fit_colors_bbox(vec_colors, 16, &c0, &c1);
    if (c0 == c1) {
        compress_dxt1_single_color_optimal(vector3_to_color32(c0), output);
        return;
    }
    inset_bbox(&c0, &c1);
    select_diagonal(vec_colors, 16, &c0, &c1);
    output_block4(vec_colors, c0, c1, output);

    // Refine color for the selected indices.
    if (optimize_end_points4(output->indices, vec_colors, 16, &c0, &c1)) {
        output_block4(vec_colors, c0, c1, output);
    }
}

// Public API

void init() {
    init_dxt1_tables();
}

float compress_dxt1(const float input_colors[16 * 4], const float input_weights[16], const float rgb[3], bool three_color_mode, bool hq, void * output) {
    return compress_dxt1((Vector4*)input_colors, input_weights, { rgb[0], rgb[1], rgb[2] }, three_color_mode, hq, (BlockDXT1*)output);
}

float compress_dxt1_fast(const float input_colors[16 * 4], const float input_weights[16], const float rgb[3], void * output) {
    return compress_dxt1_fast((Vector4*)input_colors, input_weights, { rgb[0], rgb[1], rgb[2] }, (BlockDXT1*)output);
}

void compress_dxt1_fast(const unsigned char input_colors[16 * 4], void * output) {
    compress_dxt1_fast(input_colors, (BlockDXT1*)output);
}

float evaluate_dxt1_error(const unsigned char rgba_block[16 * 4], const void * dxt_block, Decoder decoder/*=Decoder_D3D10*/) {
    return evaluate_dxt1_error(rgba_block, (BlockDXT1 *)dxt_block, decoder);
}

} // icbc
#endif // ICBC_IMPLEMENTATION
