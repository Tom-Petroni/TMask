static const char* const CLASS = "TMask";
static const char* const HELP =
    "Position mask with TNoise-like center/pref picking lock.";

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "DDImage/Channel.h"
#include "DDImage/DDMath.h"
#include "DDImage/Format.h"
#include "DDImage/Iop.h"
#include "DDImage/Knob.h"
#include "DDImage/Knobs.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Row.h"
#include "DDImage/Thread.h"
#include "DDImage/Vector3.h"
#include "DDImage/noise.h"

using namespace DD::Image;

namespace {

enum ShapeMode {
  SHAPE_SPHERE = 0,
  SHAPE_CUBE = 1,
  SHAPE_CYLINDER_X = 2,
  SHAPE_CYLINDER_Y = 3,
  SHAPE_CYLINDER_Z = 4,
  SHAPE_DIAMOND = 5,
  SHAPE_PLANE_X = 6,
  SHAPE_PLANE_Y = 7,
  SHAPE_PLANE_Z = 8,
};

enum FalloffMode {
  FALLOFF_LINEAR = 0,
  FALLOFF_SMOOTH = 1,
};

enum NoiseType {
  NOISE_PERLIN = 0,
  NOISE_SIMPLEX = 1,
  NOISE_VORONOI = 2,
  NOISE_PATTERN = 3,
};

enum FractalMode {
  FRACTAL_NONE = 0,
  FRACTAL_FBM = 1,
  FRACTAL_BILLOW = 2,
  FRACTAL_RIDGED = 3,
};

enum VoronoiMetric {
  VORONOI_EUCLIDEAN = 0,
  VORONOI_MANHATTAN = 1,
  VORONOI_CHEBYSHEV = 2,
  VORONOI_MINKOWSKI = 3,
};

enum VoronoiShapeMode {
  VORONOI_SHAPE_VORONOI = 0,
  VORONOI_SHAPE_CELLS = 1,
  VORONOI_SHAPE_CRYSTALS = 2,
  VORONOI_SHAPE_CRACKS = 3,
  VORONOI_SHAPE_WEB = 4,
  VORONOI_SHAPE_BUBBLES = 5,
};

enum PatternTypeMode {
  PATTERN_CONCENTRIC = 0,
  PATTERN_LINEAR = 1,
  PATTERN_RADIAL = 2,
  PATTERN_SPIRAL = 3,
};

enum PatternShapeMode {
  PATTERN_SHAPE_ROUND = 0,
  PATTERN_SHAPE_SQUARE = 1,
  PATTERN_SHAPE_DIAMOND = 2,
};

const char* const kShapeModes[] = {
    "Sphere", "Cube", "Cylinder X", "Cylinder Y", "Cylinder Z",
    "Diamond", "Plane X", "Plane Y", "Plane Z", nullptr,
};
const char* const kFalloffModes[] = {"Linear", "Smooth", nullptr};
const char* const kNoiseTypes[] = {"Perlin", "Simplex", "Voronoi", "Pattern", nullptr};
const char* const kFractalModes[] = {"None", "fBm", "Billow", "Ridged", nullptr};
const char* const kVoronoiMetrics[] = {"Euclidean", "Manhattan", "Chebyshev", "Minkowski", nullptr};
const char* const kVoronoiShapeModes[] = {
    "Voronoi", "Cells", "Crystals", "Cracks", "Web", "Bubbles", nullptr,
};
const char* const kPatternTypeModes[] = {"Concentric", "Linear", "Radial", nullptr};
const char* const kPatternShapeModes[] = {"Round", nullptr};
constexpr int kMaxMasks = 12;

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

template <typename T>
T clamp_compat(T value, T lo, T hi) {
  return std::max(lo, std::min(hi, value));
}

bool is_finitef(float value) {
  return std::isfinite(static_cast<double>(value));
}

bool is_finited(double value) {
  return std::isfinite(value);
}

float sanitize_finite(float value, float fallback = 0.0f) {
  return is_finitef(value) ? value : fallback;
}

double sanitize_finited(double value, double fallback = 0.0) {
  return is_finited(value) ? value : fallback;
}

float clamp01(float value) {
  if (!is_finitef(value)) { return 0.0f; }
  return clamp_compat(value, 0.0f, 1.0f);
}
float fractf(float value) { return value - std::floor(value); }
float deg_to_rad(float degrees) { return degrees * 0.01745329251994329577f; }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

float soft_clip_signed(float v) {
  return static_cast<float>(std::tanh(static_cast<double>(v) * 0.45));
}

static void animated_translate_from_frame(float speed, float angle_degrees, float frame,
                                          float& tx, float& ty,
                                          float effect_scale = 0.5f) {
  const float distance = std::max(0.0f, speed) * std::max(0.0f, effect_scale) * frame;
  const float angle = deg_to_rad(angle_degrees);
  tx = -std::cos(angle) * distance;
  ty = -std::sin(angle) * distance;
}

// ---------------------------------------------------------------------------
// Noise utilities (identical to TNoise domain warp versions)
// ---------------------------------------------------------------------------

float noise4_compat(float x, float y, float z, float w) {
#if defined(kDDImageVersionMajorNum) && (kDDImageVersionMajorNum >= 14)
  return static_cast<float>(noise(x, y, z, w));
#else
  const float wx = w * 0.754877666f;
  const float wy = w * 0.569840296f;
  const float wz = w * 0.438579212f;
  return static_cast<float>(noise(x + wx, y + wy, z + wz));
#endif
}

float perlin01(float x, float y, float z) {
  return clamp01(static_cast<float>(noise(x, y, z)) * 0.5f + 0.5f);
}

float finalize_noise_value(float value, float gamma) {
  float out = value;
  if (gamma != 1.0f) {
    if (gamma <= 0.0001f) {
      out = out >= 1.0f ? 1.0f : 0.0f;
    } else if (gamma == 0.5f) {
      if (out > 0.0f) { out *= out; }
    } else if (out > 0.0f) {
      out = std::pow(out, 1.0f / gamma);
    }
  }
  return clamp01(out);
}

std::uint32_t fnv_hash4(int x, int y, int z, std::uint32_t seed) {
  const std::uint32_t kPrime = 16777619u;
  std::uint32_t h = 2166136261u;
  h = (h ^ static_cast<std::uint32_t>(x)) * kPrime;
  h = (h ^ static_cast<std::uint32_t>(y)) * kPrime;
  h = (h ^ static_cast<std::uint32_t>(z)) * kPrime;
  h = (h ^ seed) * kPrime;
  return h;
}

std::uint32_t fnv_hash5(int x, int y, int z, int w, std::uint32_t seed) {
  const std::uint32_t kPrime = 16777619u;
  std::uint32_t h = fnv_hash4(x, y, z, seed);
  h = (h ^ static_cast<std::uint32_t>(w)) * kPrime;
  return h;
}

float hash01(std::uint32_t h) {
  return static_cast<float>(h & 0x00ffffffu) / 16777215.0f;
}

float hash_signed(std::uint32_t h) { return hash01(h) * 2.0f - 1.0f; }

constexpr std::uint32_t kVoronoiRandModulus = 2147483647u;
constexpr float kVoronoiInvRandModulus = 4.65661287e-10f;

std::uint32_t voronoi_lcg_random(std::uint32_t seed) {
  if (seed == 0u) { seed = 1u; }
  constexpr std::int64_t kA = 48271;
  constexpr std::int64_t kM = 2147483647;
  constexpr std::int64_t kQ = 44488;
  constexpr std::int64_t kR = 3399;
  const std::int64_t hi = static_cast<std::int64_t>(seed) / kQ;
  const std::int64_t lo = static_cast<std::int64_t>(seed) % kQ;
  const std::int64_t test = kA * lo - kR * hi;
  return static_cast<std::uint32_t>(test > 0 ? test : test + kM);
}

int voronoi_prob_lookup(std::uint32_t value) {
  if (value < 1022645910u) return 1;
  if (value < 2700834071u) return 2;
  if (value < 3819626178u) return 3;
  return 4;
}

float voronoi_distance(float dx, float dy, float dz, float dw, bool use_4d,
                       int distance_function, float minkowski_exp) {
  const float adx = std::fabs(dx);
  const float ady = std::fabs(dy);
  const float adz = std::fabs(dz);
  const float adw = use_4d ? std::fabs(dw) : 0.0f;
  switch (distance_function) {
    case VORONOI_MANHATTAN:
      return adx + ady + adz + adw;
    case VORONOI_CHEBYSHEV:
      return use_4d ? std::max(std::max(adx, ady), std::max(adz, adw))
                    : std::max(adx, std::max(ady, adz));
    case VORONOI_MINKOWSKI: {
      const float p = std::max(0.1f, minkowski_exp);
      const float inv_p = 1.0f / p;
      float sum = std::pow(adx, p) + std::pow(ady, p) + std::pow(adz, p);
      if (use_4d) { sum += std::pow(adw, p); }
      return std::pow(sum, inv_p);
    }
    case VORONOI_EUCLIDEAN:
    default:
      return dx * dx + dy * dy + dz * dz + (use_4d ? (dw * dw) : 0.0f);
  }
}

float distance_to_interval(float v, float vmin, float vmax) {
  if (v < vmin) return vmin - v;
  if (v > vmax) return v - vmax;
  return 0.0f;
}

float voronoi_cell_lower_bound(float x, float y, float z, float w, bool use_4d,
                               int cX, int cY, int cZ, int cW,
                               float bias, float rand_amt,
                               int distance_function, float minkowski_exp) {
  const float min_x = static_cast<float>(cX) + bias;
  const float min_y = static_cast<float>(cY) + bias;
  const float min_z = static_cast<float>(cZ) + bias;
  const float min_w = static_cast<float>(cW) + bias;
  const float dx = distance_to_interval(x, min_x, min_x + rand_amt);
  const float dy = distance_to_interval(y, min_y, min_y + rand_amt);
  const float dz = distance_to_interval(z, min_z, min_z + rand_amt);
  const float dw = use_4d ? distance_to_interval(w, min_w, min_w + rand_amt) : 0.0f;
  return voronoi_distance(dx, dy, dz, dw, use_4d, distance_function, minkowski_exp);
}

struct VoronoiHit { float f1; float f2; std::uint32_t id1; std::uint32_t id2; };

VoronoiHit voronoi_search(float x, float y, float z, float w, bool use_time,
                          int distance_function, float jitter_modifier,
                          float minkowski_exp, bool need_second_hit) {
  float best_dist0 = 1e20f, best_dist1 = 1e20f;
  std::uint32_t best_id0 = 0u, best_id1 = 0u;
  const float wt = use_time ? w : 0.0f;
  const int cx = static_cast<int>(std::floor(x));
  const int cy = static_cast<int>(std::floor(y));
  const int cz = static_cast<int>(std::floor(z));
  const int cw = static_cast<int>(std::floor(wt));
  const float rand_amt = std::max(0.0f, jitter_modifier);
  const float bias = 0.5f * (1.0f - rand_amt);
  constexpr std::uint32_t user_seed = 1337u;

  const int dw_min = use_time ? -1 : 0;
  const int dw_max = use_time ? 1 : 0;
  for (int dw = dw_min; dw <= dw_max; ++dw) {
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int cX = cx + dx, cY = cy + dy, cZ = cz + dz, cW = cw + dw;
          const float lb = voronoi_cell_lower_bound(
              x, y, z, wt, use_time, cX, cY, cZ, cW,
              bias, rand_amt, distance_function, minkowski_exp);
          if (lb >= (need_second_hit ? best_dist1 : best_dist0)) { continue; }
          std::uint32_t rng = use_time
              ? voronoi_lcg_random(fnv_hash5(cX, cY, cZ, cW, user_seed))
              : voronoi_lcg_random(fnv_hash4(cX, cY, cZ, user_seed));
          const std::uint32_t id_base = rng;
          const int n_pts = voronoi_prob_lookup(rng);
          for (int l = 0; l < n_pts; ++l) {
            rng = voronoi_lcg_random(rng);
            const float rx = static_cast<float>(rng - 1u) * kVoronoiInvRandModulus;
            rng = voronoi_lcg_random(rng);
            const float ry = static_cast<float>(rng - 1u) * kVoronoiInvRandModulus;
            rng = voronoi_lcg_random(rng);
            const float rz = static_cast<float>(rng - 1u) * kVoronoiInvRandModulus;
            float rw_val = 0.0f;
            if (use_time) {
              rng = voronoi_lcg_random(rng);
              rw_val = static_cast<float>(rng - 1u) * kVoronoiInvRandModulus;
            }
            const float px = static_cast<float>(cX) + bias + rand_amt * rx;
            const float py = static_cast<float>(cY) + bias + rand_amt * ry;
            const float pz = static_cast<float>(cZ) + bias + rand_amt * rz;
            const float pw = static_cast<float>(cW) + bias + rand_amt * rw_val;
            const float d = voronoi_distance(x - px, y - py, z - pz, wt - pw,
                                             use_time, distance_function, minkowski_exp);
            const std::uint32_t sid = id_base + static_cast<std::uint32_t>(l);
            if (d < best_dist0) {
              if (need_second_hit) { best_dist1 = best_dist0; best_id1 = best_id0; }
              best_dist0 = d; best_id0 = sid;
            } else if (need_second_hit && d < best_dist1) {
              best_dist1 = d; best_id1 = sid;
            }
          }
        }
      }
    }
  }
  if (!need_second_hit) { best_dist1 = best_dist0; best_id1 = best_id0; }
  return {best_dist0, best_dist1, best_id0, best_id1};
}

float voronoi_shape_value01(const VoronoiHit& hit, int distance_function, int shape_mode) {
  float d0 = hit.f1;
  float d1 = (hit.f2 > 1e19f) ? hit.f1 : hit.f2;
  if (distance_function == VORONOI_EUCLIDEAN) {
    d0 = std::sqrt(std::max(0.0f, d0));
    d1 = std::sqrt(std::max(0.0f, d1));
  }
  float value = 1.0f;
  switch (shape_mode) {
    case VORONOI_SHAPE_CELLS:   value = d0;         break;
    case VORONOI_SHAPE_CRYSTALS: value = d1;        break;
    case VORONOI_SHAPE_CRACKS:  value = d1 - d0;   break;
    case VORONOI_SHAPE_WEB:     value = d0 * d1;   break;
    case VORONOI_SHAPE_BUBBLES: value = 1.0f - d0; break;
    default:                    value = 1.0f;       break;
  }
  return clamp01(value);
}

float voronoi_noise_value(float x, float y, float z, float w, bool use_time,
                          int distance_function, int shape_mode,
                          float jitter_modifier, float minkowski_exp) {
  const int safe_shape = clamp_compat(shape_mode,
      static_cast<int>(VORONOI_SHAPE_VORONOI), static_cast<int>(VORONOI_SHAPE_BUBBLES));
  const bool need_second = (safe_shape != VORONOI_SHAPE_VORONOI);
  const VoronoiHit hit = voronoi_search(x, y, z, w, use_time, distance_function,
                                        jitter_modifier, minkowski_exp, need_second);
  float val01 = voronoi_shape_value01(hit, distance_function, safe_shape);
  return clamp_compat(val01 * 2.0f - 1.0f, -1.0f, 1.0f);
}

int sanitize_pattern_type_mode(int m) {
  if (m == PATTERN_SPIRAL) { return PATTERN_CONCENTRIC; }
  return clamp_compat(m, static_cast<int>(PATTERN_CONCENTRIC), static_cast<int>(PATTERN_RADIAL));
}

float simplex2_noise_value(float x, float y, float z, float w, bool use_time) {
  const float xy = x + y;
  const float s2 = xy * -0.2113248654f;
  const float zz = z * 0.5773502692f;
  const float sx = x + s2 - zz;
  const float sy = y + s2 - zz;
  const float sz = xy * 0.5773502692f + zz;
  const float sw = use_time ? (w * 0.5773502692f) : 0.0f;
  return noise4_compat(sx, sy, sz, sw + 17.13f);
}

void unit_complex_pow_int(float nx, float ny, int power, float& out_cos, float& out_sin) {
  if (power == 0) { out_cos = 1.0f; out_sin = 0.0f; return; }
  int p = (power < 0) ? -power : power;
  float rr = 1.0f, ri = 0.0f, br = nx, bi = ny;
  while (p > 0) {
    if ((p & 1) != 0) {
      const float nr = rr * br - ri * bi;
      const float ni = rr * bi + ri * br;
      rr = nr; ri = ni;
    }
    const float nbr = br * br - bi * bi;
    const float nbi = 2.0f * br * bi;
    br = nbr; bi = nbi;
    p >>= 1;
  }
  if (power < 0) { ri = -ri; }
  out_cos = rr; out_sin = ri;
}

float evaluate_pattern_val_for_mode(float x, float y, float z, float w, bool use_time,
                                    int pattern_type_mode, int pattern_segment_count,
                                    int pattern_twist) {
  pattern_type_mode = sanitize_pattern_type_mode(pattern_type_mode);
  const float time_phase = use_time ? w : 0.0f;
  float val = 0.0f;
  if (pattern_type_mode == PATTERN_CONCENTRIC) {
    const float dist = std::sqrt(x * x + y * y + z * z);
    const float phase = dist - time_phase;
    const float xy_len = std::sqrt(x * x + y * y);
    if (xy_len <= 1e-12f || pattern_twist == 0) {
      val = std::sin(phase);
    } else {
      const float nx_norm = x / xy_len, ny_norm = y / xy_len;
      float cos_tw = 1.0f, sin_tw = 0.0f;
      unit_complex_pow_int(nx_norm, ny_norm, pattern_twist, cos_tw, sin_tw);
      val = std::sin(phase) * cos_tw - std::cos(phase) * sin_tw;
    }
  } else if (pattern_type_mode == PATTERN_LINEAR) {
    val = std::sin(x - time_phase);
  } else if (pattern_type_mode == PATTERN_RADIAL) {
    const int segments = std::max(1, pattern_segment_count);
    const float xy_len = std::sqrt(x * x + y * y);
    if (xy_len <= 1e-12f) {
      val = std::sin(time_phase);
    } else {
      const float nx_norm = x / xy_len, ny_norm = y / xy_len;
      float cos_seg = 1.0f, sin_seg = 0.0f;
      unit_complex_pow_int(nx_norm, ny_norm, segments, cos_seg, sin_seg);
      val = sin_seg * std::cos(time_phase) + cos_seg * std::sin(time_phase);
    }
  }
  return clamp_compat(val, -1.0f, 1.0f);
}

float base_source_value_for_type(int noise_type, float x, float y, float z, float w,
                                 bool use_time, int voronoi_metric, int voronoi_shape_mode,
                                 float voronoi_jitter, float voronoi_minkowski_exp,
                                 int pattern_type_mode, int pattern_segment_count,
                                 int pattern_twist) {
  switch (noise_type) {
    case NOISE_SIMPLEX:
      return simplex2_noise_value(x, y, z, w, use_time);
    case NOISE_VORONOI:
      return voronoi_noise_value(x, y, z, w, use_time, voronoi_metric, voronoi_shape_mode,
                                 voronoi_jitter, voronoi_minkowski_exp);
    case NOISE_PATTERN:
      return evaluate_pattern_val_for_mode(x, y, z, w, use_time, pattern_type_mode,
                                           pattern_segment_count, pattern_twist);
    case NOISE_PERLIN:
    default:
      return use_time ? noise4_compat(x, y, z, w) : static_cast<float>(noise(x, y, z));
  }
}

float fractalized_value_for_settings(int noise_type, int fractal_mode,
                                     int real_octaves, double lacunarity,
                                     double gain, float x, float y, float z,
                                     float w, bool use_time,
                                     int voronoi_metric, int voronoi_shape_mode,
                                     float voronoi_jitter, float voronoi_minkowski_exp,
                                     int pattern_type_mode, int pattern_segment_count,
                                     int pattern_twist) {
  const int oct = (fractal_mode == FRACTAL_NONE) ? 1 : std::max(1, real_octaves);
  const double lac = std::max(1e-4, lacunarity);
  const double g = std::max(0.0, gain);
  double sum = 0.0, amp = 1.0, freq = 1.0, amp_sum = 0.0;
  for (int i = 0; i < oct; ++i) {
    float n = base_source_value_for_type(
        noise_type, x * static_cast<float>(freq), y * static_cast<float>(freq),
        z * static_cast<float>(freq), w * static_cast<float>(freq), use_time,
        voronoi_metric, voronoi_shape_mode, voronoi_jitter, voronoi_minkowski_exp,
        pattern_type_mode, pattern_segment_count, pattern_twist);
    if (fractal_mode == FRACTAL_BILLOW) {
      n = 2.0f * std::fabs(n) - 1.0f;
    } else if (fractal_mode == FRACTAL_RIDGED) {
      float r = 1.0f - std::fabs(n);
      n = r * r * 2.0f - 1.0f;
    }
    sum += static_cast<double>(n) * amp;
    amp_sum += amp;
    freq *= lac;
    amp *= g;
  }
  if (amp_sum <= 1e-9) { return 0.0f; }
  return clamp_compat(static_cast<float>(sum / amp_sum), -1.0f, 1.0f);
}

// Octave table variant â€” avoids recomputing freq/amp per pixel.
float fractalized_value_with_tables(int noise_type, int fractal_mode,
                                    int oct, const float* freqs, const float* amps,
                                    float amp_sum,
                                    float x, float y, float z, float w, bool use_time,
                                    int voronoi_metric, int voronoi_shape_mode,
                                    float voronoi_jitter, float voronoi_minkowski_exp,
                                    int pattern_type_mode, int pattern_segment_count,
                                    int pattern_twist) {
  if (amp_sum <= 1e-9f || oct <= 0) { return 0.0f; }
  double sum = 0.0;
  for (int i = 0; i < oct; ++i) {
    const float f = freqs[i];
    float n = base_source_value_for_type(
        noise_type, x * f, y * f, z * f, w * f, use_time,
        voronoi_metric, voronoi_shape_mode, voronoi_jitter, voronoi_minkowski_exp,
        pattern_type_mode, pattern_segment_count, pattern_twist);
    if (fractal_mode == FRACTAL_BILLOW) {
      n = 2.0f * std::fabs(n) - 1.0f;
    } else if (fractal_mode == FRACTAL_RIDGED) {
      float r = 1.0f - std::fabs(n);
      n = r * r * 2.0f - 1.0f;
    }
    sum += static_cast<double>(n) * static_cast<double>(amps[i]);
  }
  return clamp_compat(static_cast<float>(sum / static_cast<double>(amp_sum)), -1.0f, 1.0f);
}

static void build_fractal_octave_tables(int fractal_mode, int real_octaves,
                                        float lacunarity, float gain,
                                        std::vector<float>& out_freqs,
                                        std::vector<float>& out_amps,
                                        float& out_amp_sum) {
  const int oct = (fractal_mode == FRACTAL_NONE) ? 1 : std::max(1, real_octaves);
  out_freqs.resize(static_cast<size_t>(oct));
  out_amps.resize(static_cast<size_t>(oct));
  const double lac = std::max(1e-4, static_cast<double>(lacunarity));
  const double g = std::max(0.0, static_cast<double>(gain));
  double freq = 1.0, amp = 1.0, amp_sum = 0.0;
  for (int i = 0; i < oct; ++i) {
    out_freqs[static_cast<size_t>(i)] = static_cast<float>(freq);
    out_amps[static_cast<size_t>(i)] = static_cast<float>(amp);
    amp_sum += amp;
    freq *= lac;
    amp *= g;
  }
  out_amp_sum = static_cast<float>(amp_sum);
}

struct NoiseRGB { float r; float g; float b; };

NoiseRGB evaluate_voronoi_shape_vector_from_point(float x, float y, float z, float w,
                                                  bool use_time, bool pref_space,
                                                  int distance_function, float jitter_modifier,
                                                  float minkowski_exp,
                                                  int fractal_mode, int octaves,
                                                  double lacunarity, double gain,
                                                  float vector_offset) {
  const int oct = (fractal_mode == FRACTAL_NONE) ? 1 : std::max(1, octaves);
  const double lac = std::max(1e-4, lacunarity);
  const double g = std::max(0.0, gain);
  const float phase = std::max(0.0f, vector_offset);
  const float rot = deg_to_rad(phase * 3.6f);
  const float ca = std::cos(rot), sa = std::sin(rot);

  NoiseRGB accum = {0.0f, 0.0f, 0.0f};
  double amp = 1.0, freq = 1.0, amp_sum = 0.0;
  for (int i = 0; i < oct; ++i) {
    const float sf = static_cast<float>(freq);
    const float per_oct_phase = phase * 0.01f * static_cast<float>(i + 1);
    const float px = x * sf + per_oct_phase;
    const float py = y * sf - per_oct_phase * 0.73f;
    const float pz = z * sf + per_oct_phase * 0.41f;
    const float pw_val = use_time ? (w * sf + per_oct_phase * 0.19f) : 0.0f;
    const VoronoiHit hit = voronoi_search(px, py, pz, pw_val, use_time,
                                          distance_function, jitter_modifier, minkowski_exp, false);
    const int cid = static_cast<int>(hit.id1);
    float vx = hash_signed(fnv_hash4(cid, 0, 97 + i * 31, 0xA341316Cu));
    float vy = hash_signed(fnv_hash4(cid, 1, 173 + i * 31, 0xC8013EA4u));
    float vz = pref_space ? hash_signed(fnv_hash4(cid, 2, 251 + i * 31, 0xAD90777Du)) : 0.0f;
    const float len = pref_space ? std::sqrt(vx * vx + vy * vy + vz * vz)
                                 : std::sqrt(vx * vx + vy * vy);
    if (len > 1e-6f) { const float inv = 1.0f / len; vx *= inv; vy *= inv; vz *= inv; }
    else { vx = 1.0f; vy = 0.0f; vz = 0.0f; }
    const float wa = static_cast<float>(amp);
    accum.r += (ca * vx - sa * vy) * wa;
    accum.g += (sa * vx + ca * vy) * wa;
    accum.b += (pref_space ? vz : 0.0f) * wa;
    amp_sum += amp;
    freq *= lac;
    amp *= g;
  }
  if (amp_sum > 1e-9) {
    const float inv = static_cast<float>(1.0 / amp_sum);
    accum.r *= inv; accum.g *= inv; accum.b *= inv;
  }
  return {clamp_compat(accum.r, -1.0f, 1.0f),
          clamp_compat(accum.g, -1.0f, 1.0f),
          clamp_compat(accum.b, -1.0f, 1.0f)};
}

NoiseRGB evaluate_pattern_vector_from_point_for_mode(float x, float y, float z,
                                                     float w, bool use_time,
                                                     bool pref_space,
                                                     int pattern_type_mode,
                                                     int pattern_segment_count,
                                                     int pattern_twist,
                                                     int fractal_mode,
                                                     int real_octaves,
                                                     double lacunarity, double gain) {
  pattern_type_mode = sanitize_pattern_type_mode(pattern_type_mode);
  const float dir_x = x, dir_y = y, dir_z = z;
  const float len_metric = std::sqrt(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
  float metric_gx = 1.0f, metric_gy = 0.0f, metric_gz = 0.0f;
  if (len_metric > 1e-6f) {
    const float inv = 1.0f / len_metric;
    metric_gx = dir_x * inv; metric_gy = dir_y * inv; metric_gz = dir_z * inv;
  }
  const float dir_r2 = dir_x * dir_x + dir_y * dir_y;
  float tangent_x = 0.0f, tangent_y = 1.0f;
  if (dir_r2 > 1e-8f) {
    const float inv_r = 1.0f / std::sqrt(dir_r2);
    tangent_x = -(dir_y * inv_r);
    tangent_y = dir_x * inv_r;
  }
  const float pattern_signed = fractalized_value_for_settings(
      NOISE_PATTERN, fractal_mode, real_octaves, lacunarity, gain,
      x, y, z, w, use_time, VORONOI_EUCLIDEAN, VORONOI_SHAPE_VORONOI, 1.0f, 1.0f,
      pattern_type_mode, pattern_segment_count, pattern_twist);
  float vector_weight = pattern_signed;
  float vx = 0.0f, vy = 0.0f, vz = 0.0f;
  const float tw = static_cast<float>(pattern_twist);
  if (pattern_type_mode == PATTERN_CONCENTRIC) {
    vx = -metric_gx + tangent_x * tw;
    vy = -metric_gy + tangent_y * tw;
    vz = -metric_gz;
  } else if (pattern_type_mode == PATTERN_LINEAR) {
    vx = 0.0f; vy = 1.0f; vz = 0.0f;
    vector_weight = std::max(0.0f, pattern_signed);
  } else if (pattern_type_mode == PATTERN_RADIAL) {
    vx = -metric_gx; vy = -metric_gy; vz = -metric_gz;
  }
  float len = pref_space ? std::sqrt(vx * vx + vy * vy + vz * vz)
                         : std::sqrt(vx * vx + vy * vy);
  if (len > 1e-6f) { const float inv = 1.0f / len; vx *= inv; vy *= inv; vz *= inv; }
  else { vx = 1.0f; vy = 0.0f; vz = 0.0f; }
  vx *= vector_weight; vy *= vector_weight; vz *= vector_weight;
  return {vx, vy, pref_space ? vz : 0.0f};
}

float domainwarp_amount_scale_from_knob(double domainwarp_amount_value) {
  const float n = std::max(0.0f, static_cast<float>(domainwarp_amount_value) * 0.01f);
  return 3000.0f * n * n;
}

float zspeed_scale_for_noise_type(int noise_type);

float animated_seed_from_z_params(double zoffset, double zspeed,
                                  float frame, int noise_type,
                                  bool apply_2d_tuning = true) {
  const float z_anim = static_cast<float>(zspeed * 0.01) *
                       (apply_2d_tuning ? zspeed_scale_for_noise_type(noise_type) : 1.0f) *
                       frame;
  return static_cast<float>(zoffset) + z_anim;
}

float zspeed_scale_for_noise_type(int noise_type) {
  switch (noise_type) {
    case NOISE_PERLIN:
    case NOISE_SIMPLEX:
    case NOISE_VORONOI: return 0.1f;
    case NOISE_PATTERN: return 2.0f;
    default:            return 1.0f;
  }
}

float shape_distance(int shape, float dx, float dy, float dz) {
  switch (shape) {
    case SHAPE_CUBE:       return std::max(std::fabs(dx), std::max(std::fabs(dy), std::fabs(dz)));
    case SHAPE_CYLINDER_X: return std::sqrt(dy * dy + dz * dz);
    case SHAPE_CYLINDER_Y: return std::sqrt(dx * dx + dz * dz);
    case SHAPE_CYLINDER_Z: return std::sqrt(dx * dx + dy * dy);
    case SHAPE_DIAMOND:    return (std::fabs(dx) + std::fabs(dy) + std::fabs(dz)) * 0.57735026919f;
    case SHAPE_PLANE_X:    return std::fabs(dx);
    case SHAPE_PLANE_Y:    return std::fabs(dy);
    case SHAPE_PLANE_Z:    return std::fabs(dz);
    case SHAPE_SPHERE:
    default:               return std::sqrt(dx * dx + dy * dy + dz * dz);
  }
}

}  // namespace

// ===========================================================================
// TMaskBase
// ===========================================================================

class TMaskBase : public Iop {
 public:
  explicit TMaskBase(Node* node) : Iop(node) {
    inputs(1);
    output_channels_[0] = Chan_Red;
    output_channels_[1] = Chan_Green;
    output_channels_[2] = Chan_Blue;
    output_channels_[3] = Chan_Alpha;
    pref_channels_[0] = Chan_Red;
    pref_channels_[1] = Chan_Green;
    pref_channels_[2] = Chan_Blue;
    pref_channels_[3] = Chan_Alpha;
    for (int i = 0; i < kMaxMasks; ++i) {
      mask_center_[i][0] = 0.0;
      mask_center_[i][1] = 0.0;
      mask_translate_[i][0] = 0.0;
      mask_translate_[i][1] = 0.0;
      mask_translate_[i][2] = 0.0;
      mask_scale_[i][0] = 1.0;
      mask_scale_[i][1] = 1.0;
      mask_scale_[i][2] = 1.0;
      mask_size_[i] = 5.0;
      mask_falloff_[i] = 3.0;
      mask_blur_[i] = 0.0;
      mask_falloff_mode_[i] = FALLOFF_LINEAR;
      mask_shape_[i] = SHAPE_SPHERE;
      mask_mirror_x_[i] = false;
      mask_mirror_y_[i] = false;
      mask_mirror_z_[i] = false;
      mask_slot_initialized_[i] = false;
      pref_center_cached_[i][0] = 0.0f;
      pref_center_cached_[i][1] = 0.0f;
      pref_center_cached_[i][2] = 0.0f;
      pref_center_cached_valid_[i] = false;
      pref_center_cache_dirty_[i] = true;
    }
    noise_invmatrix_.makeIdentity();
  }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  int optional_input() const override { return 2; }
  int minimum_inputs() const override { return 3; }
  int maximum_inputs() const override { return 3; }
  const char* input_label(int input, char*) const override {
    if (input == 0) { return "Pref"; }
    if (input == 1) { return "Warp"; }
    if (input == 2) { return "mask"; }
    return nullptr;
  }

  bool has_mask_input() const {
    Op* mask_input = input(2);
    if (!mask_input) { return false; }
    Iop* mask_iop = mask_input->iop();
    if (!mask_iop) { return false; }
#if defined(kDDImageVersionMajorNum) && (kDDImageVersionMajorNum >= 14)
    return !mask_iop->isBlackIop();
#else
    Iop* default_iop = Iop::default_input(outputContext());
    return default_iop ? (mask_iop != default_iop) : true;
#endif
  }

  bool has_mask_input_runtime() const { return has_mask_input(); }

  bool has_warp_input() const {
    Op* warp_input = input(1);
    if (!warp_input) { return false; }
    Iop* warp_iop = warp_input->iop();
    if (!warp_iop) { return false; }
#if defined(kDDImageVersionMajorNum) && (kDDImageVersionMajorNum >= 14)
    return !warp_iop->isBlackIop();
#else
    Iop* default_iop = Iop::default_input(outputContext());
    return default_iop ? (warp_iop != default_iop) : true;
#endif
  }

  bool has_warp_input_runtime() const { return has_warp_input(); }

  // ---------------------------------------------------------------------------
  // knobs
  // ---------------------------------------------------------------------------
  void knobs(Knob_Callback f) override {
    Tab_knob(f, "TMask");

    BeginGroup(f, "output_group", "Output");
    Channel_knob(f, output_channels_, 4, "channels");
    Tooltip(f, "Output channels. RGB keeps input preview with red mask overlay; alpha writes the mask.");
    Channel_knob(f, pref_channels_, 4, "pref_channels", "P channels");
    Tooltip(f, "P/Pref channels used to lock center and evaluate distance.");
    Bool_knob(f, &pref_keep_alpha_, "pref_keep_alpha", "Keep Alpha");
    ClearFlags(f, Knob::STARTLINE);
    EndGroup(f);
    Divider(f);

    Int_knob(f, &mask_count_, "mask_count", "Mask Number");
    SetRange(f, 1.0, static_cast<double>(kMaxMasks));
    Button(f, "mask_add", "+");
    ClearFlags(f, Knob::STARTLINE);
    Button(f, "mask_remove", "-");
    ClearFlags(f, Knob::STARTLINE);
    if (Knob* d = Divider(f)) { d->name("mask_count_divider"); d->label(""); }
    Double_knob(f, &global_size_, "global_size", "global size");
    SetRange(f, 0.0, 10.0);
    Tooltip(f, "Global multiplier applied to every mask size.");
    Double_knob(f, &global_falloff_, "global_falloff", "global falloff");
    SetRange(f, 0.0, 10.0);
    Tooltip(f, "Global multiplier applied to every mask falloff.");
    if (Knob* d = Divider(f)) { d->name("global_controls_divider"); d->label(""); }

    for (int i = 0; i < kMaxMasks; ++i) {
      const int idx = i + 1;
      const std::string idx_str = std::to_string(idx);
      const std::string group_name = "mask_" + idx_str + "_group";
      const std::string group_label = "Mask " + idx_str;
      const std::string shape_name = "mask_" + idx_str + "_shape";
      const std::string center_name = "mask_" + idx_str + "_center";
      const std::string translate_name = "mask_" + idx_str + "_translate";
      const std::string scale_name = "mask_" + idx_str + "_scale";
      const std::string size_name = "mask_" + idx_str + "_size";
      const std::string falloff_name = "mask_" + idx_str + "_falloff";
      const std::string blur_name = "mask_" + idx_str + "_blur";
      const std::string falloff_mode_name = "mask_" + idx_str + "_falloff_mode";
      const std::string mirror_x_name = "mask_" + idx_str + "_mirror_x";
      const std::string mirror_y_name = "mask_" + idx_str + "_mirror_y";
      const std::string mirror_z_name = "mask_" + idx_str + "_mirror_z";
      const std::string mirror_group_name = "mask_" + idx_str + "_mirror_group";
      const std::string divider_a_name = "mask_" + idx_str + "_divider_a";
      const std::string divider_b_name = "mask_" + idx_str + "_divider_b";
      const std::string divider_c_name = "mask_" + idx_str + "_divider_c";
      const std::string divider_d_name = "mask_" + idx_str + "_divider_d";
      const std::string center_label = "mask " + idx_str;

      BeginGroup(f, group_name.c_str(), group_label.c_str());
      Enumeration_knob(f, &mask_shape_[i], kShapeModes, shape_name.c_str(), "shape");
      if (Knob* d = Divider(f)) { d->name(divider_a_name.c_str()); d->label(""); }
      XY_knob(f, mask_center_[i], center_name.c_str(), center_label.c_str());
      SetRange(f, -20000.0, 20000.0);
      Tooltip(f, "2D picker position used to lock P center for this mask.");
      XYZ_knob(f, mask_translate_[i], translate_name.c_str(), "translate");
      SetRange(f, -10000.0, 10000.0);
      Tooltip(f, "Offset applied after locked center.");
      XYZ_knob(f, mask_scale_[i], scale_name.c_str(), "scale");
      SetRange(f, 0.001, 100.0);
      Tooltip(f, "Per-mask xyz scale.");
      if (Knob* d = Divider(f)) { d->name(divider_b_name.c_str()); d->label(""); }
      Double_knob(f, &mask_size_[i], size_name.c_str(), "size");
      SetRange(f, 0.0, 100.0);
      Tooltip(f, "Size range 0-100. Effective size is divided by 2.");
      Double_knob(f, &mask_falloff_[i], falloff_name.c_str(), "falloff");
      SetRange(f, 0.0, 100.0);
      Tooltip(f, "Falloff range 0-100.");
      Double_knob(f, &mask_blur_[i], blur_name.c_str(), "blur");
      SetRange(f, 0.0, 100.0);
      Tooltip(f, "Additional blur radius in P space for this mask.");
      Enumeration_knob(f, &mask_falloff_mode_[i], kFalloffModes, falloff_mode_name.c_str(), "mode");
      ClearFlags(f, Knob::STARTLINE);
      if (Knob* d = Divider(f)) { d->name(divider_c_name.c_str()); d->label(""); }
      BeginGroup(f, mirror_group_name.c_str(), "Mirror");
      Bool_knob(f, &mask_mirror_x_[i], mirror_x_name.c_str(), "x");
      Tooltip(f, "Mirror X.");
      Bool_knob(f, &mask_mirror_y_[i], mirror_y_name.c_str(), "y");
      Tooltip(f, "Mirror Y.");
      Bool_knob(f, &mask_mirror_z_[i], mirror_z_name.c_str(), "z");
      Tooltip(f, "Mirror Z.");
      EndGroup(f);
      if (Knob* d = Divider(f)) { d->name(divider_d_name.c_str()); d->label(""); }
      EndGroup(f);
    }

    Double_knob(f, &mix_, "mix", "mix");
    SetRange(f, 0.0, 1.0);
    Tooltip(f, "Global opacity for the final mask.");
    Bool_knob(f, &invert_mask_, "invert_mask", "invert");
    ClearFlags(f, Knob::STARTLINE);

    Tab_knob(f, "Domain Warping");
    // -----------------------------------------------------------------------
    // Domain Warp group (mirrors TNoise domain warp layout)
    // -----------------------------------------------------------------------
    BeginGroup(f, "noise_group", "Domain Warp");
    Double_knob(f, &noise_amount_, "noise_amount", "amount");
    SetRange(f, 0.0, 100.0);
    Tooltip(f, "Domain-warp strength. In 3D (PRef), effective amount is scaled by /50.");
    Bool_knob(f, &show_noise_, "show_noise", "show map");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Display the domain-warp vector map instead of the mask overlay.");
    if (Knob* d = Divider(f)) { d->name("noise_output_divider_1"); d->label(""); }

    // --- Noise subgroup ---
    BeginGroup(f, "noise_noise_group", "Noise");
    Enumeration_knob(f, &noise_type_, kNoiseTypes, "noise_type", "type");
    Enumeration_knob(f, &noise_fractal_mode_, kFractalModes, "noise_fractal", "fractal");
    ClearFlags(f, Knob::STARTLINE);
    Divider(f);
    Scale_knob(f, &noise_xsize_, "noise_size", "x/ysize");
    SetRange(f, 1.0, 1000.0);
    Double_knob(f, &noise_zoffset_, "noise_zoffset", "z");
    SetRange(f, 0.0, 5.0);
    Double_knob(f, &noise_zspeed_, "noise_zspeed", "z speed");
    SetRange(f, 0.0, 100.0);
    Tooltip(f, "Auto animation speed. Effective Z = z + frame * (z speed / 100).");
    Divider(f);
    Int_knob(f, &noise_octaves_, "noise_octaves", "octaves");
    SetRange(f, 1.0, 10.0);
    Int_knob(f, &noise_voronoi_octaves_, "noise_octaves_voronoi", "octaves");
    SetRange(f, 1.0, 10.0);
    Int_knob(f, &noise_pattern_octaves_, "noise_octaves_pattern", "octaves");
    SetRange(f, 1.0, 10.0);
    Double_knob(f, &noise_lacunarity_, "noise_lacunarity", "lacunarity");
    SetRange(f, 0.0, 10.0);
    Double_knob(f, &noise_gain_, "noise_gain", "gain");
    SetRange(f, 0.0, 2.0);
    Float_knob(f, &noise_gamma_, "noise_gamma", "gamma");
    SetRange(f, 0.0, 2.0);
    Divider(f);
    // Voronoi sub-group
    BeginGroup(f, "noise_voronoi_group", "Voronoi");
    Enumeration_knob(f, &noise_voronoi_shape_mode_, kVoronoiShapeModes,
                     "noise_voronoi_shape_mode", "shape");
    Tooltip(f, "Voronoi style for the warp field.");
    Divider(f);
    Enumeration_knob(f, &noise_voronoi_metric_, kVoronoiMetrics,
                     "noise_voronoi_metric", "distance function");
    Divider(f);
    Double_knob(f, &noise_voronoi_minkowski_exp_, "noise_voronoi_minkowski_exp", "minkowski exp");
    SetRange(f, 0.1, 8.0);
    if (Knob* d = Divider(f)) { d->name("noise_voronoi_minkowski_divider"); d->label(""); }
    Double_knob(f, &noise_voronoi_randomness_, "noise_voronoi_randomness", "jitter");
    SetRange(f, 0.0, 2.0);
    Divider(f);
    EndGroup(f);  // noise_voronoi_group
    // Pattern sub-group
    BeginGroup(f, "noise_pattern_group", "Pattern");
    Enumeration_knob(f, &noise_pattern_type_mode_, kPatternTypeModes,
                     "noise_pattern_type_mode", "shape");
    Enumeration_knob(f, &noise_pattern_shape_mode_, kPatternShapeModes,
                     "noise_pattern_shape_mode", "metric");
    ClearFlags(f, Knob::STARTLINE);
    if (Knob* d = Divider(f)) { d->name("noise_pattern_shape_divider"); d->label(""); }
    Int_knob(f, &noise_pattern_segment_count_, "noise_pattern_segment_count", "segments");
    SetRange(f, 1.0, 100.0);
    Int_knob(f, &noise_pattern_twist_, "noise_pattern_twist", "twist");
    SetRange(f, -10.0, 10.0);
    Divider(f);
    EndGroup(f);  // noise_pattern_group
    // divergence_mix is hidden (kept for compatibility/future use)
    Double_knob(f, &noise_divergence_mix_, "noise_divergence_mix", "divergence mix");
    SetRange(f, 0.0, 1.0);
    if (Knob* d = Divider(f)) { d->name("noise_divergence_divider"); d->label(""); }
    EndGroup(f);  // noise_noise_group

    // --- Transform subgroup ---
    BeginGroup(f, "noise_transform_group", "Transform");
    MultiFloat_knob(f, noise_translate_, 3, "noise_translate", "translate");
    SetRange(f, -1000.0, 1000.0);
    SetFlags(f, Knob::SLIDER | Knob::MAGNITUDE | Knob::NO_HANDLES | Knob::NO_PROXYSCALE);
    Tooltip(f, "Offset applied in noise space.");
    Divider(f);
    Double_knob(f, &noise_translate_speed_, "noise_translate_speed", "translate speed");
    SetRange(f, 0.0, 100.0);
    Tooltip(f, "Auto translation speed per frame.");
    Double_knob(f, &noise_translate_angle_, "noise_translate_angle", "translate angle");
    SetRange(f, -180.0, 180.0);
    Divider(f);
    MultiFloat_knob(f, noise_rotate_, 3, "noise_rotate", "rotate");
    SetRange(f, -180.0, 180.0);
    SetFlags(f, Knob::SLIDER | Knob::MAGNITUDE | Knob::NO_HANDLES | Knob::NO_PROXYSCALE);
    Tooltip(f, "Rotation XYZ applied in noise space. In 2D mode only Z is used.");
    MultiFloat_knob(f, noise_scale_, 3, "noise_scale", "scale");
    SetRange(f, 0.001, 1000.0);
    SetFlags(f, Knob::SLIDER | Knob::LOG_SLIDER | Knob::MAGNITUDE | Knob::NO_HANDLES | Knob::NO_PROXYSCALE);
    Tooltip(f, "Non-uniform scale multiplier applied to the noise size.");
    Divider(f);
    MultiFloat_knob(f, noise_skew_, 3, "noise_skew", "skew");
    SetRange(f, -10.0, 10.0);
    SetFlags(f, Knob::SLIDER | Knob::MAGNITUDE | Knob::NO_HANDLES | Knob::NO_PROXYSCALE);
    Tooltip(f, "Skew XYZ. In PRef, boosted x10 for visible effect.");
    Divider(f);
    XY_knob(f, noise_center_, "noise_center", "center");
    SetFlags(f, Knob::NO_PROXYSCALE);
    Tooltip(f, "Pivot for 2D rotation. In PRef mode, used as warp origin.");
    Button(f, "noise_reset_center", "Reset Center");
    ClearFlags(f, Knob::STARTLINE);
    Divider(f);
    XY_knob(f, noise_rotate2d_xy_, "noise_xyrotate", "x/yrotate");
    SetRange(f, -180.0, 180.0);
    SetFlags(f, Knob::SLIDER | Knob::MAGNITUDE | Knob::NO_HANDLES | Knob::NO_PROXYSCALE);
    Tooltip(f, "2D-only X/Y rotation. Ignored when PRef input is connected.");
    Divider(f);
    EndGroup(f);  // noise_transform_group

    Button(f, "noise_restore_defaults", "Restore Defaults");
    EndGroup(f);  // noise_group
  }

  // ---------------------------------------------------------------------------
  // knob_changed
  // ---------------------------------------------------------------------------
  int knob_changed(Knob* k) override {
    if (!k) { return Iop::knob_changed(k); }
    if (ui_knob_sync_in_progress_) { return Iop::knob_changed(k); }
    ScopedUiSyncGuard ui_sync_guard(ui_knob_sync_in_progress_);
    const std::string knob_name = k->name();
    if (knob_name.empty()) { return Iop::knob_changed(k); }

    if (k == &Knob::showPanel) {
      ui_panel_opened_ = true;
      if (!mask_slot_initialized_[0]) {
        if (mask_slot_is_at_defaults(0, false)) {
          initialize_mask_slot_defaults(0, true);
          apply_mask_slot_knobs(0);
        } else {
          mask_slot_initialized_[0] = true;
        }
      }
      mask_count_ui_cached_ = clamp_compat(mask_count_, 1, kMaxMasks);
      update_noise_type_ui();
      update_masks_ui();
      return 1;
    }

    int direct_center_index = -1;
    if (find_mask_center_knob_index(knob_name, direct_center_index) &&
        direct_center_index >= 0 &&
        direct_center_index < kMaxMasks) {
      {
        Guard guard(pref_center_cache_lock_);
        pref_center_cache_dirty_[direct_center_index] = true;
      }
      mask_slot_initialized_[direct_center_index] = true;
      return 1;
    }

    if (knob_name == "noise_center" || knob_name.rfind("noise_center.", 0) == 0) {
      {
        Guard guard(pref_center_cache_lock_);
        noise_pref_center_cache_dirty_ = true;
      }
      return 1;
    }

    // Reset center button
    if (knob_name == "noise_reset_center") {
      const Format& f = format();
      double nc[2] = {
          0.5 * (static_cast<double>(f.x()) + static_cast<double>(f.r())),
          0.5 * (static_cast<double>(f.y()) + static_cast<double>(f.t())),
      };
      if (Knob* ck = knob("noise_center")) {
        ck->set_value(nc[0], 0);
        ck->set_value(nc[1], 1);
      }
      {
        Guard guard(pref_center_cache_lock_);
        noise_pref_center_cache_dirty_ = true;
      }
      node_redraw();
      return 1;
    }

    if (knob_name == "mask_add") {
      if (mask_count_ < kMaxMasks) {
        const int new_index = mask_count_;
        ++mask_count_;
        if (!mask_slot_initialized_[new_index]) {
          initialize_mask_slot_defaults(new_index, true);
          apply_mask_slot_knobs(new_index);
        }
      }
      if (Knob* mk = knob("mask_count")) { mk->set_value(mask_count_); }
      mask_count_ui_cached_ = mask_count_;
      update_masks_ui();
      update_noise_type_ui();
      node_redraw();
      return 1;
    }

    if (knob_name == "mask_remove") {
      if (mask_count_ > 1) {
        --mask_count_;
        reset_mask_slot(mask_count_);
      }
      if (Knob* mk = knob("mask_count")) { mk->set_value(mask_count_); }
      mask_count_ui_cached_ = mask_count_;
      update_masks_ui();
      update_noise_type_ui();
      node_redraw();
      return 1;
    }

    if (knob_name == "mask_count") {
      const int previous_count = clamp_compat(mask_count_ui_cached_, 1, kMaxMasks);
      mask_count_ = clamp_compat(mask_count_, 1, kMaxMasks);
      if (mask_count_ > previous_count) {
        for (int i = previous_count; i < mask_count_; ++i) {
          if (!mask_slot_initialized_[i]) {
            initialize_mask_slot_defaults(i, true);
            apply_mask_slot_knobs(i);
          }
        }
      }
      if (Knob* mk = knob("mask_count")) { mk->set_value(mask_count_); }
      mask_count_ui_cached_ = mask_count_;
      update_masks_ui();
      update_noise_type_ui();
      node_redraw();
      return 1;
    }

    sanitize_output_channels();
    sanitize_pref_channels();
    const bool pref_changed = (knob_name == "pref_channels" ||
                               knob_name.rfind("pref_channels.", 0) == 0 ||
                               knob_name == "pref_keep_alpha");
    const int prior_mask_count = mask_count_;
    mask_count_ = clamp_compat(mask_count_, 1, kMaxMasks);
    if (Knob* mk = knob("mask_count")) { mk->set_value(mask_count_); }

    // Sanitize core mask params (no hard max cap, only finite + semantic minimums)
    for (int i = 0; i < kMaxMasks; ++i) {
      mask_center_[i][0] = sanitize_finited(mask_center_[i][0], 0.0);
      mask_center_[i][1] = sanitize_finited(mask_center_[i][1], 0.0);
      mask_translate_[i][0] = sanitize_finited(mask_translate_[i][0], 0.0);
      mask_translate_[i][1] = sanitize_finited(mask_translate_[i][1], 0.0);
      mask_translate_[i][2] = sanitize_finited(mask_translate_[i][2], 0.0);
      mask_scale_[i][0] = std::max(0.001, sanitize_finited(mask_scale_[i][0], 1.0));
      mask_scale_[i][1] = std::max(0.001, sanitize_finited(mask_scale_[i][1], 1.0));
      mask_scale_[i][2] = std::max(0.001, sanitize_finited(mask_scale_[i][2], 1.0));
      mask_size_[i] = std::max(0.0, sanitize_finited(mask_size_[i], 0.0));
      mask_falloff_[i] = std::max(0.0, sanitize_finited(mask_falloff_[i], 0.0));
      mask_blur_[i] = std::max(0.0, sanitize_finited(mask_blur_[i], 0.0));
      mask_falloff_mode_[i] = clamp_compat(mask_falloff_mode_[i],
          static_cast<int>(FALLOFF_LINEAR), static_cast<int>(FALLOFF_SMOOTH));
      mask_shape_[i] = clamp_compat(mask_shape_[i],
          static_cast<int>(SHAPE_SPHERE), static_cast<int>(SHAPE_PLANE_Z));
    }
    global_size_ = std::max(0.0, sanitize_finited(global_size_, 1.0));
    global_falloff_ = std::max(0.0, sanitize_finited(global_falloff_, 1.0));
    mix_ = clamp_compat(mix_, 0.0, 1.0);

    // Sanitize domain warp core
    noise_amount_ = std::max(0.0, sanitize_finited(noise_amount_, 0.0));

    // Clamp noise params
    noise_type_ = clamp_compat(noise_type_,
        static_cast<int>(NOISE_PERLIN), static_cast<int>(NOISE_PATTERN));
    noise_fractal_mode_ = clamp_compat(noise_fractal_mode_,
        static_cast<int>(FRACTAL_NONE), static_cast<int>(FRACTAL_RIDGED));
    noise_xsize_ = std::max(0.000001, sanitize_finited(noise_xsize_, 350.0));
    noise_ysize_ = std::max(0.000001, sanitize_finited(noise_ysize_, 350.0));
    noise_zoffset_ = std::max(0.0, sanitize_finited(noise_zoffset_, 0.0));
    noise_zspeed_  = std::max(0.0, sanitize_finited(noise_zspeed_, 0.0));
    noise_octaves_ = std::max(1, noise_octaves_);
    noise_voronoi_octaves_ = std::max(1, noise_voronoi_octaves_);
    noise_pattern_octaves_ = std::max(1, noise_pattern_octaves_);
    noise_lacunarity_ = std::max(0.0, sanitize_finited(noise_lacunarity_, 2.0));
    noise_gain_      = std::max(0.0, sanitize_finited(noise_gain_, 0.5));
    noise_gamma_     = std::max(0.0f, static_cast<float>(sanitize_finited(noise_gamma_, 0.5)));
    noise_voronoi_metric_ = clamp_compat(noise_voronoi_metric_,
        static_cast<int>(VORONOI_EUCLIDEAN), static_cast<int>(VORONOI_MINKOWSKI));
    noise_voronoi_shape_mode_ = clamp_compat(noise_voronoi_shape_mode_,
        static_cast<int>(VORONOI_SHAPE_VORONOI), static_cast<int>(VORONOI_SHAPE_BUBBLES));
    noise_voronoi_minkowski_exp_ = std::max(0.1, noise_voronoi_minkowski_exp_);
    noise_voronoi_randomness_    = std::max(0.0, noise_voronoi_randomness_);
    noise_pattern_type_mode_  = sanitize_pattern_type_mode(noise_pattern_type_mode_);
    noise_pattern_shape_mode_ = PATTERN_SHAPE_ROUND;
    noise_pattern_segment_count_ = std::max(1, noise_pattern_segment_count_);
    noise_pattern_twist_ = sanitize_finited(noise_pattern_twist_, 0.0);
    noise_divergence_mix_ = clamp_compat(noise_divergence_mix_, 0.0, 1.0);

    // Sanitize transform params
    noise_translate_speed_ = std::max(0.0, sanitize_finited(noise_translate_speed_, 0.0));
    noise_translate_angle_ = sanitize_finited(noise_translate_angle_, 0.0);
    for (int i = 0; i < 3; ++i) {
      noise_translate_[i] = sanitize_finited(noise_translate_[i], 0.0);
      noise_scale_[i]     = std::max(0.001, sanitize_finited(noise_scale_[i], 1.0));
      noise_skew_[i]      = sanitize_finited(noise_skew_[i], 0.0);
      noise_rotate_[i]    = sanitize_finited(noise_rotate_[i], 0.0);
    }
    for (int i = 0; i < 2; ++i) {
      noise_rotate2d_xy_[i] = sanitize_finited(noise_rotate2d_xy_[i], 0.0);
    }

    if (pref_changed || prior_mask_count != mask_count_) {
      Guard guard(pref_center_cache_lock_);
      for (int i = 0; i < kMaxMasks; ++i) {
        pref_center_cache_dirty_[i] = true;
        pref_center_cached_valid_[i] = false;
      }
      noise_pref_center_cache_dirty_ = true;
      noise_pref_center_cached_valid_ = false;
    }

    const bool noise_ui_knob =
        (knob_name == "show_noise") ||
        (knob_name.rfind("noise_", 0) == 0);
    if (noise_ui_knob) {
      update_noise_type_ui();
    }
    mask_count_ui_cached_ = mask_count_;
    node_redraw();
    const int base_result = Iop::knob_changed(k);
    if (noise_ui_knob) {
      // Ensure dynamic visibility uses the latest committed enum values.
      update_noise_type_ui();
    }
    return base_result;
  }

  bool updateUI(const OutputContext&) override {
    update_noise_type_ui();
    update_masks_ui();
    return true;
  }

  // ---------------------------------------------------------------------------
  // _validate
  // ---------------------------------------------------------------------------
  void _validate(bool for_real) override {
    if (!input(0)) {
      set_out_channels(Mask_None);
      return;
    }
    copy_info();
    sanitize_output_channels();
    sanitize_pref_channels();
    mask_count_ = clamp_compat(mask_count_, 1, kMaxMasks);
    mask_count_ui_cached_ = mask_count_;
    for (int i = 0; i < kMaxMasks; ++i) {
      mask_center_[i][0] = sanitize_finited(mask_center_[i][0], 0.0);
      mask_center_[i][1] = sanitize_finited(mask_center_[i][1], 0.0);
      mask_translate_[i][0] = sanitize_finited(mask_translate_[i][0], 0.0);
      mask_translate_[i][1] = sanitize_finited(mask_translate_[i][1], 0.0);
      mask_translate_[i][2] = sanitize_finited(mask_translate_[i][2], 0.0);
      mask_scale_[i][0] = std::max(0.001, sanitize_finited(mask_scale_[i][0], 1.0));
      mask_scale_[i][1] = std::max(0.001, sanitize_finited(mask_scale_[i][1], 1.0));
      mask_scale_[i][2] = std::max(0.001, sanitize_finited(mask_scale_[i][2], 1.0));
      mask_size_[i] = std::max(0.0, sanitize_finited(mask_size_[i], 0.0));
      mask_falloff_[i] = std::max(0.0, sanitize_finited(mask_falloff_[i], 0.0));
      mask_blur_[i] = std::max(0.0, sanitize_finited(mask_blur_[i], 0.0));
      mask_falloff_mode_[i] = clamp_compat(mask_falloff_mode_[i],
          static_cast<int>(FALLOFF_LINEAR), static_cast<int>(FALLOFF_SMOOTH));
      mask_shape_[i] = clamp_compat(mask_shape_[i],
          static_cast<int>(SHAPE_SPHERE), static_cast<int>(SHAPE_PLANE_Z));
    }
    global_size_ = std::max(0.0, sanitize_finited(global_size_, 1.0));
    global_falloff_ = std::max(0.0, sanitize_finited(global_falloff_, 1.0));
    mix_ = clamp_compat(mix_, 0.0, 1.0);
    sync_center_cache_from_knobs(mask_count_);

    const PrefRuntime pref_rt = resolve_pref_runtime();
    const bool use_pref = pref_rt.use_pref;

    // If pref availability changed, force a one-time repick for all mask slots.
    if (use_pref != pref_input_state_cached_) {
      Guard guard(pref_center_cache_lock_);
      for (int i = 0; i < kMaxMasks; ++i) {
        pref_center_cache_dirty_[i] = true;
        if (!use_pref) { pref_center_cached_valid_[i] = false; }
      }
      noise_pref_center_cache_dirty_ = true;
      if (!use_pref) { noise_pref_center_cached_valid_ = false; }
      pref_input_state_cached_ = use_pref;
    }

    // --- Build noise invmatrix (mirrors TNoise domainwarp_invmatrix_ build) ---
    constexpr double kPatternSizeScale = 40.0 / 350.0;
    constexpr double kPrefSizeBoost = 1000.0 / 350.0;
    constexpr double kPrefTo2DSizeCoeff = kPrefSizeBoost / 50.0;
    const double domain_size_scale = (noise_type_ == NOISE_PATTERN) ? kPatternSizeScale : 1.0;
    const double safe_dwx = std::max(0.000001, noise_xsize_ * domain_size_scale);
    const double safe_dwy = std::max(0.000001, noise_ysize_ * domain_size_scale);
    double dw_sx = safe_dwx * noise_scale_[0];
    double dw_sy = safe_dwy * noise_scale_[1];
    double dw_sz = 1.0;
    if (use_pref) {
      const double x_base = std::max(0.0, safe_dwx * kPrefTo2DSizeCoeff);
      const double y_base = std::max(0.0, safe_dwy * kPrefTo2DSizeCoeff);
      const double z_base = std::max(0.0, (safe_dwx + safe_dwy) * 0.5 * kPrefTo2DSizeCoeff);
      dw_sx = x_base * noise_scale_[0];
      dw_sy = y_base * noise_scale_[1];
      dw_sz = z_base * noise_scale_[2];
    }
    Matrix4 dw;
    dw.makeIdentity();
    const float rot_x = static_cast<float>(use_pref ? noise_rotate_[0] : noise_rotate2d_xy_[0]);
    const float rot_y = static_cast<float>(use_pref ? noise_rotate_[1] : noise_rotate2d_xy_[1]);
    const float rot_z = static_cast<float>(noise_rotate_[2]);
    dw.rotateZ(radians(rot_z));
    dw.rotateY(radians(rot_y));
    dw.rotateX(radians(rot_x));
    if (use_pref) {
      constexpr float kPrefSkewScale = 10.0f;
      dw.skewVec(Vector3(
          static_cast<float>(noise_skew_[0]) * kPrefSkewScale,
          static_cast<float>(noise_skew_[1]) * kPrefSkewScale,
          static_cast<float>(noise_skew_[2]) * kPrefSkewScale));
    } else {
      dw.skewXY(static_cast<float>(noise_skew_[0]),
                static_cast<float>(noise_skew_[1]));
    }
    dw.scale(static_cast<float>(dw_sx), static_cast<float>(dw_sy), static_cast<float>(dw_sz));
    noise_uniform_ = false;
    const float dw_det = dw.determinant();
    if (!dw_det) {
      noise_uniform_ = true;
    } else {
      noise_invmatrix_ = dw.inverse(dw_det);
    }

    // --- Build cached octave tables ---
    noise_real_octaves_ = std::max(1, current_noise_octaves());
    build_fractal_octave_tables(
        noise_fractal_mode_, noise_real_octaves_,
        std::max(1e-4f, static_cast<float>(noise_lacunarity_)),
        std::max(0.0f,  static_cast<float>(noise_gain_)),
        noise_octave_freqs_, noise_octave_amps_, noise_octave_amp_sum_);

    ChannelSet output_set;
    output_set += output_channels_[0];
    output_set += output_channels_[1];
    output_set += output_channels_[2];
    output_set += output_channels_[3];
    set_out_channels(output_set);
    info_.turn_on(output_set);
    (void)for_real;
  }

  // ---------------------------------------------------------------------------
  // _request
  // ---------------------------------------------------------------------------
  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override {
    (void)channels;
    if (!input(0)) { return; }
    const int active_masks = clamp_compat(mask_count_, 1, kMaxMasks);
    const PrefRuntime pref_rt = resolve_pref_runtime();
    ChannelSet source_request;
    source_request += output_channels_[0];
    source_request += output_channels_[1];
    source_request += output_channels_[2];
    input0().request(x, y, r, t, source_request, count);
    if (pref_rt.use_pref) {
      ChannelSet pref_request;
      pref_request += pref_rt.pref_r;
      pref_request += pref_rt.pref_g;
      pref_request += pref_rt.pref_b;
      if (pref_keep_alpha_ && pref_rt.pref_a &&
          static_cast<bool>(ChannelSet(pref_rt.pref_a) & input0().channels())) {
        pref_request += pref_rt.pref_a;
      }
      input0().request(x, y, r, t, pref_request, count);
      const Format& pf = input0().format();
      std::array<bool, kMaxMasks> request_mask_center = {};
      bool request_noise_center = false;
      bool need_pref_center_points = false;
      {
        Guard guard(pref_center_cache_lock_);
        for (int i = 0; i < active_masks; ++i) {
          request_mask_center[i] = pref_center_cache_dirty_[i];
          need_pref_center_points = need_pref_center_points || request_mask_center[i];
        }
        request_noise_center = noise_pref_center_cache_dirty_;
        need_pref_center_points = need_pref_center_points || request_noise_center;
      }

      if (need_pref_center_points && pf.r() > pf.x() && pf.t() > pf.y()) {
        auto request_pref_point = [&](double center_x, double center_y) {
          const int cx = clamp_compat(
              static_cast<int>(std::floor(center_x)), pf.x(), pf.r() - 1);
          const int cy = clamp_compat(
              static_cast<int>(std::floor(center_y)), pf.y(), pf.t() - 1);
          input0().request(cx, cy, cx + 1, cy + 1, pref_request, count);
        };

        for (int i = 0; i < active_masks; ++i) {
          if (!request_mask_center[i]) { continue; }
          request_pref_point(mask_center_[i][0], mask_center_[i][1]);
        }
        if (request_noise_center) {
          request_pref_point(noise_center_[0], noise_center_[1]);
        }
      }
    }
    if (has_warp_input_runtime()) {
      input(1)->request(x, y, r, t, Mask_RGBA, count);
    }
    if (has_mask_input_runtime()) {
      input(2)->request(x, y, r, t, Mask_RGBA, count);
    }
  }

  // ---------------------------------------------------------------------------
  // engine
  // ---------------------------------------------------------------------------
  void engine(int y, int x, int r, ChannelMask channels, Row& outrow) override {
    if (!input(0) || aborted()) { return; }
    const int active_masks = clamp_compat(mask_count_, 1, kMaxMasks);
    const bool use_warp = has_warp_input_runtime();
    const bool use_mask = has_mask_input_runtime();
    Iop* warp_iop = nullptr;
    if (use_warp) {
      Op* warp_op = input(1);
      if (warp_op) { warp_iop = warp_op->iop(); }
    }
    Iop* mask_iop = nullptr;
    if (use_mask) {
      Op* mask_op = input(2);
      if (mask_op) { mask_iop = mask_op->iop(); }
    }
    const bool mask_has_alpha =
        (mask_iop != nullptr) && mask_iop->channels().contains(Chan_Alpha);

    const PrefRuntime pref_rt = resolve_pref_runtime();
    ChannelSet source_request;
    source_request += output_channels_[0];
    source_request += output_channels_[1];
    source_request += output_channels_[2];
    Row source_row(x, r);
    source_row.get(input0(), y, x, r, source_request);

    ChannelSet pref_request;
    if (pref_rt.use_pref) {
      pref_request += pref_rt.pref_r;
      pref_request += pref_rt.pref_g;
      pref_request += pref_rt.pref_b;
      if (pref_keep_alpha_ && pref_rt.pref_a &&
          static_cast<bool>(ChannelSet(pref_rt.pref_a) & input0().channels())) {
        pref_request += pref_rt.pref_a;
      }
    }
    Row pref_row(x, r);
    if (pref_rt.use_pref) {
      pref_row.get(input0(), y, x, r, pref_request);
    }
    Row warp_row(x, r);
    if (warp_iop) {
      warp_row.get(*warp_iop, y, x, r, Mask_RGBA);
    }
    Row mask_row(x, r);
    if (mask_iop) {
      mask_row.get(*mask_iop, y, x, r, Mask_RGBA);
    }

    float* out_channels[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 4; ++i) {
      const Channel out_c = output_channels_[i];
      if (out_c && (channels & out_c)) {
        out_channels[i] = outrow.writable(out_c);
      }
    }

    // Resolve locked pref centers per mask slot.
    float locked_center_pref[kMaxMasks][3] = {};
    bool locked_center_valid[kMaxMasks] = {};
    float locked_noise_center_pref[3] = {0.0f, 0.0f, 0.0f};
    bool locked_noise_center_valid = false;
    if (pref_rt.use_pref) {
      Guard guard(pref_center_cache_lock_);
      for (int i = 0; i < active_masks; ++i) {
        if (pref_center_cache_dirty_[i]) {
          float sampled[3] = {0.0f, 0.0f, 0.0f};
          const bool valid =
              sample_pref_center_at(pref_rt, mask_center_[i][0], mask_center_[i][1], sampled);
          if (valid) {
            pref_center_cached_[i][0] = sampled[0];
            pref_center_cached_[i][1] = sampled[1];
            pref_center_cached_[i][2] = sampled[2];
            pref_center_cached_valid_[i] = true;
          }
          pref_center_cache_dirty_[i] = false;
        }
        locked_center_pref[i][0] = pref_center_cached_[i][0];
        locked_center_pref[i][1] = pref_center_cached_[i][1];
        locked_center_pref[i][2] = pref_center_cached_[i][2];
        locked_center_valid[i] = pref_center_cached_valid_[i];
      }

      if (noise_pref_center_cache_dirty_) {
        float sampled[3] = {0.0f, 0.0f, 0.0f};
        const bool valid =
            sample_pref_center_at(pref_rt, noise_center_[0], noise_center_[1], sampled);
        if (valid) {
          noise_pref_center_cached_[0] = sampled[0];
          noise_pref_center_cached_[1] = sampled[1];
          noise_pref_center_cached_[2] = sampled[2];
          noise_pref_center_cached_valid_ = true;
        }
        noise_pref_center_cache_dirty_ = false;
      }
      locked_noise_center_pref[0] = noise_pref_center_cached_[0];
      locked_noise_center_pref[1] = noise_pref_center_cached_[1];
      locked_noise_center_pref[2] = noise_pref_center_cached_[2];
      locked_noise_center_valid = noise_pref_center_cached_valid_;
    }

    const bool show_noise = show_noise_;
    const int noise_type  = noise_type_;
    const float frame = static_cast<float>(outputContext().frame());
    const float noise_seed = animated_seed_from_z_params(
        noise_zoffset_, noise_zspeed_, frame, noise_type, true);
    const float warp_amount =
        domainwarp_amount_scale_from_knob(noise_amount_) * (pref_rt.use_pref ? 0.02f : 1.0f);
    const bool needs_warp_vector = (warp_amount > 0.0f);
    const float translate_speed_scale = pref_rt.use_pref ? 0.02f : 1.0f;
    const float translate_effect_scale = 0.5f;

    float noise_at_tx = 0.0f, noise_at_ty = 0.0f;
    animated_translate_from_frame(
        static_cast<float>(noise_translate_speed_) * translate_speed_scale,
        static_cast<float>(noise_translate_angle_),
        frame, noise_at_tx, noise_at_ty, translate_effect_scale);
    const float global_size = static_cast<float>(global_size_);
    const float global_falloff = static_cast<float>(global_falloff_);
    const float mix_value = clamp01(static_cast<float>(mix_));

    for (int xx = x; xx < r; ++xx) {
      if (aborted()) { return; }

      float px = static_cast<float>(xx) + 0.5f;
      float py = static_cast<float>(y) + 0.5f;
      float pz = 0.0f;
      float alpha_mask = 1.0f;
      float input_mask = 1.0f;

      if (pref_rt.use_pref) {
        const float* pref_r = pref_row[pref_rt.pref_r];
        const float* pref_g = pref_row[pref_rt.pref_g];
        const float* pref_b = pref_row[pref_rt.pref_b];
        const bool pref_point_valid =
            (pref_r && pref_g && pref_b &&
             is_finitef(pref_r[xx]) &&
             is_finitef(pref_g[xx]) &&
             is_finitef(pref_b[xx]));
        if (pref_point_valid) {
          px = pref_r[xx];
          py = pref_g[xx];
          pz = pref_b[xx];
        } else {
          // Invalid P pixels (NaN/Inf/missing) must not generate random white spikes.
          px = static_cast<float>(xx) + 0.5f;
          py = static_cast<float>(y) + 0.5f;
          pz = 0.0f;
          alpha_mask = 0.0f;
        }
        if (pref_keep_alpha_ && pref_rt.pref_a) {
          const float* alpha_row = pref_row[pref_rt.pref_a];
          if (alpha_row) { alpha_mask = clamp01(alpha_row[xx]); }
        }
      }
      if (mask_iop) {
        const float* mask_a = mask_row[Chan_Alpha];
        const float* mask_r = mask_row[Chan_Red];
        if (mask_has_alpha && mask_a) {
          input_mask = clamp01(mask_a[xx]);
        } else if (mask_r) {
          input_mask = clamp01(mask_r[xx]);
        } else {
          input_mask = 0.0f;
        }
      }

      float warped_px = px, warped_py = py, warped_pz = pz;
      NoiseRGB noise_map = {0.0f, 0.0f, 0.0f};
      if (show_noise || needs_warp_vector) {
        if (warp_iop) {
          const float* wr = warp_row[Chan_Red];
          const float* wg = warp_row[Chan_Green];
          const float* wb = warp_row[Chan_Blue];
          noise_map.r = wr ? clamp_compat(sanitize_finite(wr[xx], 0.0f), -1.0f, 1.0f) : 0.0f;
          noise_map.g = wg ? clamp_compat(sanitize_finite(wg[xx], 0.0f), -1.0f, 1.0f) : 0.0f;
          noise_map.b = (pref_rt.use_pref && wb)
                            ? clamp_compat(sanitize_finite(wb[xx], 0.0f), -1.0f, 1.0f)
                            : 0.0f;
        } else {
          noise_map = evaluate_domainwarp_unit_vector(
              px, py, pz, pref_rt.use_pref, noise_seed, noise_at_tx, noise_at_ty,
              locked_noise_center_pref, locked_noise_center_valid);
        }
        noise_map.r = sanitize_finite(noise_map.r, 0.0f);
        noise_map.g = sanitize_finite(noise_map.g, 0.0f);
        noise_map.b = sanitize_finite(noise_map.b, 0.0f);
        if (needs_warp_vector) {
          warped_px += noise_map.r * warp_amount;
          warped_py += noise_map.g * warp_amount;
          warped_pz += noise_map.b * warp_amount;
        }
      }

      float combined_value = 0.0f;
      for (int i = 0; i < active_masks; ++i) {
        float cx = static_cast<float>(mask_center_[i][0]);
        float cy = static_cast<float>(mask_center_[i][1]);
        float cz = 0.0f;
        if (pref_rt.use_pref && locked_center_valid[i]) {
          cx = locked_center_pref[i][0];
          cy = locked_center_pref[i][1];
          cz = locked_center_pref[i][2];
        }
        cx += static_cast<float>(mask_translate_[i][0]);
        cy += static_cast<float>(mask_translate_[i][1]);
        cz += static_cast<float>(mask_translate_[i][2]);

        const float scale_x = static_cast<float>(std::max(0.001, mask_scale_[i][0]));
        const float scale_y = static_cast<float>(std::max(0.001, mask_scale_[i][1]));
        const float scale_z = static_cast<float>(std::max(0.001, mask_scale_[i][2]));
        const float size = static_cast<float>(std::max(0.0, mask_size_[i])) *
                           0.5f * std::max(0.0f, global_size);
        const float falloff = static_cast<float>(std::max(0.0, mask_falloff_[i])) *
                              std::max(0.0f, global_falloff);
        const float blur = static_cast<float>(std::max(0.0, mask_blur_[i])) *
                           std::max(0.0f, global_falloff);
        const float effective_falloff = falloff + blur;
        const int falloff_mode = mask_falloff_mode_[i];
        const int shape_mode = mask_shape_[i];
        const bool mirror_x = mask_mirror_x_[i];
        const bool mirror_y = mask_mirror_y_[i];
        const bool mirror_z = mask_mirror_z_[i];

        const int x_variants = mirror_x ? 2 : 1;
        const int y_variants = mirror_y ? 2 : 1;
        const int z_variants = mirror_z ? 2 : 1;

        for (int vx = 0; vx < x_variants; ++vx) {
          const float mcx = (vx == 0) ? cx : -cx;
          for (int vy = 0; vy < y_variants; ++vy) {
            const float mcy = (vy == 0) ? cy : -cy;
            for (int vz = 0; vz < z_variants; ++vz) {
              const float mcz = (vz == 0) ? cz : -cz;
              const float dx = (warped_px - mcx) / scale_x;
              const float dy = (warped_py - mcy) / scale_y;
              const float dz = (warped_pz - mcz) / scale_z;
              const float distance = shape_distance(shape_mode, dx, dy, dz);
              if (!is_finitef(distance)) {
                continue;
              }

              float local_value = 0.0f;
              if (effective_falloff <= 0.0f) {
                local_value = (distance <= size) ? 1.0f : 0.0f;
              } else if (distance <= size) {
                local_value = 1.0f;
              } else {
                const float t = clamp01((distance - size) / effective_falloff);
                if (falloff_mode == FALLOFF_SMOOTH) {
                  const float smooth = t * t * (3.0f - 2.0f * t);
                  local_value = clamp01(1.0f - smooth);
                } else {
                  local_value = clamp01(1.0f - t);
                }
              }
              combined_value = std::max(combined_value, local_value);
            }
          }
        }
      }

      if (invert_mask_) { combined_value = 1.0f - combined_value; }
      combined_value = clamp01(combined_value * mix_value);
      const float value = clamp01(combined_value * alpha_mask * input_mask);

      float src_rgb[3] = {0.0f, 0.0f, 0.0f};
      for (int i = 0; i < 3; ++i) {
        const Channel c = output_channels_[i];
        if (!c) { continue; }
        const float* row = source_row[c];
        if (row) { src_rgb[i] = sanitize_finite(row[xx], 0.0f); }
      }

      if (out_channels[0]) {
        out_channels[0][xx] = show_noise ? noise_map.r : clamp01(src_rgb[0] + value);
      }
      if (out_channels[1]) {
        out_channels[1][xx] = show_noise ? noise_map.g : src_rgb[1];
      }
      if (out_channels[2]) {
        out_channels[2][xx] = show_noise ? noise_map.b : src_rgb[2];
      }
      if (out_channels[3]) {
        out_channels[3][xx] = show_noise ? output_alpha_from_vector(noise_map) : value;
      }
    }
  }

  void append(Hash& hash) override {
    Iop::append(hash);
    // Only tell Nuke the output varies per frame when there's an actual
    // time-dependent component. Otherwise the hash is fully determined by
    // knob state, letting Nuke reuse cache across frames. Forcing a
    // frame-varying hash unconditionally causes aggressive re-evaluation
    // and, in scenes with many TMask instances, exposes concurrent-sampling
    // edge cases that manifest as viewer flicker.
    if (noise_zspeed_ > 0.0 || noise_translate_speed_ > 0.0) {
      hash.append(outputContext().frame());
#if defined(kDDImageVersionMajorNum) && (kDDImageVersionMajorNum >= 14)
      enableVaryingOutputHash();
#endif
    }
  }

 private:
  // ---------------------------------------------------------------------------
  // PRef
  // ---------------------------------------------------------------------------
  struct PrefRuntime {
    bool use_pref = false;
    Channel pref_r = Chan_Red;
    Channel pref_g = Chan_Green;
    Channel pref_b = Chan_Blue;
    Channel pref_a = Chan_Alpha;
  };

  // ---------------------------------------------------------------------------
  // Member variables
  // ---------------------------------------------------------------------------
  Channel output_channels_[4] = {Chan_Red, Chan_Green, Chan_Blue, Chan_Alpha};
  Channel pref_channels_[4]   = {Chan_Red, Chan_Green, Chan_Blue, Chan_Alpha};
  bool pref_keep_alpha_ = true;

  // Mask
  int    mask_count_ = 1;
  double mask_center_[kMaxMasks][2] = {};
  double mask_translate_[kMaxMasks][3] = {};
  double mask_scale_[kMaxMasks][3] = {};
  double mask_size_[kMaxMasks] = {};
  double mask_falloff_[kMaxMasks] = {};
  double mask_blur_[kMaxMasks] = {};
  int    mask_falloff_mode_[kMaxMasks] = {};
  int    mask_shape_[kMaxMasks] = {};
  bool   mask_mirror_x_[kMaxMasks] = {};
  bool   mask_mirror_y_[kMaxMasks] = {};
  bool   mask_mirror_z_[kMaxMasks] = {};
  bool   mask_slot_initialized_[kMaxMasks] = {};
  double global_size_ = 1.0;
  double global_falloff_ = 1.0;
  double mix_ = 1.0;
  bool   invert_mask_ = false;

  // Domain Warp â€” output
  double noise_amount_               = 0.0;
  bool   show_noise_                 = false;

  // Domain Warp â€” noise params
  int    noise_type_                = NOISE_PERLIN;
  int    noise_fractal_mode_        = FRACTAL_FBM;
  double noise_xsize_               = 350.0;
  double noise_ysize_               = 350.0;
  double noise_zoffset_             = 0.0;
  double noise_zspeed_              = 0.0;
  int    noise_octaves_             = 10;
  int    noise_voronoi_octaves_     = 1;
  int    noise_pattern_octaves_     = 1;
  double noise_lacunarity_          = 2.0;
  double noise_gain_                = 0.5;
  float  noise_gamma_               = 0.5f;
  int    noise_voronoi_metric_      = VORONOI_EUCLIDEAN;
  int    noise_voronoi_shape_mode_  = VORONOI_SHAPE_VORONOI;
  double noise_voronoi_minkowski_exp_ = 1.0;
  double noise_voronoi_randomness_    = 1.0;
  int    noise_pattern_type_mode_   = PATTERN_CONCENTRIC;
  int    noise_pattern_shape_mode_  = PATTERN_SHAPE_ROUND;
  int    noise_pattern_segment_count_ = 10;
  int    noise_pattern_twist_       = 0;
  double noise_divergence_mix_      = 0.0;

  // Domain Warp â€” transform
  double noise_translate_[3]   = {0.0, 0.0, 0.0};
  double noise_scale_[3]       = {1.0, 1.0, 1.0};
  double noise_skew_[3]        = {0.0, 0.0, 0.0};
  double noise_rotate_[3]      = {0.0, 0.0, 0.0};
  double noise_center_[2]      = {0.0, 0.0};
  double noise_rotate2d_xy_[2] = {0.0, 0.0};
  double noise_translate_speed_ = 0.0;
  double noise_translate_angle_ = 0.0;

  // Computed in _validate
  Matrix4 noise_invmatrix_;
  bool    noise_uniform_      = false;
  int     noise_real_octaves_ = 10;
  std::vector<float> noise_octave_freqs_;
  std::vector<float> noise_octave_amps_;
  float              noise_octave_amp_sum_ = 1.0f;

  // Pref center cache
  float pref_center_cached_[kMaxMasks][3] = {};
  bool  pref_center_cached_valid_[kMaxMasks] = {};
  bool  pref_center_cache_dirty_[kMaxMasks] = {};
  double mask_center_tracker_[kMaxMasks][2] = {};
  bool   mask_center_tracker_valid_[kMaxMasks] = {};
  float noise_pref_center_cached_[3] = {0.0f, 0.0f, 0.0f};
  bool  noise_pref_center_cached_valid_ = false;
  bool  noise_pref_center_cache_dirty_ = true;
  double noise_center_tracker_[2] = {0.0, 0.0};
  bool   noise_center_tracker_valid_ = false;
  int   mask_count_ui_cached_ = 1;
  bool  pref_input_state_cached_  = false;
  bool  ui_panel_opened_ = false;
  bool  ui_knob_sync_in_progress_ = false;
  Lock  pref_center_cache_lock_;

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------
  struct ScopedUiSyncGuard {
    explicit ScopedUiSyncGuard(bool& in_flag) : flag(in_flag) { flag = true; }
    ~ScopedUiSyncGuard() { flag = false; }
    bool& flag;
  };

  int current_noise_octaves() const {
    if (noise_type_ == NOISE_VORONOI) { return std::max(1, noise_voronoi_octaves_); }
    if (noise_type_ == NOISE_PATTERN) { return std::max(1, noise_pattern_octaves_); }
    return std::max(1, noise_octaves_);
  }

  void sync_center_cache_from_knobs(int active_masks) {
    constexpr double kCenterEpsilon = 1e-6;
    const int clamped_masks = clamp_compat(active_masks, 1, kMaxMasks);
    auto changed = [&](double a, double b) {
      return std::fabs(a - b) > kCenterEpsilon;
    };

    Guard guard(pref_center_cache_lock_);
    for (int i = 0; i < clamped_masks; ++i) {
      const double cx = mask_center_[i][0];
      const double cy = mask_center_[i][1];
      if (!mask_center_tracker_valid_[i] ||
          changed(mask_center_tracker_[i][0], cx) ||
          changed(mask_center_tracker_[i][1], cy)) {
        pref_center_cache_dirty_[i] = true;
        mask_center_tracker_[i][0] = cx;
        mask_center_tracker_[i][1] = cy;
        mask_center_tracker_valid_[i] = true;
      }
    }
    for (int i = clamped_masks; i < kMaxMasks; ++i) {
      mask_center_tracker_valid_[i] = false;
    }

    const double ncx = noise_center_[0];
    const double ncy = noise_center_[1];
    if (!noise_center_tracker_valid_ ||
        changed(noise_center_tracker_[0], ncx) ||
        changed(noise_center_tracker_[1], ncy)) {
      noise_pref_center_cache_dirty_ = true;
      noise_center_tracker_[0] = ncx;
      noise_center_tracker_[1] = ncy;
      noise_center_tracker_valid_ = true;
    }
  }

  // Returns the transformed noise-space point.
  // at_tx/at_ty: animated translate contribution from per-frame computation.
  NoiseRGB noise_point_from_input(float sx, float sy, float sz, bool pref_space,
                                  const float pref_center[3], bool pref_center_valid,
                                  float at_tx = 0.0f, float at_ty = 0.0f) const {
    if (noise_uniform_) { return {0.0f, 0.0f, 0.0f}; }
    const float translate_x = static_cast<float>(noise_translate_[0]) + at_tx;
    const float translate_y = static_cast<float>(noise_translate_[1]) + at_ty;
    const float translate_z = static_cast<float>(noise_translate_[2]);
    const float pref_center_x =
        (pref_space && pref_center_valid && pref_center) ? pref_center[0] : 0.0f;
    const float pref_center_y =
        (pref_space && pref_center_valid && pref_center) ? pref_center[1] : 0.0f;
    const float pref_center_z =
        (pref_space && pref_center_valid && pref_center) ? pref_center[2] : 0.0f;
    const float local_x = pref_space
        ? (sx - pref_center_x - translate_x)
        : (sx - static_cast<float>(noise_center_[0]) - translate_x);
    const float local_y = pref_space
        ? (sy - pref_center_y - translate_y)
        : (sy - static_cast<float>(noise_center_[1]) - translate_y);
    const float local_z = pref_space ? (sz - pref_center_z - translate_z) : sz;
    const Vector3 result = noise_invmatrix_.transform(Vector3(local_x, local_y, local_z));
    return {result.x, result.y, pref_space ? result.z : 0.0f};
  }

  // Evaluates the fractalized scalar using pre-built octave tables.
  float evaluate_noise_scalar_signed(float x, float y, float z, float time_seed,
                                     bool use_time, float phase_shift) const {
    const float px = phase_shift;
    const float py = phase_shift * 0.5f;
    const float pz = -phase_shift * 0.5f;
    const float pw = phase_shift;
    if (!noise_octave_freqs_.empty()) {
      return fractalized_value_with_tables(
          noise_type_, noise_fractal_mode_,
          noise_real_octaves_,
          noise_octave_freqs_.data(), noise_octave_amps_.data(), noise_octave_amp_sum_,
          x + px, y + py, z + pz, time_seed + pw, use_time,
          noise_voronoi_metric_, noise_voronoi_shape_mode_,
          static_cast<float>(noise_voronoi_randomness_),
          static_cast<float>(noise_voronoi_minkowski_exp_),
          noise_pattern_type_mode_, noise_pattern_segment_count_, noise_pattern_twist_);
    }
    return fractalized_value_for_settings(
        noise_type_, noise_fractal_mode_,
        current_noise_octaves(), noise_lacunarity_, noise_gain_,
        x + px, y + py, z + pz, time_seed + pw, use_time,
        noise_voronoi_metric_, noise_voronoi_shape_mode_,
        static_cast<float>(noise_voronoi_randomness_),
        static_cast<float>(noise_voronoi_minkowski_exp_),
        noise_pattern_type_mode_, noise_pattern_segment_count_, noise_pattern_twist_);
  }

  // Evaluate the warp unit vector using the default TNoise field behavior.
  NoiseRGB evaluate_domainwarp_unit_vector(float sx, float sy, float sz,
                                           bool pref_space, float time_seed,
                                           float at_tx, float at_ty,
                                           const float pref_center[3],
                                           bool pref_center_valid) const {
    const NoiseRGB p = noise_point_from_input(
        sx, sy, sz, pref_space, pref_center, pref_center_valid, at_tx, at_ty);
    if (noise_type_ == NOISE_VORONOI &&
        noise_voronoi_shape_mode_ == VORONOI_SHAPE_VORONOI) {
      return evaluate_voronoi_shape_vector_from_point(
          p.r, p.g, p.b, time_seed, true, pref_space,
          noise_voronoi_metric_,
          static_cast<float>(noise_voronoi_randomness_),
          static_cast<float>(noise_voronoi_minkowski_exp_),
          noise_fractal_mode_, current_noise_octaves(),
          noise_lacunarity_, noise_gain_,
          50.0f);
    }
    if (noise_type_ == NOISE_PATTERN) {
      return evaluate_pattern_vector_from_point_for_mode(
          p.r, p.g, p.b, time_seed, true, pref_space,
          noise_pattern_type_mode_, noise_pattern_segment_count_, noise_pattern_twist_,
          noise_fractal_mode_, current_noise_octaves(),
          noise_lacunarity_, noise_gain_);
    }
    const float rx = evaluate_noise_scalar_signed(p.r, p.g, p.b, time_seed, true, 0.0f);
    const float gy = evaluate_noise_scalar_signed(p.r, p.g, p.b, time_seed, true, 50.0f);
    const float bz = pref_space
        ? evaluate_noise_scalar_signed(p.r, p.g, p.b, time_seed, true, -50.0f)
        : 0.0f;
    return {rx, gy, bz};
  }

  float output_alpha_from_vector(const NoiseRGB& n) const {
    return std::max(clamp01(n.r), std::max(clamp01(n.g), clamp01(n.b)));
  }

  // ---------------------------------------------------------------------------
  // UI visibility
  // ---------------------------------------------------------------------------
  void set_knob_visibility(const char* name, bool visible) {
    Knob* k = knob(name);
    if (!k) { return; }
    k->visible(visible);
    k->updateWidgets();
  }

  bool find_mask_center_knob_index(const std::string& knob_name,
                                   int& out_index) const {
    out_index = -1;
    for (int i = 0; i < kMaxMasks; ++i) {
      const std::string base = "mask_" + std::to_string(i + 1) + "_center";
      if (knob_name.rfind(base, 0) == 0) {
        out_index = i;
        return true;
      }
    }
    return false;
  }

  void reset_mask_slot(int i) {
    if (i < 0 || i >= kMaxMasks) { return; }
    mask_center_[i][0] = 0.0;
    mask_center_[i][1] = 0.0;
    mask_translate_[i][0] = 0.0;
    mask_translate_[i][1] = 0.0;
    mask_translate_[i][2] = 0.0;
    mask_scale_[i][0] = 1.0;
    mask_scale_[i][1] = 1.0;
    mask_scale_[i][2] = 1.0;
    mask_size_[i] = 5.0;
    mask_falloff_[i] = 3.0;
    mask_blur_[i] = 0.0;
    mask_falloff_mode_[i] = FALLOFF_LINEAR;
    mask_shape_[i] = SHAPE_SPHERE;
    mask_mirror_x_[i] = false;
    mask_mirror_y_[i] = false;
    mask_mirror_z_[i] = false;
    pref_center_cached_[i][0] = 0.0f;
    pref_center_cached_[i][1] = 0.0f;
    pref_center_cached_[i][2] = 0.0f;
    pref_center_cached_valid_[i] = false;
    pref_center_cache_dirty_[i] = true;
    mask_center_tracker_[i][0] = 0.0;
    mask_center_tracker_[i][1] = 0.0;
    mask_center_tracker_valid_[i] = false;
    mask_slot_initialized_[i] = false;
  }

  void apply_mask_slot_knobs(int i) {
    if (i < 0 || i >= kMaxMasks) { return; }
    const std::string idx = std::to_string(i + 1);
    auto set2 = [&](const std::string& name, double a, double b) {
      if (Knob* k = knob(name.c_str())) {
        k->set_value(a, 0);
        k->set_value(b, 1);
      }
    };
    auto set3 = [&](const std::string& name, double a, double b, double c) {
      if (Knob* k = knob(name.c_str())) {
        k->set_value(a, 0);
        k->set_value(b, 1);
        k->set_value(c, 2);
      }
    };
    auto set1 = [&](const std::string& name, double v) {
      if (Knob* k = knob(name.c_str())) { k->set_value(v); }
    };

    set2("mask_" + idx + "_center", mask_center_[i][0], mask_center_[i][1]);
    set3("mask_" + idx + "_translate",
         mask_translate_[i][0], mask_translate_[i][1], mask_translate_[i][2]);
    set3("mask_" + idx + "_scale",
         mask_scale_[i][0], mask_scale_[i][1], mask_scale_[i][2]);
    set1("mask_" + idx + "_size", mask_size_[i]);
    set1("mask_" + idx + "_falloff", mask_falloff_[i]);
    set1("mask_" + idx + "_blur", mask_blur_[i]);
    set1("mask_" + idx + "_falloff_mode", static_cast<double>(mask_falloff_mode_[i]));
    set1("mask_" + idx + "_shape", static_cast<double>(mask_shape_[i]));
    set1("mask_" + idx + "_mirror_x", mask_mirror_x_[i] ? 1.0 : 0.0);
    set1("mask_" + idx + "_mirror_y", mask_mirror_y_[i] ? 1.0 : 0.0);
    set1("mask_" + idx + "_mirror_z", mask_mirror_z_[i] ? 1.0 : 0.0);
  }

  bool mask_slot_is_at_defaults(int i, bool ignore_center) const {
    if (i < 0 || i >= kMaxMasks) { return true; }
    constexpr double eps = 1e-6;
    auto near0 = [&](double v) { return std::fabs(v) <= eps; };
    auto nearv = [&](double a, double b) { return std::fabs(a - b) <= eps; };
    if (!ignore_center) {
      if (!near0(mask_center_[i][0]) || !near0(mask_center_[i][1])) { return false; }
    }
    if (!near0(mask_translate_[i][0]) || !near0(mask_translate_[i][1]) || !near0(mask_translate_[i][2])) { return false; }
    if (!nearv(mask_scale_[i][0], 1.0) || !nearv(mask_scale_[i][1], 1.0) || !nearv(mask_scale_[i][2], 1.0)) { return false; }
    if (!nearv(mask_size_[i], 5.0) || !nearv(mask_falloff_[i], 3.0)) { return false; }
    if (!near0(mask_blur_[i])) { return false; }
    if (mask_falloff_mode_[i] != FALLOFF_LINEAR) { return false; }
    if (mask_shape_[i] != SHAPE_SPHERE) { return false; }
    if (mask_mirror_x_[i] || mask_mirror_y_[i] || mask_mirror_z_[i]) { return false; }
    return true;
  }

  void default_mask_center_for_new_slot(double& out_x, double& out_y) const {
    const Format& source_format = input(0) ? input0().format() : format();
    if (source_format.r() > source_format.x() && source_format.t() > source_format.y()) {
      out_x = 0.5 * (static_cast<double>(source_format.x()) + static_cast<double>(source_format.r()));
      out_y = 0.5 * (static_cast<double>(source_format.y()) + static_cast<double>(source_format.t()));
      return;
    }
    out_x = 0.0;
    out_y = 0.0;
  }

  void initialize_mask_slot_defaults(int i, bool center_from_format) {
    if (i < 0 || i >= kMaxMasks) { return; }
    double center_x = 0.0;
    double center_y = 0.0;
    if (center_from_format) {
      default_mask_center_for_new_slot(center_x, center_y);
    }
    mask_center_[i][0] = center_x;
    mask_center_[i][1] = center_y;
    mask_translate_[i][0] = 0.0;
    mask_translate_[i][1] = 0.0;
    mask_translate_[i][2] = 0.0;
    mask_scale_[i][0] = 1.0;
    mask_scale_[i][1] = 1.0;
    mask_scale_[i][2] = 1.0;
    mask_size_[i] = 5.0;
    mask_falloff_[i] = 3.0;
    mask_blur_[i] = 0.0;
    mask_falloff_mode_[i] = FALLOFF_LINEAR;
    mask_shape_[i] = SHAPE_SPHERE;
    mask_mirror_x_[i] = false;
    mask_mirror_y_[i] = false;
    mask_mirror_z_[i] = false;
    pref_center_cached_[i][0] = 0.0f;
    pref_center_cached_[i][1] = 0.0f;
    pref_center_cached_[i][2] = 0.0f;
    pref_center_cached_valid_[i] = false;
    pref_center_cache_dirty_[i] = true;
    mask_center_tracker_[i][0] = mask_center_[i][0];
    mask_center_tracker_[i][1] = mask_center_[i][1];
    mask_center_tracker_valid_[i] = true;
    mask_slot_initialized_[i] = true;
  }

  void update_masks_ui() {
    mask_count_ = clamp_compat(mask_count_, 1, kMaxMasks);
    for (int i = 0; i < kMaxMasks; ++i) {
      const std::string group_name = "mask_" + std::to_string(i + 1) + "_group";
      set_knob_visibility(group_name.c_str(), i < mask_count_);
      if (Knob* g = knob(group_name.c_str())) { g->updateWidgets(); }
    }
    if (Knob* k = knob("mask_add")) { k->enable(mask_count_ < kMaxMasks); }
    if (Knob* k = knob("mask_remove")) { k->enable(mask_count_ > 1); }
  }

  void update_noise_type_ui() {
    const bool voronoi_type = (noise_type_ == NOISE_VORONOI);
    const bool pattern_type = (noise_type_ == NOISE_PATTERN);
    set_knob_visibility("noise_octaves",         !voronoi_type && !pattern_type);
    set_knob_visibility("noise_octaves_voronoi",  voronoi_type);
    set_knob_visibility("noise_octaves_pattern",  pattern_type);

    set_knob_visibility("noise_voronoi_group",       voronoi_type);
    set_knob_visibility("noise_voronoi_shape_mode",  voronoi_type);
    set_knob_visibility("noise_voronoi_metric",      voronoi_type);
    const bool voronoi_minkowski = voronoi_type && (noise_voronoi_metric_ == VORONOI_MINKOWSKI);
    set_knob_visibility("noise_voronoi_minkowski_exp",     voronoi_minkowski);
    set_knob_visibility("noise_voronoi_minkowski_divider", voronoi_minkowski);
    set_knob_visibility("noise_voronoi_randomness",  voronoi_type);

    set_knob_visibility("noise_pattern_group",      pattern_type);
    set_knob_visibility("noise_pattern_type_mode",  pattern_type);
    set_knob_visibility("noise_pattern_shape_mode", false);
    const bool pattern_radial      = (noise_pattern_type_mode_ == PATTERN_RADIAL);
    const bool pattern_concentric  = (noise_pattern_type_mode_ == PATTERN_CONCENTRIC);
    set_knob_visibility("noise_pattern_shape_divider",
                        pattern_type && (pattern_radial || pattern_concentric));
    set_knob_visibility("noise_pattern_segment_count", pattern_type && pattern_radial);
    set_knob_visibility("noise_pattern_twist",         pattern_type && pattern_concentric);

    // divergence_mix and its divider are hidden (reserved for future use).
    set_knob_visibility("noise_divergence_mix",    false);
    set_knob_visibility("noise_divergence_divider", false);

    if (Knob* g = knob("noise_group"))          { g->updateWidgets(); }
    if (Knob* g = knob("noise_noise_group"))    { g->updateWidgets(); }
    if (Knob* g = knob("noise_voronoi_group"))  { g->updateWidgets(); }
    if (Knob* g = knob("noise_pattern_group"))  { g->updateWidgets(); }
    if (Knob* g = knob("noise_transform_group")){ g->updateWidgets(); }
  }

  // ---------------------------------------------------------------------------
  // Channel sanitization
  // ---------------------------------------------------------------------------
  void sanitize_output_channels() {
    if (!output_channels_[0]) { output_channels_[0] = Chan_Red; }
    if (!output_channels_[1]) { output_channels_[1] = Chan_Green; }
    if (!output_channels_[2]) { output_channels_[2] = Chan_Blue; }
    if (!output_channels_[3]) { output_channels_[3] = Chan_Alpha; }
  }
  void sanitize_pref_channels() {
    if (!pref_channels_[0]) { pref_channels_[0] = Chan_Red; }
    if (!pref_channels_[1]) { pref_channels_[1] = Chan_Green; }
    if (!pref_channels_[2]) { pref_channels_[2] = Chan_Blue; }
    if (!pref_channels_[3]) { pref_channels_[3] = Chan_Alpha; }
  }

  // ---------------------------------------------------------------------------
  // Pref center sampling
  // ---------------------------------------------------------------------------
  bool source_has_channel(const ChannelSet& source_channels, Channel c) const {
    return c && source_channels.contains(c);
  }

  bool resolve_pref_triplet_for_layer(const char* layer_name,
                                      const ChannelSet& source_channels,
                                      Channel& out_x,
                                      Channel& out_y,
                                      Channel& out_z) const {
    if (!layer_name || !*layer_name) { return false; }

    struct TrioNames {
      const char* a;
      const char* b;
      const char* c;
    };
    static const TrioNames kTriplets[] = {
        {"x", "y", "z"},
        {"X", "Y", "Z"},
        {"red", "green", "blue"},
        {"r", "g", "b"},
    };

    for (const TrioNames& t : kTriplets) {
      const std::string ca = std::string(layer_name) + "." + t.a;
      const std::string cb = std::string(layer_name) + "." + t.b;
      const std::string cc = std::string(layer_name) + "." + t.c;
      const Channel ch_a = channel(ca.c_str());
      const Channel ch_b = channel(cb.c_str());
      const Channel ch_c = channel(cc.c_str());
      if (source_has_channel(source_channels, ch_a) &&
          source_has_channel(source_channels, ch_b) &&
          source_has_channel(source_channels, ch_c)) {
        out_x = ch_a;
        out_y = ch_b;
        out_z = ch_c;
        return true;
      }
    }
    return false;
  }

  PrefRuntime resolve_pref_runtime() const {
    PrefRuntime out;
    if (!input(0)) { return out; }
    out.pref_r = pref_channels_[0] ? pref_channels_[0] : Chan_Red;
    out.pref_g = pref_channels_[1] ? pref_channels_[1] : Chan_Green;
    out.pref_b = pref_channels_[2] ? pref_channels_[2] : Chan_Blue;
    out.pref_a = pref_channels_[3] ? pref_channels_[3] : Chan_Alpha;
    const ChannelSet source_channels = input0().channels();

    auto has_triplet = [&](Channel cx, Channel cy, Channel cz) {
      return source_has_channel(source_channels, cx) &&
             source_has_channel(source_channels, cy) &&
             source_has_channel(source_channels, cz);
    };

    if (!has_triplet(out.pref_r, out.pref_g, out.pref_b)) {
      // Auto-detect common position layers used in comp-light setups.
      static const char* kPreferredLayers[] = {
          "Pref", "PRef", "P", "position", "Position",
      };
      Channel auto_x = out.pref_r;
      Channel auto_y = out.pref_g;
      Channel auto_z = out.pref_b;
      bool resolved = false;
      for (const char* layer : kPreferredLayers) {
        if (resolve_pref_triplet_for_layer(layer, source_channels, auto_x, auto_y, auto_z)) {
          resolved = true;
          break;
        }
      }
      if (resolved) {
        out.pref_r = auto_x;
        out.pref_g = auto_y;
        out.pref_b = auto_z;
      }
    }

    out.use_pref = has_triplet(out.pref_r, out.pref_g, out.pref_b);
    return out;
  }

  bool sample_pref_center_at(const PrefRuntime& pref_rt, double sx, double sy,
                              float out_center[3]) {
    out_center[0] = out_center[1] = out_center[2] = 0.0f;
    if (!pref_rt.use_pref || !input(0)) { return false; }
    const Format& pf = input0().format();
    if (!(pf.r() > pf.x() && pf.t() > pf.y())) { return false; }
    const int cx = clamp_compat(static_cast<int>(std::floor(sx)), pf.x(), pf.r() - 1);
    const int cy = clamp_compat(static_cast<int>(std::floor(sy)), pf.y(), pf.t() - 1);
    ChannelSet center_mask;
    center_mask += pref_rt.pref_r;
    center_mask += pref_rt.pref_g;
    center_mask += pref_rt.pref_b;
    Row center_row(cx, cx + 1);
    center_row.get(input0(), cy, cx, cx + 1, center_mask);
    const float* cr = center_row[pref_rt.pref_r];
    const float* cg = center_row[pref_rt.pref_g];
    const float* cb = center_row[pref_rt.pref_b];
    if (!cr || !cg || !cb) { return false; }
    const float svx = cr[cx];
    const float svy = cg[cx];
    const float svz = cb[cx];
    if (!is_finitef(svx) || !is_finitef(svy) || !is_finitef(svz)) { return false; }
    out_center[0] = svx;
    out_center[1] = svy;
    out_center[2] = svz;
    return true;
  }

 public:
  static const Description d;
};

static Op* build(Node* node) { return new TMaskBase(node); }
const Op::Description TMaskBase::d(::CLASS, "Filter/TMask", build);
extern "C" void tmask_base_keepalive() {}
